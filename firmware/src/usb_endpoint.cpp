// Measure active IN stalls without touching the controller's DMA state.
#include "diagnostics.h"
#include <esp32-hal-tinyusb.h>
#include <esp_timer.h>
#include <soc/usb_reg.h>
#include <soc/usb_struct.h>

extern "C" void __real_dcd_edpt_stall(uint8_t port, uint8_t address);

extern "C" void __wrap_dcd_edpt_stall(uint8_t port, uint8_t address) {
    const uint8_t n=tu_edpt_number(address);
    if (tu_edpt_dir(address)!=TUSB_DIR_IN || !n || !(USB0.in_ep_reg[n].diepctl & USB_D_EPENA1_M)) {
        __real_dcd_edpt_stall(port,address);
        return;
    }
    const int64_t start=esp_timer_get_time();
    diagnostics::record(diagnostics::Event::UsbInStallBegin,address,USB0.in_ep_reg[n].diepint);
    __real_dcd_edpt_stall(port,address);
    diagnostics::record(diagnostics::Event::UsbInStallEnd,address,esp_timer_get_time()-start);
}
