#pragma once
#include <cstdint>
enum class AudioResult { Ok, Card, Mic, Write };
AudioResult startRecording();
AudioResult serviceRecording();
AudioResult finishRecording();
bool discardRecording();
uint32_t recordedSeconds();
uint8_t audioLevel();
class Print;
void printMicLevels(Print &out);
