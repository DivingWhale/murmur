#include "board.h"
#include <SPI.h>
#include <Wire.h>
#include <lvgl.h>

namespace {
constexpr int batteryEnable = 2;
constexpr int cs = 21, dc = 45, lcdReset = 40, backlight = 46, touchReset = 47;
lv_color_t pixels[240 * 16];
lv_disp_draw_buf_t drawBuffer;
lv_disp_drv_t display;

void command(uint8_t cmd, const uint8_t *data = nullptr, unsigned len = 0) {
    digitalWrite(dc, LOW);
    SPI.transfer(cmd);
    digitalWrite(dc, HIGH);
    if (len) SPI.writeBytes(data, len);
}
void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *buf) {
    uint8_t xs[] = {0, uint8_t(area->x1), 0, uint8_t(area->x2)};
    uint8_t ys[] = {0, uint8_t(area->y1), 0, uint8_t(area->y2)};
    SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE3));
    digitalWrite(cs, LOW);
    command(0x2a, xs, sizeof(xs));
    command(0x2b, ys, sizeof(ys));
    command(0x2c);
    SPI.writeBytes(reinterpret_cast<uint8_t *>(buf), lv_area_get_size(area)*2);
    digitalWrite(cs, HIGH);
    SPI.endTransaction();
    lv_disp_flush_ready(driver);
}
}

void boardBegin() {
    pinMode(batteryEnable, OUTPUT);
    digitalWrite(batteryEnable, HIGH);
    for (int pin : {plusPin, nextPin, okPin, 3}) pinMode(pin, INPUT_PULLUP);
    for (int pin : {cs, dc, lcdReset, backlight, touchReset, 7}) pinMode(pin, OUTPUT);
    digitalWrite(7, LOW); // Speaker amplifier
    digitalWrite(cs, HIGH);
    ledcSetup(0,5000,8);
    ledcAttachPin(backlight,0);
    setBacklight(0);
    digitalWrite(lcdReset, LOW);
    digitalWrite(touchReset, LOW);
    delay(15);
    digitalWrite(lcdReset, HIGH);
    digitalWrite(touchReset, HIGH);
    delay(120);
    Wire.begin(42, 41, 400000);
    SPI.begin(38, -1, 39, cs);
    SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE3));
    digitalWrite(cs, LOW);
    command(0x11); // Sleep out
    delay(120);
    const uint8_t mode = 0x55, orientation = 0;
    command(0x3a, &mode, 1);
    command(0x36, &orientation, 1);
    command(0x21); // IPS panel inversion
    command(0x29);
    digitalWrite(cs, HIGH);
    SPI.endTransaction();
    lv_init();
    lv_disp_draw_buf_init(&drawBuffer, pixels, nullptr, 240*16);
    lv_disp_drv_init(&display);
    display.hor_res = display.ver_res = 240;
    display.flush_cb = flush;
    display.draw_buf = &drawBuffer;
    lv_disp_drv_register(&display);
}
void setBacklight(uint8_t percent) { ledcWrite(0,(percent*255+50)/100); }
void releaseBatteryPower() { digitalWrite(batteryEnable, LOW); }
bool charging() { return digitalRead(3) == LOW; }
int batteryPercent() {
    static const int mv[] = {3200,3400,3500,3600,3700,3770,3850,3950,4060,4200};
    static const int pc[] = {0,3,8,15,30,50,65,80,92,100};
    int v = analogReadMilliVolts(1)*3;
    if (v < 2600 || v > 4500) return -1;
    for (unsigned i = 1; i < sizeof(mv)/sizeof(*mv); ++i)
        if (v <= mv[i]) return constrain(map(v,mv[i-1],mv[i],pc[i-1],pc[i]),0L,100L);
    return 100;
}
Contact readContact(int &x, int &y) {
    Wire.beginTransmission(0x15);
    Wire.write(2);
    if (Wire.endTransmission(false) || Wire.requestFrom(uint8_t(0x15),uint8_t(5)) != 5)
        return Contact::Unavailable;
    int count = Wire.read() & 15;
    int xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
    if (!count) return Contact::Released;
    x = ((xh & 15)<<8) | xl;
    y = ((yh & 15)<<8) | yl;
    return count == 1 && x < 240 && y < 240 ? Contact::Pressed : Contact::Unavailable;
}
