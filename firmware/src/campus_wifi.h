#pragma once
#include <Arduino.h>

bool campusOnline();
uint32_t campusNetworkTime();
// Automatic sessions end soon after network time arrives; manual ones (USB helper) last until `wifi off`.
void startSavedCampusWifi(bool manual=false);
void startCampusWifiOnWake();
void beginCampusDiagnostics(); // Call first in setup().
void stopCampusWifi();
bool serviceCampusWifi();
void campusCommand(char *command, Print &out);
