#pragma once
#include <cstdint>

// Battery consumption log, for working out what drains the battery.
//
// Every minute it stores the battery voltage and how that minute was spent:
// screen awake, CPU at 240 MHz, mean backlight, Wi-Fi radio on, recording,
// USB host attached, charging and display refreshes. Samples stay in PSRAM
// (12 h). A running summary goes to NVS every 10 minutes, so a run that ends
// with a flat battery can still be read after the next start (`power last`).
class Print;
void beginPowerLog();
void powerLogTick(uint32_t now, bool awake, bool recording);
void powerCommand(const char *args, Print &out); // "", "log", "last"
