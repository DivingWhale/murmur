#include "card_usb.h"
#include <Arduino.h>
#include <USB.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <esp32-hal-tinyusb.h>
#include <ff.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <atomic>
#include <memory>

namespace {
SemaphoreHandle_t gate;
TaskHandle_t probeTask;
sdmmc_card_t card;
bool rawOpen, recording, ejected, usbConnected;
std::atomic<uint32_t> visibleBlocks{0}, mediumChanged{0};
uint32_t retryAt, lastProbe, usbAttachedAt;
alignas(4) uint8_t sector[512];

struct CardLock {
    CardLock() { xSemaphoreTake(gate,portMAX_DELAY); }
    ~CardLock() { xSemaphoreGive(gate); }
};
void closeRaw() {
    visibleBlocks.store(0);
    if (rawOpen) sdmmc_host_deinit();
    rawOpen=false;
}
bool openRaw() {
    // Called with the lock held. Read sectors without mounting or formatting FAT.
    if (sdmmc_host_init()!=ESP_OK) return false;
    sdmmc_host_t host=SDMMC_HOST_DEFAULT();
    host.slot=SDMMC_HOST_SLOT_1;
    host.flags=SDMMC_HOST_FLAG_4BIT;
    host.max_freq_khz=SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot=SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width=4;
    slot.clk=gpio_num_t(cardPins::clk); slot.cmd=gpio_num_t(cardPins::cmd);
    slot.d0=gpio_num_t(cardPins::d0); slot.d1=gpio_num_t(cardPins::d1);
    slot.d2=gpio_num_t(cardPins::d2); slot.d3=gpio_num_t(cardPins::d3);
    if (sdmmc_host_init_slot(host.slot,&slot)==ESP_OK &&
        sdmmc_card_init(&host,&card)==ESP_OK && card.csd.sector_size==sizeof(sector)) {
        rawOpen=true;
        lastProbe=millis();
        return true;
    }
    sdmmc_host_deinit();
    return false;
}
bool ready() {
    if (recording || ejected) return false;
    uint32_t now=millis();
    if (rawOpen && now-lastProbe>=1000) {
        lastProbe=now;
        if (sdmmc_get_status(&card)!=ESP_OK) {
            closeRaw();
            mediumChanged=true;
            retryAt=now+1200;
        }
    }
    if (rawOpen) return true;
    if (int32_t(now-retryAt)<0) return false;
    retryAt=now+1200;
    return openRaw();
}
uint16_t descriptor(uint8_t *dst,uint8_t *interfaceNumber) {
    uint8_t ep=tinyusb_get_free_duplex_endpoint();
    if (!ep) return 0;
    uint8_t str=tinyusb_add_string_descriptor("Murmur TF card");
    uint8_t bytes[]={TUD_MSC_DESCRIPTOR(*interfaceNumber,str,ep,uint8_t(0x80|ep),64)};
    ++*interfaceNumber;
    memcpy(dst,bytes,sizeof(bytes));
    return sizeof(bytes);
}
void connectionEvent(void *,esp_event_base_t,int32_t id,void *) {
    CardLock lock;
    ejected=false;
    usbConnected=id==ARDUINO_USB_STARTED_EVENT;
    visibleBlocks.store(0);
    if (usbConnected) usbAttachedAt=millis();
    else closeRaw();
}
void probeMedia(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        CardLock lock;
        if (!usbConnected || millis()-usbAttachedAt<4000) continue;
        if (ready() && !visibleBlocks.load()) {
            mediumChanged.store(1);
            visibleBlocks.store(card.csd.capacity);
        }
    }
}
uint32_t mediaCapacity() {
    // A card can take seconds to initialize. Keep the USB control and CDC
    // endpoints responsive while Windows polls this removable reader.
    xTaskNotifyGive(probeTask);
    return visibleBlocks.load();
}
}

void beginCardUsb(USBCDC &serial) {
    gate=xSemaphoreCreateMutex();
    if (gate && xTaskCreatePinnedToCore(probeMedia,"sd-probe",4096,nullptr,1,&probeTask,xPortGetCoreID())==pdPASS) {
        USB.onEvent(ARDUINO_USB_STARTED_EVENT,connectionEvent);
        USB.onEvent(ARDUINO_USB_STOPPED_EVENT,connectionEvent);
        tinyusb_enable_interface(USB_INTERFACE_MSC,TUD_MSC_DESC_LEN,descriptor);
    }
    serial.setRxBufferSize(512);
    serial.begin(115200);
    // Espressif's TinyUSB development PID bitmap: CDC bit 0, MSC bit 1.
    // The ROM's 0x1001 identifies a different set of interfaces (Serial/JTAG).
    USB.PID(probeTask ? 0x4003 : 0x4001);
    USB.productName("Murmur");
    if (USB.begin()) {
        // Give the host a detach interval between the ROM USB controller and TinyUSB.
        tud_disconnect();
        delay(750);
        tud_connect();
    }
}
void takeCardForRecording() {
    tryTakeCardForLocalFiles();
}
bool tryTakeCardForLocalFiles() {
    if (!gate) return false;
    CardLock lock;
    if (recording) return false;
    recording=true;
    closeRaw(); // In-flight block reads finish before the recorder mounts FAT.
    return true;
}
void releaseCardAfterRecording() {
    if (!gate) return;
    CardLock lock;
    recording=false;
    mediumChanged=true;
    retryAt=millis()+1200;
}

int64_t cardFreeBytes() {
    if (!gate) return -1;
    CardLock lock;
    BYTE drive;
    if (!ready() || ff_diskio_get_drive(&drive)!=ESP_OK) return -1;
    // FATFS holds a 4 KiB window. Keep it off the 8 KiB UI task stack.
    std::unique_ptr<FATFS> volume(new (std::nothrow) FATFS{});
    if (!volume) return -1;
    // Share the initialized card with MSC. Only inspect FAT, then drop the mapping.
    const char path[]={char('0'+drive),':',0};
    ff_diskio_register_sdmmc(drive,&card);
    FATFS *fs=nullptr;
    DWORD clusters;
    int64_t bytes=-1;
    if (f_mount(volume.get(),path,1)==FR_OK && f_getfree(path,&clusters,&fs)==FR_OK)
        bytes=int64_t(clusters)*fs->csize*card.csd.sector_size;
    f_mount(nullptr,path,0);
    ff_diskio_unregister(drive);
    return bytes;
}

extern "C" {
void tud_msc_inquiry_cb(uint8_t,uint8_t vendor[8],uint8_t product[16],uint8_t rev[4]) {
    memcpy(vendor,"Murmur  ",8); memcpy(product,"TF Card Reader  ",16); memcpy(rev,"1.0 ",4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!mediaCapacity()) {
        tud_msc_set_sense(lun,SCSI_SENSE_NOT_READY,0x3a,0);
        return false;
    }
    if (mediumChanged.exchange(0)) {
        tud_msc_set_sense(lun,SCSI_SENSE_UNIT_ATTENTION,0x28,0);
        return false;
    }
    return true;
}
void tud_msc_capacity_cb(uint8_t,uint32_t *blocks,uint16_t *size) {
    *blocks=mediaCapacity();
    *size=sizeof(sector);
}
bool tud_msc_is_writable_cb(uint8_t) { return false; }
int32_t tud_msc_write10_cb(uint8_t lun,uint32_t,uint32_t,uint8_t *,uint32_t) {
    tud_msc_set_sense(lun,SCSI_SENSE_DATA_PROTECT,0x27,0);
    return -1;
}
int32_t tud_msc_read10_cb(uint8_t lun,uint32_t lba,uint32_t offset,void *buffer,uint32_t length) {
    CardLock lock;
    if (!rawOpen || !visibleBlocks.load()) {
        tud_msc_set_sense(lun,SCSI_SENSE_NOT_READY,0x3a,0);
        return -1;
    }
    uint64_t pos=uint64_t(lba)*sizeof(sector)+offset;
    if (pos+length>uint64_t(card.csd.capacity)*sizeof(sector)) {
        tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x21,0);
        return -1;
    }
    auto *out=static_cast<uint8_t *>(buffer);
    uint32_t left=length;
    while (left) {
        unsigned skip=pos%sizeof(sector);
        uint32_t count=left/sizeof(sector);
        bool whole=skip==0 && count>0;
        size_t n=whole?count*sizeof(sector):std::min<size_t>(left,sizeof(sector)-skip);
        if (sdmmc_read_sectors(&card,whole?out:sector,pos/sizeof(sector),whole?count:1)!=ESP_OK) {
            closeRaw();
            mediumChanged=true;
            retryAt=millis()+1200;
            tud_msc_set_sense(lun,SCSI_SENSE_MEDIUM_ERROR,0x11,0);
            return -1;
        }
        if (!whole) memcpy(out,sector+skip,n);
        out+=n; pos+=n; left-=n;
    }
    return length;
}
bool tud_msc_start_stop_cb(uint8_t,uint8_t,bool start,bool loadEject) {
    if (loadEject) {
        CardLock lock;
        ejected=!start;
        if (ejected) closeRaw();
    }
    return true;
}
int32_t tud_msc_scsi_cb(uint8_t lun,const uint8_t cmd[16],void *,uint16_t) {
    if (cmd[0]==SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL || cmd[0]==0x35 /* SYNCHRONIZE CACHE */) return 0;
    tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x20,0);
    return -1;
}
}
