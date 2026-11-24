// Controller layer from TinyUSB 0.18. The Arduino SDK owns the device and class layers.
#define CFG_TUSB_CONFIG_FILE "sdkconfig.h"
#define CFG_TUSB_MCU OPT_MCU_ESP32S3
#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_DWC2_DMA_ENABLE 1
#define CFG_TUD_DWC2_SLAVE_ENABLE 0
#define dcd_init upstream_dcd_init
#define dcd_edpt_open upstream_dcd_edpt_open
#include "portable/synopsys/dwc2/dwc2_common.c"
#include "portable/synopsys/dwc2/dcd_dwc2.c"
#undef dcd_init
#undef dcd_edpt_open

#if TUSB_VERSION_MAJOR != 0 || TUSB_VERSION_MINOR != 18
#error The controller requires its pinned TinyUSB headers
#endif

// The SDK's control, CDC and MSC endpoint buffers live in internal DMA SRAM.
// The S3 has no data cache on this region. TinyUSB 0.16 lacks these 0.18 hooks.
bool dcd_dcache_clean(const void *addr, uint32_t size) {
    (void)addr;
    (void)size;
    return true;
}

bool dcd_dcache_invalidate(const void *addr, uint32_t size) {
    (void)addr;
    (void)size;
    return true;
}

// Arduino-ESP32 2.0.17 uses the TinyUSB 0.16 device-controller entry point.
void dcd_init(uint8_t port) {
    const tusb_rhport_init_t device = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    upstream_dcd_init(port, &device);
}

bool dcd_edpt_open(uint8_t port, tusb_desc_endpoint_t const *endpoint) {
    // Arduino CDC advertises 0x85 for notifications but never sends one. On the
    // S3 the four non-control TX FIFOs are available to endpoints 1 through 4.
    if (endpoint->bEndpointAddress == 0x85 && endpoint->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
        return true;
    }
    return upstream_dcd_edpt_open(port, endpoint);
}
