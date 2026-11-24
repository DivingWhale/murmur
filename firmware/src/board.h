#pragma once
#include <cstdint>

constexpr int plusPin = 4, nextPin = 0, okPin = 5;
void boardBegin();
void releaseBatteryPower();
void setBacklight(uint8_t percent);
uint8_t backlightPercent(); // Last level passed to setBacklight.
// 80 MHz instead of 240 MHz, for Sleep and for still screens. USB, Wi-Fi, I2S and the display
// use their own clocks.
void setLowPower(bool low);
uint32_t cpuMhz();
uint32_t displayFrames(); // Completed screen refreshes since boot.
int batteryMillivolts();
int batteryPercent();
bool charging();
enum class Contact { Unavailable, Released, Pressed };
Contact readContact(int &x, int &y);
