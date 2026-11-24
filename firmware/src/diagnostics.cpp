#include "diagnostics.h"
#include "card_usb.h"
#include <Arduino.h>
#include <esp32-hal-tinyusb.h>
#include <esp_crc.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <soc/assist_debug_reg.h>
#include <soc/system_reg.h>
#include <algorithm>
#include <atomic>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace diagnostics {
namespace {
constexpr unsigned ringSize = 128;
constexpr unsigned flashRecords = 32;
constexpr uint32_t ringMagic = 0x4d4c0001;
constexpr uint32_t snapshotMagic = 0x4d530001;
constexpr uint32_t firstCheckpointMs = 10000, checkpointIntervalMs = 60000;
constexpr uint32_t exportTimeoutMs = 15000, stalledExportMs = 2000;
constexpr uint8_t cdcInterface = 0;

struct Record {
    uint32_t sequence;
    uint32_t uptime;
    int32_t value;
    int32_t detail;
    Event event;
    uint8_t core;
    uint8_t reserved;
};
static_assert(sizeof(Record)==20, "Diagnostic record layout");

struct Session {
    uint32_t boot;
    uint8_t build[8];
};

template<unsigned Capacity>
struct Snapshot {
    Session session;
    unsigned count;
    uint32_t reset;
    uint32_t phase;
    Record records[Capacity];
};

struct SavedSnapshot {
    uint32_t magic;
    Snapshot<flashRecords> log;
    uint32_t checksum;
};

struct Ring {
    uint32_t magic;
    Session session;
    uint32_t last;
    Record records[ringSize];
};

RTC_NOINIT_ATTR Ring ring;
struct WatchdogCapture {
    uint32_t boot, uptime, names[2][2];
    uint32_t pc[3][2];
    uint32_t valid;
};
RTC_NOINIT_ATTR WatchdogCapture watchdog;
constexpr uint32_t watchdogMagic=0x57445432;
portMUX_TYPE ringLock=portMUX_INITIALIZER_UNLOCKED;
Snapshot<ringSize> previous, current;
SavedSnapshot previousCheckpoint, lastIncident, lastUsbGap;
TaskHandle_t worker;
std::atomic<bool> checkpointRequested{true};
enum class Request { None, Status, Dump, Usb, Invalid };
std::atomic<Request> request{Request::None};

const char *eventName(Event event) {
    switch (event) {
    case Event::Boot: return "boot";
    case Event::WorkerError: return "log.worker_error";
    case Event::StorageError: return "log.storage_error";
    case Event::ExportAborted: return "log.export_aborted";
    case Event::ClockSet: return "clock.set";
    case Event::LoopSlow: return "loop.slow";
    case Event::Sleep: return "ui.sleep";
    case Event::Wake: return "ui.wake";
    case Event::UsbAttached: return "usb.configured";
    case Event::UsbDetached: return "usb.unconfigured";
    case Event::CardReady: return "usb.card_ready";
    case Event::CardReadError: return "usb.card_error";
    case Event::WifiProfile: return "wifi.profile";
    case Event::WifiJoin: return "wifi.join";
    case Event::WifiJoinError: return "wifi.join_error";
    case Event::WifiLinkUp: return "wifi.link_up";
    case Event::WifiLinkDown: return "wifi.link_down";
    case Event::WifiGotIp: return "wifi.got_ip";
    case Event::WifiFailed: return "wifi.failed";
    case Event::SntpQueued: return "sntp.queued";
    case Event::SntpStarted: return "sntp.started";
    case Event::SntpSynced: return "sntp.synced";
    case Event::SntpStopped: return "sntp.stopped";
    case Event::WifiStopBegin: return "wifi.stop_begin";
    case Event::WifiDisconnect: return "wifi.disconnect";
    case Event::WifiEapDisabled: return "wifi.eap_disabled";
    case Event::WifiRadioOff: return "wifi.radio_off";
    case Event::UsbSuspend: return "usb.suspend";
    case Event::UsbResume: return "usb.resume";
    case Event::UsbBus: return "usb.bus";
    case Event::UsbTraffic: return "usb.traffic";
    case Event::UsbRead: return "usb.read";
    case Event::UsbGap: return "usb.gap";
    case Event::WatchdogTask: return "wdt.task";
    case Event::UsbIrq: return "usb.irq";
    case Event::UsbControl: return "usb.control";
    case Event::CpuPc: return "cpu.pc";
    case Event::UsbInStallBegin: return "usb.in_stall_begin";
    case Event::UsbInStallEnd: return "usb.in_stall_end";
    case Event::UsbBulkIn: return "usb.bulk_in";
    case Event::UsbBulkTx: return "usb.bulk_tx";
    case Event::UsbBulkOut: return "usb.bulk_out";
    case Event::UsbReadBlock: return "usb.read_block";
    case Event::UsbProbeError: return "usb.probe_error";
    case Event::UsbMediaUnavailable: return "usb.media_unavailable";
    }
    return "unknown";
}

template<unsigned Capacity>
void copyRecent(Snapshot<Capacity> &dest) {
    portENTER_CRITICAL(&ringLock);
    dest.session=ring.session;
    dest.count=0;
    const uint32_t last=ring.last;
    const unsigned count=std::min<unsigned>(last,Capacity);
    for (unsigned i=0; i<count; ++i) {
        const uint32_t seq=last-count+i+1;
        const Record &entry=ring.records[(seq-1)%ringSize];
        if (entry.sequence==seq) dest.records[dest.count++]=entry;
    }
    portEXIT_CRITICAL(&ringLock);
}

uint32_t checksum(const SavedSnapshot &snapshot) {
    return esp_crc32_le(0,reinterpret_cast<const uint8_t*>(&snapshot),offsetof(SavedSnapshot,checksum));
}

void load(nvs_handle_t nvs, const char *key, int slot, SavedSnapshot &snapshot) {
    size_t length=sizeof(snapshot);
    const esp_err_t err=nvs_get_blob(nvs,key,&snapshot,&length);
    if (err==ESP_OK && length==sizeof(snapshot) && snapshot.magic==snapshotMagic &&
        snapshot.log.count<=flashRecords && snapshot.checksum==checksum(snapshot)) return;
    snapshot={};
    if (err!=ESP_ERR_NVS_NOT_FOUND) record(Event::StorageError,err==ESP_OK?ESP_ERR_INVALID_CRC:err,slot);
}

bool save(nvs_handle_t nvs, const char *key, int slot, SavedSnapshot &snapshot) {
    snapshot.magic=snapshotMagic;
    snapshot.checksum=checksum(snapshot);
    esp_err_t err=nvs_set_blob(nvs,key,&snapshot,sizeof(snapshot));
    if (err==ESP_OK) err=nvs_commit(nvs);
    if (err==ESP_OK) return true;
    record(Event::StorageError,err,slot);
    return false;
}

struct Export {
    uint32_t started=millis();
    uint32_t progress=started;

    // Only the worker writes while a log command is active. Use TinyUSB's FIFO directly
    // so a full endpoint never enters the blocking Arduino USBCDC::write loop.
    bool send(const char *text) {
        size_t remaining=strlen(text);
        while (remaining) {
            const uint32_t now=millis();
            if (!tud_cdc_n_connected(cdcInterface) || now-started>=exportTimeoutMs || now-progress>=stalledExportMs)
                return false;
            const size_t available=tud_cdc_n_write_available(cdcInterface);
            const size_t sent=available?tud_cdc_n_write(cdcInterface,text,std::min(available,remaining)):0;
            if (sent) {
                text+=sent;
                remaining-=sent;
                progress=now;
            }
            tud_cdc_n_write_flush(cdcInterface);
            if (!sent) vTaskDelay(pdMS_TO_TICKS(5));
        }
        return true;
    }
};

template<unsigned Capacity>
bool sendSnapshot(Export &out, const char *name, const Snapshot<Capacity> &snapshot) {
    if (!snapshot.count) return true;
    char build[17], line[192];
    for (unsigned i=0; i<8; ++i) snprintf(build+2*i,3,"%02x",unsigned(snapshot.session.build[i]));
    snprintf(line,sizeof(line),"log source=%s boot=%08lx build=%s reset=%lu phase=%lu wifi_step=%lu count=%u\n",
             name,(unsigned long)snapshot.session.boot,build,(unsigned long)snapshot.reset,
             (unsigned long)(snapshot.phase>>8),(unsigned long)(snapshot.phase&255),snapshot.count);
    if (!out.send(line)) return false;
    for (unsigned i=0; i<snapshot.count; ++i) {
        const auto &r=snapshot.records[i];
        snprintf(line,sizeof(line),"log seq=%lu ms=%lu core=%u event=%s a=%ld b=%ld\n",
                 (unsigned long)r.sequence,(unsigned long)r.uptime,unsigned(r.core),eventName(r.event),
                 (long)r.value,(long)r.detail);
        if (!out.send(line)) return false;
    }
    return true;
}

bool exportLogs(Request pending) {
    Export out;
    if (pending==Request::Invalid) return out.send("log error expected status, dump or usb\n");
    copyRecent(current);
    char line[128];
    if (pending==Request::Status) {
        snprintf(line,sizeof(line),"log status version=1 boot=%08lx current=%u previous=%u checkpoint=%u incident=%u\n",
                 (unsigned long)current.session.boot,current.count,previous.count,
                 previousCheckpoint.log.count,lastIncident.log.count);
        return out.send(line);
    }
    if (pending==Request::Usb) {
        snprintf(line,sizeof(line),"log end count=%u\n",lastUsbGap.log.count+current.count);
        return out.send("log begin version=1\n") && sendSnapshot(out,"usb_gap",lastUsbGap.log) &&
               sendSnapshot(out,"current",current) && out.send(line);
    }
    unsigned count=previousCheckpoint.log.count+lastIncident.log.count+previous.count+current.count;
    snprintf(line,sizeof(line),"log end count=%u\n",count);
    return out.send("log begin version=1\n") &&
           sendSnapshot(out,"checkpoint",previousCheckpoint.log) &&
           sendSnapshot(out,"incident",lastIncident.log) &&
           sendSnapshot(out,"previous",previous) &&
           sendSnapshot(out,"current",current) && out.send(line);
}

bool isCrash(uint32_t reason) {
    return reason==ESP_RST_PANIC || reason==ESP_RST_INT_WDT || reason==ESP_RST_TASK_WDT || reason==ESP_RST_WDT;
}

void runWorker(void *) {
    nvs_handle_t nvs=0;
    const esp_err_t storage=nvs_open("diagnostics",NVS_READWRITE,&nvs);
    if (storage==ESP_OK) {
        load(nvs,"checkpoint",1,previousCheckpoint);
        load(nvs,"incident",2,lastIncident);
        load(nvs,"usb-gap",3,lastUsbGap);
        if (previous.count && isCrash(previous.reset)) {
            SavedSnapshot incident{};
            auto &dest=incident.log;
            dest.session=previous.session;
            dest.reset=previous.reset;
            dest.phase=previous.phase;
            dest.count=std::min<unsigned>(previous.count,flashRecords);
            memcpy(dest.records,previous.records+previous.count-dest.count,dest.count*sizeof(Record));
            if (save(nvs,"incident",2,incident)) lastIncident=incident;
        }
    } else record(Event::StorageError,storage,0);

    uint32_t lastCheckpoint=0;
    bool attemptedCheckpoint=false;
    bool gapPending=false;
    uint32_t gapSavedAt=0;
    for (;;) {
        const Request pending=request.load();
        if (pending!=Request::None) {
            if (!exportLogs(pending)) record(Event::ExportAborted);
            request.store(Request::None);
        }
        const uint32_t now=millis();
        if (sampleUsbProgress(now)) {
            copyRecent(lastUsbGap.log);
            gapPending=true;
        }
        if (storage==ESP_OK && gapPending && (!gapSavedAt || now-gapSavedAt>=60000)) {
            save(nvs,"usb-gap",3,lastUsbGap);
            gapSavedAt=now;
            gapPending=false;
        }
        const bool due=attemptedCheckpoint?now-lastCheckpoint>=checkpointIntervalMs:now>=firstCheckpointMs;
        if (storage==ESP_OK && due && checkpointRequested.exchange(false)) {
            SavedSnapshot checkpoint{};
            copyRecent(checkpoint.log);
            save(nvs,"checkpoint",1,checkpoint);
            attemptedCheckpoint=true;
            lastCheckpoint=now;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
}

// IDF invokes this hook from the watchdog ISR. Only touch RTC RAM here.
// These are the tasks running at the interrupt, not necessarily the overdue tasks.
extern "C" void IRAM_ATTR esp_task_wdt_isr_user_handler() {
    watchdog.valid=0;
    watchdog.boot=ring.session.boot;
    watchdog.uptime=xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;
    for (auto &pair:watchdog.pc) {
        pair[0]=REG_READ(ASSIST_DEBUG_CORE_0_RCD_PDEBUGPC_REG);
        pair[1]=REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
    }
    for (int core=0; core<2; ++core) {
        auto handle=xTaskGetCurrentTaskHandleForCPU(core);
        const char *name=handle?pcTaskGetName(handle):nullptr;
        watchdog.names[core][0]=watchdog.names[core][1]=0;
        for (int i=0; name && i<8 && name[i]; ++i)
            watchdog.names[core][i/4]|=uint32_t(uint8_t(name[i]))<<(8*(i%4));
    }
    __atomic_store_n(&watchdog.valid,watchdogMagic,__ATOMIC_RELEASE);
}

void begin(uint32_t previousPhase) {
    const esp_reset_reason_t reason=esp_reset_reason();
    if (reason!=ESP_RST_POWERON && ring.magic==ringMagic) {
        if (reason==ESP_RST_TASK_WDT && watchdog.valid==watchdogMagic && watchdog.boot==ring.session.boot) {
            // The old session is no longer running. Append before copying its incident.
            for (uint8_t core=0; core<2; ++core) {
                const uint32_t seq=++ring.last;
                ring.records[(seq-1)%ringSize]={seq,watchdog.uptime,int32_t(watchdog.names[core][0]),
                    int32_t(watchdog.names[core][1]),Event::WatchdogTask,core,0};
            }
            for (const auto &pair:watchdog.pc) {
                const uint32_t seq=++ring.last;
                ring.records[(seq-1)%ringSize]={seq,watchdog.uptime,int32_t(pair[0]),int32_t(pair[1]),Event::CpuPc,0,0};
            }
        }
        copyRecent(previous);
        previous.reset=reason;
        previous.phase=previousPhase;
    }
    watchdog.valid=0;
    REG_SET_BIT(SYSTEM_CPU_PERI_CLK_EN_REG,SYSTEM_CLK_EN_ASSIST_DEBUG);
    REG_CLR_BIT(SYSTEM_CPU_PERI_RST_EN_REG,SYSTEM_RST_EN_ASSIST_DEBUG);
    REG_WRITE(ASSIST_DEBUG_CORE_0_RCD_PDEBUGENABLE_REG,1);
    REG_WRITE(ASSIST_DEBUG_CORE_0_RCD_RECORDING_REG,1);
    REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_PDEBUGENABLE_REG,1);
    REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_RECORDING_REG,1);
    ring.magic=0;
    ring.session.boot=esp_random();
    memcpy(ring.session.build,esp_ota_get_app_description()->app_elf_sha256,sizeof(ring.session.build));
    ring.last=0;
    __atomic_store_n(&ring.magic,ringMagic,__ATOMIC_RELEASE);
    record(Event::Boot,reason,previousPhase);
}

void startWorker() {
    if (xTaskCreate(runWorker,"diag",5120,nullptr,1,&worker)!=pdPASS) record(Event::WorkerError);
}

void record(Event event, int32_t value, int32_t detail) {
    const uint32_t uptime=esp_timer_get_time()/1000;
    const uint8_t core=xPortGetCoreID();
    portENTER_CRITICAL(&ringLock);
    const uint32_t sequence=ring.last+1;
    auto &entry=ring.records[(sequence-1)%ringSize];
    entry={0,uptime,value,detail,event,core,0};
    __atomic_store_n(&entry.sequence,sequence,__ATOMIC_RELEASE);
    __atomic_store_n(&ring.last,sequence,__ATOMIC_RELEASE);
    portEXIT_CRITICAL(&ringLock);
}

void requestCheckpoint() { checkpointRequested.store(true); }

void command(const char *name) {
    if (!worker) return;
    if (!strcmp(name,"status")) request.store(Request::Status);
    else if (!strcmp(name,"dump")) request.store(Request::Dump);
    else if (!strcmp(name,"usb")) request.store(Request::Usb);
    else request.store(Request::Invalid);
}

bool exporting() { return request.load()!=Request::None; }

}
