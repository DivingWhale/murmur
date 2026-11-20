// Host stand-in for the parts of Arduino-ESP32 the UI uses. Simulator only.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/time.h>
#include <time.h>

// Virtual time and the system clock are owned by the simulator.
uint32_t millis();
void delay(uint32_t ms);
int sim_settimeofday(const struct timeval *tv, const void *);
time_t sim_time(time_t *out);
#define settimeofday(a, b) sim_settimeofday(a, b)
#define time(p) sim_time(p)

constexpr int LOW = 0, HIGH = 1, INPUT_PULLUP = 2, OUTPUT = 3;
int digitalRead(int pin);
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}

using std::max;
using std::min;
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

class String {
public:
    String() = default;
    String(const char *s) : s_(s ? s : "") {}
    String(int v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned v) : s_(std::to_string(v)) {}
    String(double v, int decimals) { char b[48]; snprintf(b, sizeof(b), "%.*f", decimals, v); s_ = b; }
    String &operator+=(const char *s) { s_ += s; return *this; }
    String &operator+=(const String &o) { s_ += o.s_; return *this; }
    friend String operator+(String a, const char *b) { a += b; return a; }
    friend String operator+(String a, const String &b) { a += b; return a; }
    const char *c_str() const { return s_.c_str(); }
    bool startsWith(const char *p) const { return s_.rfind(p, 0) == 0; }
private:
    std::string s_;
};

class Print {
public:
    virtual ~Print() = default;
    virtual void write(const char *s) { fputs(s, stdout); }
    void print(const char *s) { write(s); }
    void println(const char *s = "") { write(s); write("\n"); }
    int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[512]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof(b), fmt, a); va_end(a);
        write(b); return n;
    }
};
