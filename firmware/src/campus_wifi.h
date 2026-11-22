#pragma once
#include <cstdint>

class Print;

bool campusOnline();
uint32_t campusNetworkTime();
// Automatic sessions end soon after network time arrives; manual ones (USB helper) last until `wifi off`.
void startSavedCampusWifi(bool manual=false);
void startCampusWifiOnWake();
void startCampusWifiAfterRecording(); // Both automatic starts back off after failed logins.
void beginCampusDiagnostics(); // Call first in setup().
void diagLoopPhase(uint8_t phase); // Main-loop breadcrumb for `wifi diag`.
void stopCampusWifi();
bool serviceCampusWifi();
void campusCommand(char *command, Print &out);
