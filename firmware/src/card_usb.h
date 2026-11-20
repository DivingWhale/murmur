#pragma once
#include <stdint.h>

class USBCDC;
void beginCardUsb(USBCDC &serial);
void takeCardForRecording();
bool tryTakeCardForLocalFiles();
void releaseCardAfterRecording();
int64_t cardFreeBytes();

namespace cardPins {
constexpr int clk=16, cmd=15, d0=17, d1=18, d2=13, d3=14;
}
