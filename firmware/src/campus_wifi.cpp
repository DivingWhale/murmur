#include "campus_wifi.h"
#include <Arduino.h>
#include "campus_profile.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <esp_wpa2.h>
#include <esp_system.h>
#include <lwip/tcpip.h>
#include <mbedtls/ssl.h>
#include <mbedtls/sha256.h>
#include <Preferences.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>

extern "C" const uint8_t murmur_campus_ca[], murmur_campus_ca_end[];
namespace {
constexpr char ssid[]="HKUSTGZ", radius[]="nce.hkust-gz.edu.cn";
enum class State { Off, Deferred, WaitingClock, Joining, ClockOnly, Online, Failed, MissingCard };
State state=State::Off;
CampusLogin pending;
bool savePending=false, saveFailed=false;
uint32_t started;
std::atomic<unsigned> reason{0}, certFlags{0};
std::atomic<uint32_t> lastNetworkTime{0};
std::atomic<bool> clockUpdated{false};
struct CampusPeer { uint8_t digest[32]; uint32_t checkedAt; };
CampusPeer expectedPeer{}, observedPeer{};
portMUX_TYPE peerLock=portMUX_INITIALIZER_UNLOCKED;
bool bootstrap=false;
bool bootstrappedClock=false;
unsigned cardAttempts=0;
unsigned networkRetries=0;
bool usingSavedLogin=false;
uint32_t retryAt=0;
std::atomic<bool> linkDown{true};

// Breadcrumbs that survive a watchdog reset, reported by `wifi diag`.
// Layout: magic (16 bits) | main-loop phase (8) | Wi-Fi step (8).
constexpr uint32_t stageMagic=0x4d520000; // "MR"
RTC_NOINIT_ATTR uint32_t rtcStage;
uint32_t stageBeforeReset;
void stage(uint8_t s) { rtcStage=stageMagic|(rtcStage&0xff00u)|s; }

// This SDK builds lwIP without core locking. SNTP must only be touched from
// the tcpip thread, otherwise its timers can corrupt and hang the stack.
void sntpStart(void *) {
    if (sntp_enabled()) sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0,(char*)"ntp.aliyun.com");
    sntp_setservername(1,(char*)"time.cloudflare.com");
    sntp_setservername(2,(char*)"cn.pool.ntp.org");
    sntp_init();
}
void sntpStop(void *) { if (sntp_enabled()) sntp_stop(); }
// Session lifetime. Automatic sessions (boot, wake, after a recording, Settings)
// switch the radio off once network time arrives or after a short cap. Manual
// sessions from the USB helper stay up until `wifi off`, with a longer cap.
constexpr uint32_t autoOnlineLimit=20000, autoAfterTime=2000, manualOnlineLimit=300000;
constexpr uint32_t wakeInterval=30*60000;
bool manualSession=false, everOnline=false;
// Automatic attempts back off after failed logins (for example a changed password),
// so wakes and recordings do not each spend up to 30 s of radio time: 2, 4, 8 ... 60 min.
unsigned failStreak=0;
uint32_t failedAt=0;
uint32_t backoffRemaining() {
    if (!failStreak) return 0;
    const uint32_t wait=std::min(60u,1u<<std::min(failStreak,6u))*60000u, since=millis()-failedAt;
    return since<wait?wait-since:0;
}
uint32_t onlineAt=0;
std::atomic<uint32_t> sessionTimeAt{0};
bool readPeer(CampusPeer &peer) {
    Preferences p;
    return p.begin("campus",true) && p.getBytesLength("peer")==sizeof(peer) &&
           p.getBytes("peer",&peer,sizeof(peer))==sizeof(peer) && peer.checkedAt>=1704067200;
}
void rememberVerifiedPeer() {
    CampusPeer peer{}, previous{};
    portENTER_CRITICAL(&peerLock);
    peer=observedPeer;
    portEXIT_CRITICAL(&peerLock);
    if (!peer.checkedAt || (readPeer(previous) && !memcmp(previous.digest,peer.digest,32) &&
        peer.checkedAt>=previous.checkedAt && peer.checkedAt-previous.checkedAt<86400)) return;
    Preferences p;
    if(p.begin("campus",false)) p.putBytes("peer",&peer,sizeof(peer));
}

std::array<int,6> date(const mbedtls_x509_time &t) {
    return {t.year,t.mon,t.day,t.hour,t.min,t.sec};
}
int checkDates(void *,mbedtls_x509_crt *cert,int depth,uint32_t *flags) {
    // The prebuilt SDK omits MBEDTLS_HAVE_TIME_DATE. Preserve all chain
    // and DNS errors from mbedTLS and check each certificate in UTC.
    CampusPeer expected;
    portENTER_CRITICAL(&peerLock);
    expected=expectedPeer;
    portEXIT_CRITICAL(&peerLock);
    time_t now=expected.checkedAt?expected.checkedAt:time(nullptr);
    struct tm t{};
    gmtime_r(&now,&t);
    std::array<int,6> utc={t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec};
    if (utc<date(cert->valid_from)) *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    if (utc>date(cert->valid_to)) *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    if (depth==0) {
        CampusPeer peer{};
        if (mbedtls_sha256_ret(cert->raw.p,cert->raw.len,peer.digest,0)!=0 ||
            (expected.checkedAt && memcmp(peer.digest,expected.digest,sizeof(peer.digest))))
            *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        if (!*flags) {
            peer.checkedAt=now;
            portENTER_CRITICAL(&peerLock);
            observedPeer=peer;
            portEXIT_CRITICAL(&peerLock);
        }
    }
    certFlags.fetch_or(*flags);
    return 0;
}
void stopRadio(State next) {
    if (next==State::Failed && (state==State::Joining || state==State::ClockOnly)) { ++failStreak; failedAt=millis(); }
    if (WiFi.getMode()!=WIFI_OFF) {
        stage(1);
        tcpip_callback(sntpStop,nullptr);
        // Let the supplicant finish with the link before removing its EAP setup.
        if (esp_wifi_disconnect()==ESP_OK)
            for (uint32_t t=millis(); !linkDown.load() && millis()-t<1000;) delay(10);
        stage(2);
        esp_wifi_sta_wpa2_ent_disable();
        esp_wifi_sta_wpa2_ent_clear_password();
        esp_wifi_sta_wpa2_ent_clear_username();
        esp_wifi_sta_wpa2_ent_clear_identity();
        esp_wifi_sta_wpa2_ent_clear_ca_cert();
        stage(4);
        WiFi.mode(WIFI_OFF);
        stage(0);
    }
    state=next;
}
bool join(const char *user,const char *pass,const CampusPeer *pinned=nullptr) {
    size_t u=strlen(user),p=strlen(pass);
    if (!u||u>128||!p||p>128 || (!pinned && time(nullptr)<1704067200)) return false;
    stopRadio(State::Off);
    bootstrap=pinned!=nullptr;
    portENTER_CRITICAL(&peerLock);
    expectedPeer=pinned?*pinned:CampusPeer{};
    observedPeer=CampusPeer{};
    portEXIT_CRITICAL(&peerLock);
    static const auto listener=WiFi.onEvent([](WiFiEvent_t,WiFiEventInfo_t info){
        reason.store(info.wifi_sta_disconnected.reason);
        linkDown.store(true);
    },ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    static const auto connected=WiFi.onEvent([](WiFiEvent_t,WiFiEventInfo_t){
        linkDown.store(false);
    },ARDUINO_EVENT_WIFI_STA_CONNECTED);
    (void)listener; (void)connected;
    stage(10);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    wifi_config_t config{};
    memcpy(config.sta.ssid,ssid,sizeof(ssid));
    config.sta.threshold.authmode=WIFI_AUTH_WPA2_ENTERPRISE;
    reason.store(0); certFlags.store(0);
    linkDown.store(false); // A connect attempt is in flight from here until the next disconnect event.
    if (!WiFi.mode(WIFI_STA) || esp_wifi_set_storage(WIFI_STORAGE_RAM)!=ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA,&config)!=ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)user,u)!=ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_username((uint8_t*)user,u)!=ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_password((uint8_t*)pass,p)!=ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_ca_cert(murmur_campus_ca,murmur_campus_ca_end-murmur_campus_ca)!=ESP_OK ||
        esp_wifi_sta_wpa2_ent_set_disable_time_check(false)!=ESP_OK ||
        esp_wifi_sta_wpa2_ent_enable()!=ESP_OK || esp_wifi_connect()!=ESP_OK) {
        stopRadio(State::Failed); return false;
    }
    stage(0);
    state=State::Joining; started=millis(); return true;
}
}

extern "C" int murmur_eap_ssl_setup(mbedtls_ssl_context *ssl,const mbedtls_ssl_config *cfg) {
    if (!cfg->ca_chain || cfg->authmode!=MBEDTLS_SSL_VERIFY_REQUIRED) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    int rc=mbedtls_ssl_setup(ssl,cfg);
    if (rc==0) rc=mbedtls_ssl_set_hostname(ssl,radius);
    if (rc==0) mbedtls_ssl_set_verify(ssl,checkDates,nullptr);
    return rc;
}
// In this SDK the outer EAP dispatcher is the only external caller.
extern "C" bool __real_eap_sm_allowMethod(void *,int,int);
extern "C" bool __wrap_eap_sm_allowMethod(void *sm,int vendor,int method) {
    return vendor==0 && method==25 && __real_eap_sm_allowMethod(sm,vendor,method);
}

bool campusOnline() { return state==State::Online; }
uint32_t campusNetworkTime() { return lastNetworkTime.load(); }
void resumeSavedCampusWifi() {
    if (campusOnline() || state==State::ClockOnly || state==State::Joining || !hasCampusProfile()) return;
    if (millis()<5500) {retryAt=5500;state=State::Deferred;return;}
    saveFailed=false;
    CampusPeer peer{};
    bool needTime=time(nullptr)<1704067200;
    if (needTime && !readPeer(peer)) {state=State::WaitingClock; return;}
    CampusLogin login;
    stage(20);
    const bool loaded=loadCampusProfile(login);
    stage(0);
    if (!loaded) {state=State::MissingCard; ++cardAttempts; retryAt=millis()+4000; return;}
    cardAttempts=0;
    usingSavedLogin=true;
    join(login.user,login.password,needTime?&peer:nullptr);
}
bool serviceCampusWifi() {
    if ((state==State::Deferred && int32_t(millis()-retryAt)>=0) ||
        (state==State::MissingCard && cardAttempts<4 && int32_t(millis()-retryAt)>=0)) {
        resumeSavedCampusWifi(); return true;
    }
    if (state==State::WaitingClock && time(nullptr)>=1704067200) { resumeSavedCampusWifi(); return true; }
    if (state==State::Joining) {
        if (WiFi.status()==WL_CONNECTED && uint32_t(WiFi.localIP())) {
            portENTER_CRITICAL(&peerLock);
            bool verifiedLeaf=observedPeer.checkedAt!=0;
            portEXIT_CRITICAL(&peerLock);
            if (!verifiedLeaf) {
                pending.clear(); savePending=false; stopRadio(State::Failed); return true;
            }
            state=bootstrap?State::ClockOnly:State::Online;
            networkRetries=0;
            if (!bootstrap) { rememberVerifiedPeer(); everOnline=true; failStreak=0; }
            started=onlineAt=millis(); clockUpdated.store(false); sessionTimeAt.store(0);
            sntp_set_time_sync_notification_cb([](timeval *t){
                lastNetworkTime.store(t->tv_sec);
                sessionTimeAt.store(millis()|1);
                clockUpdated.store(true);
            });
            tcpip_callback(sntpStart,nullptr); // TZ is set in setup().
            if (savePending) saveFailed=!saveCampusProfile(pending);
            pending.clear(); savePending=false; return true;
        }
        if (millis()-started>30000) {
            const unsigned why=reason.load();
            bool retry=usingSavedLogin && networkRetries<2 && !certFlags.load() &&
                (why==0 || why==WIFI_REASON_ASSOC_LEAVE || why==WIFI_REASON_NO_AP_FOUND ||
                 why==WIFI_REASON_BEACON_TIMEOUT || why==WIFI_REASON_ASSOC_EXPIRE);
            pending.clear(); savePending=false;
            stopRadio(retry?State::Deferred:(bootstrap?State::WaitingClock:State::Failed));
            if (retry) retryAt=millis()+2500*++networkRetries;
            return true;
        }
    } else if (state==State::ClockOnly) {
        if (clockUpdated.exchange(false) && time(nullptr)>=1704067200) {
            if (time(nullptr)+300<expectedPeer.checkedAt) {
                stopRadio(State::Failed);
                return true;
            }
            bootstrappedClock=true;
            stopRadio(State::Deferred);
            retryAt=millis()+1500;
            return true;
        }
        if (millis()-started>60000 || WiFi.status()!=WL_CONNECTED) {stopRadio(State::WaitingClock); return true;}
    } else if (state==State::Online) {
        const uint32_t now=millis(), synced=sessionTimeAt.load();
        const bool done=WiFi.status()!=WL_CONNECTED ||
            (manualSession ? now-onlineAt>=manualOnlineLimit
                           : (synced && now-synced>=autoAfterTime) || now-onlineAt>=autoOnlineLimit);
        if (done) { stopRadio(State::Off); return true; }
    }
    return clockUpdated.exchange(false);
}
void campusCommand(char *cmd,Print &out) {
    if (!strcmp(cmd,"diag")) {
        // Codes are listed in docs/campus-wifi.md. Zero means none was recorded.
        out.printf("wifi diag reset=%d loop_phase=%lu wifi_step=%lu\n",int(esp_reset_reason()),
                   (unsigned long)(stageBeforeReset>>8&0xff),(unsigned long)(stageBeforeReset&0xff));
        return;
    }
    if (!strcmp(cmd,"clock")) {
        out.printf("wifi clock ntp=%lu now=%lu cold_sync=%u\n",(unsigned long)campusNetworkTime(),(unsigned long)time(nullptr),unsigned(bootstrappedClock));
        return;
    }
    if (!strncmp(cmd,"join ",5) || !strncmp(cmd,"save ",5)) {
        char *user=cmd+5,*pass=strchr(user,'\t');
        if (!pass) {out.println("wifi error input"); return;}
        *pass++=0;
        usingSavedLogin=false; networkRetries=0; manualSession=true;
        savePending=false; saveFailed=false; pending.clear();
        if (!join(user,pass)) {out.println("wifi error setup or clock"); return;}
        if (cmd[0]=='s') {
            strcpy(pending.user,user); strcpy(pending.password,pass); savePending=true;
        }
        out.println("wifi joining"); return;
    }
    if (!strcmp(cmd,"profile")) { out.println(hasCampusProfile()?"wifi saved":"wifi empty"); return; }
    if (!strcmp(cmd,"connect")) startSavedCampusWifi(true);
    if (!strcmp(cmd,"auto")) { startSavedCampusWifi(false); strcpy(cmd,"status"); } // Same session as Settings → Wi-Fi.
    if (!strcmp(cmd,"forget") || !strcmp(cmd,"off")) {
        stopCampusWifi();
        if (!strcmp(cmd,"forget")) out.println(forgetCampusProfile()?"wifi forgotten":"wifi error storage");
        else out.println("wifi off");
        return;
    }
    if (!strcmp(cmd,"status") || !strcmp(cmd,"connect")) {
        if (saveFailed) out.println("wifi error saving profile");
        else if (campusOnline()) { out.print("wifi online "); out.println(WiFi.localIP()); }
        else if (state==State::Joining) out.println("wifi joining");
        else if (state==State::WaitingClock) out.println("wifi waiting clock");
        else if (state==State::ClockOnly) out.println("wifi syncing clock");
        else if (state==State::Deferred) out.println("wifi starting");
        else if (state==State::MissingCard) out.println("wifi profile unavailable");
        else if (state==State::Failed) out.printf("wifi failed reason=%u cert_flags=%u auto_retry_s=%lu\n",reason.load(),certFlags.load(),
                                                (unsigned long)(backoffRemaining()/1000));
        else out.println("wifi off");
        return;
    }
    if (!strncmp(cmd,"probe ",6) && campusOnline()) {
        IPAddress ip;
        WiFiClient client;
        if (ip.fromString(cmd+6) && client.connect(ip,22,1800)) {
            client.setTimeout(1);
            if (client.readStringUntil('\n').startsWith("SSH-")) {out.println("wifi pi reachable"); return;}
        }
        out.println("wifi pi unreachable"); return;
    }
    out.println("wifi error command");
}
void beginCampusDiagnostics() {
    const bool valid=(rtcStage&0xffff0000u)==stageMagic && esp_reset_reason()!=ESP_RST_POWERON;
    stageBeforeReset=valid?(rtcStage&0xffffu):0;
    rtcStage=stageMagic;
}
void diagLoopPhase(uint8_t phase) {
    rtcStage=stageMagic|(uint32_t(phase)<<8)|(rtcStage&0xffu);
}
void startSavedCampusWifi(bool manual) {
    const bool active=campusOnline() || state==State::Joining || state==State::ClockOnly;
    if (manual) manualSession=true;
    else if (!active) manualSession=false; // Never shorten a running manual session.
    resumeSavedCampusWifi();
}
void startCampusWifiAfterRecording() {
    if (!backoffRemaining()) startSavedCampusWifi(false);
}
void startCampusWifiOnWake() {
    if (backoffRemaining()) return;
    // A login costs several seconds of radio time. Only repeat it on wake when
    // the clock needs setting or the last session is old.
    if (time(nullptr)<1704067200 || !everOnline || millis()-onlineAt>=wakeInterval)
        startSavedCampusWifi(false);
}
void stopCampusWifi() {
    manualSession=false;
    pending.clear();
    savePending=saveFailed=false;
    stopRadio(State::Off);
}
