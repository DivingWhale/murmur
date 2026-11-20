#include "audio.h"
#include "card_usb.h"
#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <driver/i2s.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

namespace {
constexpr int rate = 16000;
int file = -1;
uint32_t bytesWritten, lastAudio;
uint8_t level;
char partial[80], finalPath[80];
bool micRunning;
uint64_t slotEnergy[2];
uint32_t slotFrames;
constexpr float speechGain=12.0f;
float captureGain;

int16_t amplifySpeech(int16_t sample) {
    // Recover in about 180 ms after a tap. A transient reduces gain immediately.
    constexpr float ceiling=29200.0f; // about -1 dBFS
    captureGain+=(speechGain-captureGain)/(0.18f*rate);
    const int magnitude=abs(int(sample));
    if (magnitude*captureGain>ceiling) captureGain=ceiling/magnitude;
    return int16_t(lrintf(sample*captureGain));
}

bool writeCodec(uint8_t reg,uint8_t value) {
    Wire.beginTransmission(0x40);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission()==0;
}

void powerDownMic() {
    writeCodec(0x4b,0xff);
    writeCodec(0x4c,0xff);
    writeCodec(0x40,0xc0);
    writeCodec(0x01,0x7f);
    writeCodec(0x06,0x07);
}

bool beginMic() {
    // ES7210 slave, standard stereo I2S. Both onboard mics use MICBIAS12.
    // ADC3 is the speaker reference. Only power the MIC1/2 recording path.
    static const uint8_t regs[][2] = {
        {0x00,0xff},{0x00,0x41},{0x01,0x3f},
        {0x09,0x30},{0x0a,0x30},{0x23,0x2a},{0x22,0x0a},
        {0x21,0x2a},{0x20,0x0a},{0x11,0x60},{0x12,0},
        {0x07,0x20},{0x02,0xc1},{0x04,1},{0x05,0},
        {0x01,0x34},{0x06,0},{0x40,0x43},{0x41,0x70},
        {0x47,8},{0x48,8},{0x4b,0},{0x4c,0xff},
        {0x43,0x1a},{0x44,0x1a}, // 30 dB analog gain on MIC1 and MIC2
        {0x00,0x71},{0x00,0x41}
    };
    i2s_config_t config{};
    config.mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = rate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.dma_buf_count = 12;
    config.dma_buf_len = 256;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins{};
    pins.mck_io_num = 8;
    pins.bck_io_num = 9;
    pins.ws_io_num = 10;
    pins.data_out_num = -1;
    pins.data_in_num = 11;
    if (i2s_set_pin(I2S_NUM_0,&pins)!=ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    for (auto &reg : regs) {
        if (!writeCodec(reg[0],reg[1])) {
            powerDownMic();
            i2s_driver_uninstall(I2S_NUM_0);
            return false;
        }
    }
    micRunning=true;
    return true;
}
void closeSession() {
    if (micRunning) {
        powerDownMic();
        i2s_driver_uninstall(I2S_NUM_0);
    }
    micRunning = false;
    if (file >= 0) close(file);
    file = -1;
    SD_MMC.end();
    releaseCardAfterRecording();
}
struct WavHeader {
    char riff[4]; uint32_t riffSize; char wave[4], fmt[4]; uint32_t fmtSize;
    uint16_t pcm, channels; uint32_t sampleRate, byteRate; uint16_t align, bits;
    char data[4]; uint32_t dataSize;
};
static_assert(sizeof(WavHeader) == 44, "PCM WAV header");
bool writeHeader() {
    WavHeader h{{'R','I','F','F'}, 36+bytesWritten, {'W','A','V','E'}, {'f','m','t',' '}, 16,
                1, 1, rate, rate*2, 2, 16, {'d','a','t','a'}, bytesWritten};
    return lseek(file,0,SEEK_SET) == 0 && write(file,&h,sizeof(h)) == sizeof(h);
}
}

AudioResult startRecording() {
    bytesWritten = level = 0;
    captureGain=speechGain;
    slotFrames=0;
    slotEnergy[0]=slotEnergy[1]=0;
    takeCardForRecording();
    SD_MMC.setPins(cardPins::clk,cardPins::cmd,cardPins::d0,cardPins::d1,cardPins::d2,cardPins::d3);
    if (!SD_MMC.begin("/sdcard",false,false) ||
        (!SD_MMC.exists("/murmur") && !SD_MMC.mkdir("/murmur"))) {
        closeSession();
        return AudioResult::Card;
    }
    if (!beginMic()) {
        closeSession();
        return AudioResult::Mic;
    }
    // O_EXCL protects interrupted files. A random suffix also works before clock sync.
    for (int attempt = 0; attempt < 8; ++attempt) {
        time_t now=time(nullptr);
        char stamp[24]="unset";
        if (now>=1704067200) {
            struct tm local{};
            localtime_r(&now,&local);
            strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",&local);
        }
        snprintf(finalPath,sizeof(finalPath),"/sdcard/murmur/rec-%s-%08lx.wav",stamp,(unsigned long)esp_random());
        snprintf(partial,sizeof(partial),"%s.part", finalPath);
        struct stat info;
        if (stat(finalPath,&info) == 0) continue;
        file = open(partial,O_WRONLY|O_CREAT|O_EXCL,0666);
        if (file >= 0 || errno != EEXIST) break;
    }
    if (file < 0 || !writeHeader()) {
        closeSession();
        return AudioResult::Write;
    }
    lastAudio = millis();
    return AudioResult::Ok;
}
AudioResult serviceRecording() {
    int16_t samples[512], mono[256];
    size_t n = 0;
    if (i2s_read(I2S_NUM_0,samples,sizeof(samples),&n,0) == ESP_OK && n >= 4) {
        size_t frames = n/4;
        int peak = 0;
        for (size_t i = 0; i < frames; ++i) {
            mono[i] = amplifySpeech(samples[2*i]);
            peak = max(peak,abs(int(mono[i])));
            for (int slot=0;slot<2;++slot) {
                int sample=samples[2*i+slot];
                slotEnergy[slot]+=sample*sample;
            }
        }
        slotFrames+=frames;
        level = min(20,peak / 500);
        if (bytesWritten > 0xfff00000u || write(file,mono,frames*2) != ssize_t(frames*2)) {
            closeSession();
            return AudioResult::Write;
        }
        bytesWritten += frames*2;
        lastAudio = millis();
    } else if (millis()-lastAudio > 1000) {
        closeSession();
        return AudioResult::Mic;
    }
    return AudioResult::Ok;
}
AudioResult finishRecording() {
    bool ok = bytesWritten && writeHeader() && fsync(file) == 0;
    if (close(file) != 0) ok = false;
    file = -1;
    if (ok) ok = rename(partial,finalPath) == 0;
    closeSession();
    return ok ? AudioResult::Ok : AudioResult::Write;
}
uint32_t recordedSeconds() { return bytesWritten / (rate*2); }
bool discardRecording() {
    if (file < 0) return false;
    close(file);
    file = -1;
    // This session exclusively created partial. Never remove a finalized WAV.
    bool removed = unlink(partial) == 0;
    closeSession();
    return removed;
}
uint8_t audioLevel() { return level; }
void printMicLevels(Print &out) {
    out.printf("mic-speech-v3 frames=%lu",(unsigned long)slotFrames);
    for (int slot=0;slot<2;++slot) {
        double meanSquare=slotFrames ? double(slotEnergy[slot])/slotFrames : 0;
        out.printf(" slot%d_rms_dBFS=%.1f",slot,meanSquare>0 ? 10*log10(meanSquare/(32768.0*32768.0)) : -120.0);
    }
    out.println();
}
