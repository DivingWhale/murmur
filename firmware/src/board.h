#pragma once
#include <Arduino.h>

constexpr int plusPin = 4, nextPin = 0, okPin = 5;
void boardBegin();
void releaseBatteryPower();
void setBacklight(uint8_t percent);
uint32_t displayFrames(); // Completed screen refreshes since boot.
int batteryPercent();
bool charging();
enum class Contact { Unavailable, Released, Pressed };
Contact readContact(int &x, int &y);
