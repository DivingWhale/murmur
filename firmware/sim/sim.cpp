// Host simulator for the handheld UI.
//
// Compiles the real src/main.cpp and src/fonts.cpp against LVGL with fake
// hardware, then runs a script of button presses and touches on a virtual
// clock and writes screenshots as PNG. See sim/README.md.
#include "../src/audio.h"
#include "../src/board.h"
#include "../src/campus_profile.h"
#include "../src/campus_wifi.h"
#include "../src/card_usb.h"
#include <USB.h>
#include <Preferences.h>
#include <lvgl.h>
#include <zlib.h>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

void setup();
void loop();

// ---------------------------------------------------------------- clock
namespace {
uint32_t nowMs = 0;
// Wed 18 Nov 2026 10:40:00 Asia/Shanghai, matching the prototype's sample day.
time_t epochBase = 1794969600;
time_t clockOffset = 0;
bool clockSet = true;
}
uint32_t millis() { return nowMs; }
void delay(uint32_t ms) { nowMs += ms; }
time_t sim_time(time_t *out) {
    time_t t = clockSet ? epochBase + clockOffset + nowMs / 1000 : 0;
    if (out) *out = t;
    return t;
}
int sim_settimeofday(const struct timeval *tv, const void *) {
    clockSet = true;
    clockOffset = tv->tv_sec - epochBase - nowMs / 1000;
    return 0;
}

// ---------------------------------------------------------------- inputs
namespace {
std::map<int, bool> pressed;
bool touching = false;
int touchX = 0, touchY = 0;
std::deque<char> serialIn;
}
int digitalRead(int pin) { return pressed[pin] ? LOW : HIGH; }
int USBCDC::available() { return int(serialIn.size()); }
int USBCDC::read() { char c = serialIn.front(); serialIn.pop_front(); return c; }

namespace { std::map<std::string, long> nvs; }
long Preferences::get(const char *k, long d) { auto it = nvs.find(k); return it == nvs.end() ? d : it->second; }
size_t Preferences::put(const char *k, long v, size_t n) { nvs[k] = v; return n; }

// ---------------------------------------------------------------- board
namespace {
uint16_t frame[240 * 240];
uint8_t backlight = 0;
int simBattery = 78;
bool simCharging = false;
lv_disp_draw_buf_t drawBuffer;
lv_disp_drv_t display;
lv_color_t pixels[240 * 240];
void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *buf) {
    for (int y = a->y1; y <= a->y2; ++y)
        for (int x = a->x1; x <= a->x2; ++x)
            frame[y * 240 + x] = (buf++)->full;
    lv_disp_flush_ready(drv);
}
}
void boardBegin() {
    lv_init();
    lv_disp_draw_buf_init(&drawBuffer, pixels, nullptr, 240 * 240);
    lv_disp_drv_init(&display);
    display.hor_res = display.ver_res = 240;
    display.flush_cb = flush;
    display.draw_buf = &drawBuffer;
    lv_disp_drv_register(&display);
}
void releaseBatteryPower() {}
void setBacklight(uint8_t percent) { backlight = percent; }
uint32_t displayFrames() { return 0; }
void setLowPower(bool) {}
int batteryPercent() { return simBattery; }
bool charging() { return simCharging; }
Contact readContact(int &x, int &y) {
    if (!touching) return Contact::Released;
    x = touchX; y = touchY;
    return Contact::Pressed;
}

// ---------------------------------------------------------------- audio
namespace { uint32_t recStart, recBytes; bool recording, cardPresent = true; }
AudioResult startRecording() {
    if (!cardPresent) return AudioResult::Card;
    recording = true; recStart = nowMs; return AudioResult::Ok;
}
AudioResult serviceRecording() { return AudioResult::Ok; }
AudioResult finishRecording() { recording = false; recBytes = (nowMs - recStart) / 1000; return AudioResult::Ok; }
bool discardRecording() { recording = false; return true; }
uint32_t recordedSeconds() { return recording ? (nowMs - recStart) / 1000 : recBytes; }
uint8_t audioLevel() {
    // Speech-like envelope: syllables of about 180 ms with pauses.
    if (!recording) return 0;
    double t = (nowMs - recStart) / 1000.0;
    double env = std::max(0.0, std::sin(t * 17.0)) * (0.55 + 0.45 * std::sin(t * 2.1));
    return uint8_t(std::min(20.0, 2 + 18 * env));
}
void printMicLevels(Print &out) { out.println("mic-speech-sim"); }

// ---------------------------------------------------------------- card, Wi-Fi
namespace { bool online = true; }
void beginCardUsb(USBCDC &) {}
void takeCardForRecording() {}
bool tryTakeCardForLocalFiles() { return true; }
void releaseCardAfterRecording() {}
int64_t cardFreeBytes() { return cardPresent ? 12400000000LL : -1; }
bool campusOnline() { return online; }
uint32_t campusNetworkTime() { return online ? uint32_t(sim_time(nullptr)) : 0; }
void startSavedCampusWifi(bool) {}
void startCampusWifiOnWake() {}
void startCampusWifiAfterRecording() {}
void stopCampusWifi() {}
bool serviceCampusWifi() { return false; }
void campusCommand(char *cmd, Print &out) { out.printf("wifi sim %s\n", cmd); }
void beginCampusDiagnostics() {}
void diagLoopPhase(uint8_t) {}
uint32_t campusClockHint() { return 0; }
bool hasCampusProfile() { return true; }

// ---------------------------------------------------------------- PNG output
namespace {
std::string outDir = ".";
uint32_t crc(const std::string &type, const std::vector<uint8_t> &data) {
    uLong c = crc32(0, reinterpret_cast<const Bytef *>(type.data()), type.size());
    return crc32(c, data.data(), data.size());
}
void chunk(std::ofstream &f, const std::string &type, const std::vector<uint8_t> &data) {
    auto be = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; f.write((char *)b, 4); };
    be(data.size()); f.write(type.data(), 4); f.write((const char *)data.data(), data.size()); be(crc(type, data));
}
void writePng(const std::string &name, int w, int h, const std::vector<uint8_t> &rgb) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3); }
    uLongf size = compressBound(raw.size());
    std::vector<uint8_t> z(size);
    compress2(z.data(), &size, raw.data(), raw.size(), 9);
    z.resize(size);
    std::ofstream f(outDir + "/" + name + ".png", std::ios::binary);
    f.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                 uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr); chunk(f, "IDAT", z); chunk(f, "IEND", {});
}
// Screen pixel as RGB888, black when the backlight is off.
void pixel(int x, int y, uint8_t *out) {
    uint16_t p = frame[y * 240 + x];
    if (!backlight) { out[0] = out[1] = out[2] = 0; return; }
    out[0] = ((p >> 11) & 31) * 255 / 31; out[1] = ((p >> 5) & 63) * 255 / 63; out[2] = (p & 31) * 255 / 31;
}
void shot(const std::string &name, int scale) {
    int s = 240 * scale;
    std::vector<uint8_t> rgb(s * s * 3);
    for (int y = 0; y < s; ++y) for (int x = 0; x < s; ++x) pixel(x / scale, y / scale, &rgb[(y * s + x) * 3]);
    writePng(name, s, s, rgb);
}
// Contact sheet of frames, left to right then top to bottom, 6 per row.
struct Film { std::string name; uint32_t until, step, next; std::vector<std::vector<uint8_t>> frames; };
void saveFilm(Film &film) {
    const int cols = 6, gap = 4, n = int(film.frames.size()), rows = (n + cols - 1) / cols;
    const int w = cols * 240 + (cols - 1) * gap, h = rows * 240 + (rows - 1) * gap;
    std::vector<uint8_t> rgb(w * h * 3, 60);
    for (int i = 0; i < n; ++i) {
        int ox = (i % cols) * (240 + gap), oy = (i / cols) * (240 + gap);
        for (int y = 0; y < 240; ++y) memcpy(&rgb[((oy + y) * w + ox) * 3], &film.frames[i][y * 240 * 3], 240 * 3);
    }
    writePng(film.name, w, h, rgb);
}
}

// ---------------------------------------------------------------- script
namespace {
std::vector<Film> films;
Film sheet; // Screens collected with `snap`, written by the next `sheet` or at exit.
void step() {
    loop();
    for (auto &film : films) {
        if (nowMs < film.next || nowMs > film.until) continue;
        film.next += film.step;
        std::vector<uint8_t> rgb(240 * 240 * 3);
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 240; ++x) pixel(x, y, &rgb[(y * 240 + x) * 3]);
        film.frames.push_back(std::move(rgb));
    }
    for (auto it = films.begin(); it != films.end();)
        if (nowMs > it->until) { saveFilm(*it); it = films.erase(it); } else ++it;
}
void runFor(uint32_t ms) {
    uint32_t end = nowMs + ms;
    while (nowMs < end) step();
}
int pinFor(const std::string &key) {
    if (key == "plus") return plusPin;
    if (key == "next") return nextPin;
    if (key == "ok") return okPin;
    fprintf(stderr, "unknown key %s\n", key.c_str()); exit(2);
}
void moveTo(int x, int y, uint32_t ms) {
    int x0 = touchX, y0 = touchY;
    for (uint32_t t = 0; t < ms; t += 8) {
        touchX = x0 + (x - x0) * int(t) / int(ms); touchY = y0 + (y - y0) * int(t) / int(ms);
        runFor(8);
    }
    touchX = x; touchY = y;
}
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: murmur-sim <script> <out dir>\n"); return 2; }
    outDir = argv[2];
    std::ifstream script(argv[1]);
    if (!script) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    setup();
    std::string line;
    int lineNo = 0;
    while (std::getline(script, line)) {
        ++lineNo;
        std::istringstream in(line);
        std::string cmd;
        if (!(in >> cmd) || cmd[0] == '#') continue;
        if (cmd == "wait") { uint32_t ms; in >> ms; runFor(ms); }
        else if (cmd == "down" || cmd == "up") { std::string k; in >> k; pressed[pinFor(k)] = cmd == "down"; runFor(1); }
        else if (cmd == "press") { std::string k; in >> k; pressed[pinFor(k)] = true; runFor(80); pressed[pinFor(k)] = false; runFor(40); }
        else if (cmd == "hold") { std::string k; uint32_t ms; in >> k >> ms; pressed[pinFor(k)] = true; runFor(ms); pressed[pinFor(k)] = false; runFor(40); }
        else if (cmd == "touch") { in >> touchX >> touchY; touching = true; runFor(12); }
        else if (cmd == "move") { int x, y; uint32_t ms; in >> x >> y >> ms; moveTo(x, y, ms); }
        else if (cmd == "lift") { touching = false; runFor(12); }
        else if (cmd == "tap") { in >> touchX >> touchY; touching = true; runFor(60); touching = false; runFor(40); }
        else if (cmd == "swipe") {
            int x1, y1, x2, y2; uint32_t ms; in >> x1 >> y1 >> x2 >> y2 >> ms;
            touchX = x1; touchY = y1; touching = true; runFor(12); moveTo(x2, y2, ms); touching = false; runFor(12);
        }
        else if (cmd == "shot") { std::string n; int scale = 2; in >> n >> scale; shot(n, scale ? scale : 2); }
        else if (cmd == "sheet") { std::string n; in >> n; if (!sheet.name.empty()) saveFilm(sheet); sheet = Film{}; sheet.name = n; }
        else if (cmd == "snap") {
            std::vector<uint8_t> rgb(240 * 240 * 3);
            for (int y = 0; y < 240; ++y) for (int x = 0; x < 240; ++x) pixel(x, y, &rgb[(y * 240 + x) * 3]);
            sheet.frames.push_back(std::move(rgb));
        }
        else if (cmd == "film") { Film f; uint32_t total; in >> f.name >> total >> f.step; f.next = nowMs; f.until = nowMs + total; films.push_back(f); }
        else if (cmd == "serial") { std::string rest; std::getline(in, rest); rest.erase(0, rest.find_first_not_of(' ')); for (char c : rest) serialIn.push_back(c); serialIn.push_back('\n'); runFor(5); }
        else if (cmd == "online") { int v; in >> v; online = v; }
        else if (cmd == "card") { int v; in >> v; cardPresent = v; }
        else if (cmd == "battery") { in >> simBattery; }
        else if (cmd == "charging") { int v; in >> v; simCharging = v; }
        else if (cmd == "clock") { std::string v; in >> v; clockSet = v != "unset"; }
        else { fprintf(stderr, "%s:%d: unknown command %s\n", argv[1], lineNo, cmd.c_str()); return 2; }
    }
    for (auto &film : films) saveFilm(film);
    if (!sheet.name.empty() && !sheet.frames.empty()) saveFilm(sheet);
    return 0;
}
