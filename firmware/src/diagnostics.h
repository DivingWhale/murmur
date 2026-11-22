#pragma once

#include <stdint.h>

namespace diagnostics {

enum class Event : uint16_t {
    Boot, WorkerError, StorageError, ExportAborted, ClockSet, LoopSlow, Sleep, Wake,
    UsbAttached, UsbDetached, CardReady, CardReadError,
    WifiProfile, WifiJoin, WifiJoinError, WifiLinkUp, WifiLinkDown, WifiGotIp,
    WifiFailed, SntpQueued, SntpStarted, SntpSynced, SntpStopped,
    WifiStopBegin, WifiDisconnect, WifiEapDisabled, WifiRadioOff,
};

void begin(uint32_t previousPhase);
void startWorker();
void record(Event event, int32_t value=0, int32_t detail=0);
void requestCheckpoint();
void command(const char *name);
bool exporting();

}
