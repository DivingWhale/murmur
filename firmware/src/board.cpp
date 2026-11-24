#include "board.h"
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <lvgl.h>
#include <atomic>

namespace {
constexpr int batteryEnable = 2;
constexpr int cs = 21, dc = 45, sclk = 38, mosi = 39, lcdReset = 40, backlight = 46, touchReset = 47;
// Two DMA buffers: LVGL draws the next strip while the previous one is on the wire.
// 40 MHz is the clock the original blocking driver used on this panel.
constexpr int bufferLines = 40;
constexpr unsigned pixelClock = 40 * 1000 * 1000;
esp_lcd_panel_io_handle_t panel;
lv_disp_draw_buf_t drawBuffer;
lv_disp_drv_t display;
std::atomic<uint32_t> frames{0};
uint8_t backlightLevel;

bool transferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *driver) {
    lv_disp_flush_ready(static_cast<lv_disp_drv_t *>(driver)); // ISR-safe: only sets a flag.
    return false;
}
void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *buf) {
    const uint8_t xs[] = {0, uint8_t(area->x1), 0, uint8_t(area->x2)};
    const uint8_t ys[] = {0, uint8_t(area->y1), 0, uint8_t(area->y2)};
    esp_lcd_panel_io_tx_param(panel, 0x2a, xs, sizeof(xs));
    esp_lcd_panel_io_tx_param(panel, 0x2b, ys, sizeof(ys));
    esp_lcd_panel_io_tx_color(panel, 0x2c, buf, lv_area_get_size(area) * sizeof(lv_color_t));
    if (lv_disp_flush_is_last(driver)) ++frames;
}
bool beginPanelBus() {
    spi_bus_config_t bus{};
    bus.sclk_io_num = sclk;
    bus.mosi_io_num = mosi;
    bus.miso_io_num = bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 240 * bufferLines * sizeof(lv_color_t);
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    esp_lcd_panel_io_spi_config_t io{};
    io.cs_gpio_num = cs;
    io.dc_gpio_num = dc;
    io.spi_mode = 3;
    io.pclk_hz = pixelClock;
    io.trans_queue_depth = 4;
    io.on_color_trans_done = transferDone;
    io.user_ctx = &display;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    return esp_lcd_new_panel_io_spi(reinterpret_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &io, &panel) == ESP_OK;
}
void command(uint8_t cmd, const uint8_t *data = nullptr, unsigned len = 0) {
    esp_lcd_panel_io_tx_param(panel, cmd, data, len);
}
}

void boardBegin() {
    pinMode(batteryEnable, OUTPUT);
    digitalWrite(batteryEnable, HIGH);
    for (int pin : {plusPin, nextPin, okPin, 3}) pinMode(pin, INPUT_PULLUP);
    for (int pin : {lcdReset, backlight, touchReset, 7}) pinMode(pin, OUTPUT);
    digitalWrite(7, LOW); // Speaker amplifier
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
    beginPanelBus();
    command(0x11); // Sleep out
    delay(120);
    const uint8_t mode = 0x55, orientation = 0;
    command(0x3a, &mode, 1);
    command(0x36, &orientation, 1);
    command(0x21); // IPS panel inversion
    command(0x29);
    lv_init();
    const size_t bytes = 240 * bufferLines * sizeof(lv_color_t);
    auto *a = static_cast<lv_color_t *>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    auto *b = static_cast<lv_color_t *>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    lv_disp_draw_buf_init(&drawBuffer, a, b, 240 * bufferLines);
    lv_disp_drv_init(&display);
    display.hor_res = display.ver_res = 240;
    display.flush_cb = flush;
    display.draw_buf = &drawBuffer;
    lv_disp_drv_register(&display);
}
uint32_t displayFrames() { return frames.load(); }
void setLowPower(bool low) {
    const uint32_t mhz=low?80:240;
    if (getCpuFrequencyMhz()!=mhz) setCpuFrequencyMhz(mhz);
}
uint32_t cpuMhz() { return getCpuFrequencyMhz(); }
void setBacklight(uint8_t percent) {
    backlightLevel=percent;
    ledcWrite(0,(percent*255+50)/100);
}
uint8_t backlightPercent() { return backlightLevel; }
void releaseBatteryPower() { digitalWrite(batteryEnable, LOW); }
bool charging() { return digitalRead(3) == LOW; }
int batteryMillivolts() { return analogReadMilliVolts(1)*3; } // 1:3 divider on GPIO1
int batteryPercent() {
    static const int mv[] = {3200,3400,3500,3600,3700,3770,3850,3950,4060,4200};
    static const int pc[] = {0,3,8,15,30,50,65,80,92,100};
    int v = batteryMillivolts();
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
