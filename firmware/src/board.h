#pragma once
#include <Arduino.h>

constexpr int plusPin = 4, nextPin = 0, okPin = 5;
void boardBegin();
void releaseBatteryPower();
void setBacklight(uint8_t percent);
// Sleep runs the CPU at 80 MHz. USB, Wi-Fi, I2S and the display use their own clocks.
void setLowPower(bool low);
uint32_t displayFrames(); // Completed screen refreshes since boot.
int batteryPercent();
bool charging();
enum class Contact { Unavailable, Released, Pressed };
Contact readContact(int &x, int &y);
