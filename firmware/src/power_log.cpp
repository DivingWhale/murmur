#include "power_log.h"
#include "board.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp32-hal-tinyusb.h>

namespace {
constexpr uint32_t window = 60000;          // One sample per minute.
constexpr unsigned capacity = 12 * 60;      // 12 h of samples.
constexpr uint32_t saveEvery = 10;          // Minutes between NVS summaries.
constexpr uint32_t runMagic = 0x50574c31;   // "PWL1"

struct Sample {
    uint16_t minute;   // Minutes since start.
    uint16_t mv;       // Battery voltage at the end of the minute.
    uint16_t frames;   // Display refreshes during the minute.
    uint8_t awake, fast, wifi, rec, usb, charge; // Seconds of the minute in each state.
    uint8_t light;     // Mean backlight percent (0 while the screen is off).
};

// Totals for one run, from start to the last NVS save.
struct Run {
    uint32_t magic;
    uint32_t seconds, awake, fast, wifi, rec, usb, charge;
    uint32_t frames, lightSeconds; // lightSeconds: backlight percent x seconds.
    uint16_t startMv, endMv, minMv;
    int16_t startReset;            // Why this run started (esp_reset_reason).
};

Sample *samples;
unsigned count, head;      // Ring: `count` valid samples ending before `head`.
Run run, previous;
bool havePrevious;
int16_t bootReason;
Preferences store;

uint32_t windowStart, last, framesAtStart;
uint32_t awakeMs, fastMs, wifiMs, recMs, usbMs, chargeMs;
uint64_t lightPercentMs;

uint16_t readMv() {
    uint32_t sum=0;
    for (int i=0;i<16;++i) sum+=batteryMillivolts();
    return uint16_t(sum/16);
}
uint8_t share(uint32_t ms, uint32_t span) { return uint8_t(min<uint32_t>(60, (ms*60+span/2)/span)); }
const char *resetName(int r) {
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}
void printRun(Print &out, const char *tag, const Run &r) {
    const uint32_t s=max<uint32_t>(1,r.seconds), mins=r.seconds/60;
    const int drop=int(r.startMv)-int(r.endMv);
    out.printf("%s minutes=%lu start_mv=%u end_mv=%u min_mv=%u drop_mv=%d mv_per_hour=%ld "
               "awake_pct=%lu fast_pct=%lu wifi_pct=%lu rec_pct=%lu usb_pct=%lu charge_pct=%lu "
               "light_avg=%lu fps_avg=%lu.%lu started_by=%s\n",
               tag,(unsigned long)mins,r.startMv,r.endMv,r.minMv,drop,
               long(int64_t(drop)*3600/int64_t(s)),
               (unsigned long)(r.awake*100/s),(unsigned long)(r.fast*100/s),(unsigned long)(r.wifi*100/s),
               (unsigned long)(r.rec*100/s),(unsigned long)(r.usb*100/s),(unsigned long)(r.charge*100/s),
               (unsigned long)(r.lightSeconds/s),(unsigned long)(r.frames/s),(unsigned long)(r.frames*10/s%10),
               resetName(r.startReset));
}
void closeWindow(uint32_t now) {
    const uint32_t span=max<uint32_t>(1,now-windowStart);
    const uint32_t frames=displayFrames();
    Sample s{};
    s.minute=uint16_t(min<uint32_t>(0xffff,run.seconds/60+1));
    s.mv=readMv();
    s.frames=uint16_t(min<uint32_t>(0xffff,frames-framesAtStart));
    s.awake=share(awakeMs,span); s.fast=share(fastMs,span); s.wifi=share(wifiMs,span);
    s.rec=share(recMs,span); s.usb=share(usbMs,span); s.charge=share(chargeMs,span);
    s.light=uint8_t(min<uint32_t>(100,(lightPercentMs+span/2)/span));
    if (samples) {
        samples[head]=s;
        head=(head+1)%capacity;
        if (count<capacity) ++count;
    }
    run.seconds+=span/1000;
    run.awake+=awakeMs/1000; run.fast+=fastMs/1000; run.wifi+=wifiMs/1000;
    run.rec+=recMs/1000; run.usb+=usbMs/1000; run.charge+=chargeMs/1000;
    run.frames+=s.frames; run.lightSeconds+=lightPercentMs/1000;
    run.endMv=s.mv;
    if (s.mv<run.minMv) run.minMv=s.mv;
    awakeMs=fastMs=wifiMs=recMs=usbMs=chargeMs=0;
    lightPercentMs=0;
    windowStart=now; framesAtStart=frames;
    if (s.minute%saveEvery==0) store.putBytes("run",&run,sizeof(run));
}
}

void beginPowerLog() {
    samples=static_cast<Sample *>(heap_caps_calloc(capacity,sizeof(Sample),MALLOC_CAP_SPIRAM));
    bootReason=int16_t(esp_reset_reason());
    store.begin("murmur-power");
    // The last saved summary belongs to the run that just ended. Keep it as "prev"
    // unless that run was too short to save anything new (then "prev" is still right).
    Run saved{};
    if (store.getBytes("run",&saved,sizeof(saved))==sizeof(saved) && saved.magic==runMagic) {
        store.putBytes("prev",&saved,sizeof(saved));
        store.remove("run");
    }
    havePrevious=store.getBytes("prev",&previous,sizeof(previous))==sizeof(previous) && previous.magic==runMagic;
    run=Run{};
    run.magic=runMagic;
    run.startMv=run.endMv=run.minMv=readMv();
    run.startReset=bootReason;
    windowStart=last=millis();
    framesAtStart=displayFrames();
}

void powerLogTick(uint32_t now, bool awake, bool recording) {
    const uint32_t dt=now-last;
    last=now;
    if (awake) awakeMs+=dt;
    if (cpuMhz()>80) fastMs+=dt;
    if (WiFi.getMode()!=WIFI_OFF) wifiMs+=dt;
    if (recording) recMs+=dt;
    if (tud_mounted()) usbMs+=dt;
    if (charging()) chargeMs+=dt;
    lightPercentMs+=uint64_t(backlightPercent())*dt;
    if (now-windowStart>=window) closeWindow(now);
}

void powerCommand(const char *args, Print &out) {
    if (!*args) {
        const uint16_t mv=readMv();
        out.printf("power mv=%u pct=%d charging=%d usb=%d mhz=%lu light=%u wifi=%s up_min=%lu samples=%u\n",
                   mv,batteryPercent(),int(charging()),int(tud_mounted()),(unsigned long)cpuMhz(),
                   backlightPercent(),WiFi.getMode()!=WIFI_OFF?"on":"off",(unsigned long)(millis()/60000),count);
        printRun(out,"power run",run);
        return;
    }
    if (!strcmp(args,"last")) {
        if (havePrevious) {
            printRun(out,"power last",previous);
            out.printf("power last ended_by=%s\n",resetName(bootReason)); // How this start happened.
        } else {
            out.println("power last none");
        }
        return;
    }
    if (!strcmp(args,"log")) {
        out.println("minute,mv,awake_s,fast_s,light_pct,wifi_s,rec_s,usb_s,charge_s,frames");
        for (unsigned i=0;i<count;++i) {
            const Sample &s=samples[(head+capacity-count+i)%capacity];
            out.printf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",s.minute,s.mv,s.awake,s.fast,s.light,
                       s.wifi,s.rec,s.usb,s.charge,s.frames);
        }
        out.println("power end");
        return;
    }
    out.println("power error command");
}
