#pragma once
#include <Arduino.h>
// In-memory NVS. Starts empty, like a freshly erased board.
class Preferences {
public:
    bool begin(const char *, bool = false) { return true; }
    bool getBool(const char *k, bool d) { return get(k, d); }
    uint8_t getUChar(const char *k, uint8_t d) { return get(k, d); }
    uint16_t getUShort(const char *k, uint16_t d) { return get(k, d); }
    size_t putBool(const char *k, bool v) { return put(k, v, 1); }
    size_t putUChar(const char *k, uint8_t v) { return put(k, v, 1); }
    size_t putUShort(const char *k, uint16_t v) { return put(k, v, 2); }
private:
    long get(const char *k, long d);
    size_t put(const char *k, long v, size_t n);
};
