#pragma once
#include <Arduino.h>

bool campusOnline();
uint32_t campusNetworkTime();
void startSavedCampusWifi();
void stopCampusWifi();
bool serviceCampusWifi();
void campusCommand(char *command, Print &out);
