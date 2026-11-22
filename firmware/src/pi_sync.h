#pragma once
#include <Arduino.h>
#include <vector>

namespace piSync {
#ifdef ARDUINO_ARCH_ESP32
void begin();
bool paired();
bool service(bool recording, bool pickerOpen);
bool keepRadio();
bool needsWake();
void request();
void command(char *args, Print &out);
const char *statusText();
unsigned pending();
void recordingSaved();
const std::vector<int>& ids(bool today, bool completed);
const char *title(int i);
const char *detail(int i);
const char *deadline(int i);
bool done(int i);
bool change(int i,bool promote);
#else
inline void begin() {}
bool paired();
bool service(bool,bool);
inline bool keepRadio() { return false; }
inline bool needsWake() { return false; }
inline void request() {}
inline void command(char *,Print &) {}
inline const char *statusText() { return ""; }
inline unsigned pending() { return 0; }
inline void recordingSaved() {}
#endif
}
