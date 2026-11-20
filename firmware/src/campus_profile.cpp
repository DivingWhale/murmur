#include "campus_profile.h"
#include "card_usb.h"
#include <Preferences.h>
#include <SD_MMC.h>
#include <esp_system.h>
#include <mbedtls/gcm.h>
#include <mbedtls/platform_util.h>
#include <time.h>

namespace {
const char *paths[]={"/murmur/wifi-a.enc","/murmur/wifi-b.enc"};
struct Key {
    uint8_t bytes[32];
    uint32_t slot, savedAt;
    ~Key(){ mbedtls_platform_zeroize(bytes,sizeof(bytes)); }
};
struct Envelope {
    uint8_t nonce[12], tag[16], ciphertext[258];
};
// The bytes in NVS select one complete file. Replace this entry only after
// the inactive file has been closed and read back successfully.
bool readKey(Preferences &prefs,Key &key) {
    return prefs.getBytesLength("key")==sizeof(key) &&
           prefs.getBytes("key",&key,sizeof(key))==sizeof(key) && key.slot<2;
}
struct CardSession {
    bool owned=tryTakeCardForLocalFiles();
    bool mount() {
        return owned && SD_MMC.setPins(cardPins::clk,cardPins::cmd,cardPins::d0,cardPins::d1,cardPins::d2,cardPins::d3) &&
               SD_MMC.begin("/sdcard",false,false);
    }
    ~CardSession(){ if (owned) { SD_MMC.end(); releaseCardAfterRecording(); } }
};
bool readFile(const char *path,Envelope &e) {
    File f=SD_MMC.open(path,FILE_READ);
    return f && f.size()==sizeof(e) && f.read(reinterpret_cast<uint8_t*>(&e),sizeof(e))==sizeof(e);
}
bool crypt(const Key &key,Envelope &e,const uint8_t *clearIn,uint8_t *clearOut) {
    static const uint8_t context[]="Murmur/HKUSTGZ/profile/v1";
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc=mbedtls_gcm_setkey(&g,MBEDTLS_CIPHER_ID_AES,key.bytes,256);
    if (!rc && clearIn)
        rc=mbedtls_gcm_crypt_and_tag(&g,MBEDTLS_GCM_ENCRYPT,sizeof(e.ciphertext),e.nonce,sizeof(e.nonce),
            context,sizeof(context)-1,clearIn,e.ciphertext,sizeof(e.tag),e.tag);
    else if (!rc)
        rc=mbedtls_gcm_auth_decrypt(&g,sizeof(e.ciphertext),e.nonce,sizeof(e.nonce),context,sizeof(context)-1,
            e.tag,sizeof(e.tag),e.ciphertext,clearOut);
    mbedtls_gcm_free(&g);
    return rc==0;
}
}
static_assert(sizeof(CampusLogin)==sizeof(Envelope::ciphertext),"Profile layout changed");
void CampusLogin::clear(){ mbedtls_platform_zeroize(this,sizeof(*this)); }
uint32_t campusClockHint() {
    Preferences prefs; Key key{};
    return prefs.begin("campus",true) && readKey(prefs,key)?key.savedAt:0;
}
bool hasCampusProfile(){return campusClockHint()!=0;}
bool loadCampusProfile(CampusLogin &login) {
    login.clear();
    Preferences prefs; Key key{}; Envelope e{};
    if (!prefs.begin("campus",true) || !readKey(prefs,key)) return false;
    CardSession card;
    if (!card.mount() || !readFile(paths[key.slot],e) ||
        !crypt(key,e,nullptr,reinterpret_cast<uint8_t*>(&login)) ||
        !login.user[0] || !login.password[0] || login.user[128] || login.password[128]) {
        login.clear(); return false;
    }
    return true;
}
bool saveCampusProfile(const CampusLogin &login) {
    Preferences prefs; Key key{}; Envelope e{},check{};
    if (!prefs.begin("campus",false)) return false;
    bool replacing=readKey(prefs,key);
    key.slot=replacing?1-key.slot:0;
    key.savedAt=time(nullptr);
    esp_fill_random(key.bytes,sizeof(key.bytes));
    esp_fill_random(e.nonce,sizeof(e.nonce));
    if (!crypt(key,e,reinterpret_cast<const uint8_t*>(&login),nullptr)) return false;
    CardSession card;
    if (!card.mount() || (!SD_MMC.exists("/murmur") && !SD_MMC.mkdir("/murmur"))) return false;
    File f=SD_MMC.open(paths[key.slot],FILE_WRITE);
    if (!f || f.write(reinterpret_cast<const uint8_t*>(&e),sizeof(e))!=sizeof(e)) return false;
    f.close();
    if (!readFile(paths[key.slot],check) || memcmp(&check,&e,sizeof(e)) ||
        prefs.putBytes("key",&key,sizeof(key))!=sizeof(key)) return false;
    if (replacing) SD_MMC.remove(paths[1-key.slot]);
    return true;
}
bool forgetCampusProfile() {
    Preferences prefs;
    if (!prefs.begin("campus",false)) return false;
    return !prefs.isKey("key") || prefs.remove("key");
}
