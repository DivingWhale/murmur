#pragma once
#include <stdint.h>

class USBCDC;
void beginCardUsb(USBCDC &serial);
void takeCardForRecording();
bool tryTakeCardForLocalFiles();
void releaseCardAfterRecording();
int64_t cardFreeBytes();
// Sample USB progress from the diagnostic worker without taking the card mutex.
bool sampleUsbProgress(uint32_t now);

namespace cardPins {
constexpr int clk=16, cmd=15, d0=17, d1=18, d2=13, d3=14;
}
