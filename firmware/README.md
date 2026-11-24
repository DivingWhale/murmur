# Murmur handheld preview

This image targets the **Waveshare ESP32-S3-Touch-LCD-1.54**, with the 240 × 240 ST7789 display, CST816 touch controller and 16 MiB flash. The preview starts in Chinese and includes sample tasks and lock-screen excerpts from the web prototype. Task completion, promotions into Today, display language, brightness and the auto-sleep interval persist in NVS. Recordings go to the removable card.

## Build

From the repository root, using Python 3.11:

```sh
python3 firmware/tools/build.py      # macOS / Linux
python firmware/tools/build.py       # Windows
```

The first build downloads PlatformIO, the ESP32-S3 cross compiler, Arduino-ESP32 and LVGL. The builder keeps the virtual environment, compiler, caches and scratch files under `work/`. PlatformIO keeps the project libraries and objects under `firmware/.pio/`. The merged image is `firmware/build/murmur-handheld.bin`. Git ignores all three directories. The checked-in application font pack is an input to the build.

## Flash and try it

Plug the board into USB. These commands run from the repository root. List serial ports first, then substitute the board's port.

On macOS or Linux (the port looks like `/dev/cu.usbmodem1101` on macOS):

```sh
py=work/venv/bin/python
tool=work/pio/packages/tool-esptoolpy/esptool.py
$py -m serial.tools.list_ports -v
$py $tool --chip esp32s3 --port /dev/cu.usbmodem1101 write_flash 0x0 firmware/build/murmur-handheld.bin
```

On Windows, in PowerShell, substitute the board's port for `COM7`:

```powershell
.\work\venv\Scripts\python.exe -m serial.tools.list_ports -v
$py = '.\work\venv\Scripts\python.exe'
$tool = '.\work\pio\packages\tool-esptoolpy\esptool.py'
& $py $tool --chip esp32s3 --port COM7 write_flash 0x0 .\firmware\build\murmur-handheld.bin
```

The merged image includes the bootloader and partition table. Flashing it replaces the factory application and may clear device settings. To keep a copy of the current 16 MiB flash, run the following before writing (on macOS, the same arguments with `$py $tool` and your `/dev/cu.*` port):

```powershell
& $py $tool --chip esp32s3 --port COM7 read_flash 0x0 0x1000000 .\work\original-flash.bin
```

The application uses Espressif's CDC + MSC development ID (`303A:4003`). The ROM loader uses `303A:1001`. Windows can assign each mode a different COM number. [Espressif USB descriptors](https://github.com/espressif/esp-usb/blob/master/device/esp_tinyusb/usb_descriptors.c)

The USB reader initially reports an empty slot. After USB configuration it waits at least four seconds before publishing the card. A background task probes the card when the host polls, so SD initialization and status checks do not run in the USB readiness or capacity callbacks. On the test PC, Windows mounted the volume about 22 seconds after the application COM port appeared. Leave the cable connected during this interval. USB mounting has not yet been tested on macOS. See [TESTING.md](TESTING.md) for update checks and the hardware log.

The ESP32-S3 uses TinyUSB 0.18's DWC2 controller in buffer DMA mode for control, CDC and MSC transfers. Arduino-ESP32 2.0.17 supplies the device and class layers. PlatformIO pins the controller source and compiles it through `src/usb_dwc2.c` with its matching headers. The adapter handles the SDK's initialization signature and Arduino's unused CDC notification endpoint. The endpoint buffers live in internal SRAM. Recheck the controller ABI, endpoint layout and sustained-read test when upgrading the SDK. [TinyUSB DWC2 driver](https://github.com/hathach/tinyusb/blob/86ad6e56c1700e85f1c5678607a762cfe3aa2f47/src/portable/synopsys/dwc2/dcd_dwc2.c)

If Windows loses USB and the Murmur interface still responds, hold PWR for three seconds, release it at the prompt, and unplug USB. Leave the battery installed. Reconnect USB with BOOT released to start the application again. The PWR gesture runs in Murmur firmware and is available while the application is responsive. The ROM download mode has its own recovery procedure. [Waveshare USB download instructions](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Instructions-For-Use)

| Board key | Preview action |
| --- | --- |
| PLUS (GPIO4) | Press and release to record. Press again to finalize the WAV. |
| During recording | Touch Stop & save or press PLUS to finish. Touch Cancel or press NEXT to open the discard prompt. Keep recording is selected initially. NEXT changes the selection and OK confirms it, or tap either option directly. |
| BOOT (GPIO0), called NEXT in the UI | Change selection or browse. Hold for 800 ms on Lock, Today, a page, or in display settings to turn off the backlight. Press to wake to Lock. |
| PWR (GPIO5), called OK in the UI | Unlock, check a task, or confirm a move. Hold for 800 ms on Today or a page to lock, on the lock screen to sleep, or within a sub-screen to go back. Continue holding to 3 seconds, then release at the prompt to power off. With USB connected, unplug the cable at the second prompt. The battery can stay installed. |

1. Try the UI with the card removed. Swipe up from Lock and drag upward within the task list to reveal the third task. Drag down to return, then tap a task to check it off. Horizontal swipes in the task area open the Claude and MD pages. Swipe sideways inside the bottom strip and tap it to move the displayed task into Today.
2. Press PLUS with the card removed to check the error screen. Insert a writable FAT32 microSD, press PLUS, speak for a few seconds, then press PLUS again. Wait for Saved before removing the card. Check `/murmur/*.wav` on a computer and listen to the recording. Audio is 16 kHz, signed 16-bit mono PCM. A file ending in `.part` is an interrupted capture.
3. Restart to check task persistence. Open a 115200-baud USB serial monitor and send `en` or `zh` followed by a newline to switch languages. Send `time <Unix timestamp>` to set the clock from a trusted source. The preview uses Asia/Shanghai time. Until the first time setting, the clock shows dashes and recording names use `unset`.
4. From Today, Claude or MD, swipe down from the header to open the quick panel. Tap Language to switch between English and Chinese. Tap or drag the brightness bar to set the backlight from 5% to 100%. All settings opens the scrollable list, including the auto-sleep interval, card free space and battery. NEXT moves the highlight and OK activates the row. In Brightness, NEXT steps by 5%. The default settings are 80% brightness and one minute until sleep.

For a quiet recording, send `levels` over USB serial after saving. The device reports the most recent capture's frame count and raw RMS level in dBFS for each I2S slot. The mono WAV uses slot 0 with up to 12× digital gain. A peak limiter keeps button impacts below −1 dBFS and releases with a 180 ms time constant. Both microphone inputs use 30 dB analog gain and share the MICBIAS12 supply. The board routes the playback reference to ADC3. [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf)

## Browse the TF card over USB

Connect a USB data cable and open the removable drive in File Explorer. Windows lists the USB disk as **Murmur TF Card Reader** and assigns a drive letter. The root contains the whole card, including `murmur/rec-*.wav`. You can play and copy files. The USB volume is read-only. The USB serial console remains available on a separate interface and its COM number can change after updating.

Finish any file copy before starting a recording. Pressing PLUS gives the recorder exclusive use of the card, so a transfer still in progress can stop. Once recording finishes, the firmware releases the card and signals a media change to the host. Connecting USB during a recording also waits for local file closure. Refresh File Explorer after saving, or reconnect the cable after explicitly ejecting the drive.

Provision HKUSTGZ once with the [USB campus login helper](../docs/campus-wifi.md#getting-credentials-onto-the-device). Online sessions automatically synchronize network time. After a fully dated login the device remembers the campus server certificate and can recover time following a cold shutdown, then reconnect with full date validation. Settings → Clock and USB provide a fallback after a certificate change. Recording and task controls work offline. After [pairing with the Pi](../backend/README.md), new recordings upload over TLS and extracted tasks sync back to a persistent card cache. The [Obsidian PC bridge](../pc_bridge/README.md) synchronizes tasks, memos and lock-screen selection through the Pi. The Claude page shows its empty state. Sleep turns off the backlight and lowers the CPU clock. While awake, the CPU also drops to 80 MHz once the screen is still and returns to 240 MHz on touch, key presses, screen changes or animations.

## Battery consumption log

The firmware samples battery use once a minute from the moment it starts, with no setup. Each sample records the battery voltage at the end of the minute and how many seconds of that minute were spent with the screen awake, the CPU at 240 MHz, the Wi-Fi radio on, recording, a USB host attached and charging. It also records the mean backlight level and the number of display refreshes. The last 12 hours stay in PSRAM. Every 10 minutes a summary of the run is also saved to NVS, so it survives a flat battery.

Use these commands over USB serial.

| Command | Reply |
|---|---|
| `power` | Live voltage, charge state, CPU clock, backlight and Wi-Fi, then `power run …` totals since start |
| `power log` | CSV of the per-minute samples, ending with `power end` |
| `power last` | Summary of the previous run (up to its last 10-minute save) and `ended_by=`, the reset reason of the current start. Check the saved voltage alongside that reason when investigating a shutdown |

USB powers the board while it is attached, so drain is only meaningful in minutes with `usb_s=0` and `charge_s=0`. To measure, charge fully, unplug, and use the device normally or leave it in one state. Then either plug in and send `power log` before it dies, or let it run out and send `power last` after the next start. Voltage under load reads lower than at rest, so compare drain rates over spans of at least 10 minutes.

## Display and UI development

The paired lock screen cycles through synced note excerpts on the card. NEXT or a horizontal swipe advances to another note. The text stays in the original spoken language. The timestamp follows the display-language setting.

The panel is driven over SPI with DMA (`esp_lcd`) from two 40-line buffers, so LVGL draws the next strip while the previous one is sent. LVGL refreshes every 15 ms. Send `perf` over USB serial to read the measured frame rate, the slowest main-loop pass since the last `perf`, and the current CPU clock. The simulator counts refreshes in virtual time.

Check UI changes in the host simulator before flashing: it runs the real `src/main.cpp` with the real fonts and writes screenshots and animation frames. See [sim/README.md](sim/README.md).

```sh
firmware/sim/build.sh
mkdir -p work/sim/out && work/sim/murmur-sim firmware/sim/scenes/tour.txt work/sim/out
```

Icons are the prototype's SVG strokes, rasterized into `src/icons_data.cpp` by `python3 firmware/tools/gen_icons.py`. Add an icon or size there, then regenerate.

## Source layout

| File | Responsibility |
|---|---|
| `src/main.cpp` | Screens, navigation, keys and touch |
| `src/ui_kit.*` | Palette, drawing primitives and motion (prototype timings) |
| `src/tasks.*`, `src/pi_sync.*` | Task lists, card cache, recording uploads and Pi sync. Unpaired devices show sample data. |
| `src/icons.*`, `src/icons_data.cpp` | Generated stroke icons |
| `src/board.*` | Display, backlight, touch, buttons, battery |
| `src/power_log.*` | Per-minute battery consumption log and USB serial commands |
| `src/audio.*` | Microphone capture task and WAV writer |
| `src/card_usb.*` | TF card sharing between recorder and USB reader |
| `src/campus_wifi.*`, `src/campus_profile.*` | HKUSTGZ login, saved profile, clock recovery |

## Known limitations

- **Sleep is shallow.** It turns off the backlight and runs the CPU at 80 MHz instead of 240 MHz, and Wi-Fi sessions end by themselves. True light sleep would disconnect USB on every sleep and needs testing on hardware first.
- **Arduino-ESP32 2.0.17 (ESP-IDF 4.4).** Its SDK has no public setter for the RADIUS server name, so `tools/campus_tls.py` renames a symbol in the prebuilt supplicant and `-Wl,--wrap=eap_sm_allowMethod` restricts the outer method to PEAP. Moving to Arduino-ESP32 3.x (ESP-IDF 5) would replace both with `esp_eap_client_set_domain_name()`. It also changes the I2S, TinyUSB and Wi-Fi APIs this firmware uses. That move needs a hardware session to verify USB mounting and campus login, so it has not been made blind. The build fails loudly (`Supplicant TLS setup hook missing`) if the patch stops applying.

## Collect diagnostics

From the repository root on Windows, run this to save the current diagnostic history and collect again after a reconnect or reboot.

```powershell
.\work\venv\Scripts\python.exe firmware/tools/collect_logs.py --watch
```

The helper saves a timestamped file under `work/diagnostics/` and prints its location. Omit `--watch` for a single capture. Close other serial tools when capturing. The USB console also accepts `log status` and `log dump`.

The device keeps its last 128 events in RTC memory. The *previous* section is recovered after a reset that preserves RTC memory. The *checkpoint* section is the last flash snapshot from before this boot, and *incident* holds up to 32 events leading to the latest retained watchdog or panic reset. A complete loss of power leaves the last committed flash snapshots. Each section identifies the boot and build, and event times are milliseconds since boot. `sntp.synced` and `clock.set` record the corresponding Unix timestamp in `a`.

Diagnostics include stages and result codes for Wi-Fi, USB and card readiness, sleep and wake, and slow main loops. The log leaves out campus credentials, IP addresses, and recording or note content. Windows Code 43 counts describe what the host saw during an outage. Match the time and USB port when attributing an error to the handheld.

For an intermittent USB loss, collect once with `--usb` after reconnecting. The serial command is `log usb`. The `usb_gap` section contains the most recently captured gap and its preceding events. The diagnostic worker samples the controller every 250 ms and saves the first gap to NVS, then limits additional saves to one per minute. Unplugging or a host suspend can also create a gap. Match its boot ID and uptime to the host observation. The ordinary `log dump` includes the most recent watchdog incident. The collector decodes the first eight characters of the task name running on each core at the watchdog interrupt.

| Event | Field a | Field b |
|---|---|---|
| `usb.bus` | Device status register DSTS, including the received frame number and suspend bit | Device control register DCTL |
| `usb.traffic` | Count of MSC read callbacks entered | Milliseconds since the last capacity or readiness query, or −1 before the first |
| `usb.read` | 0 idle, 1 waiting for the card mutex, 2 holding the mutex in the read callback | Age of the in-flight callback in milliseconds |
| `usb.gap` | Bit 0 for two seconds without advancing USB frames, bit 1 for a read callback lasting one second, bit 2 for ten seconds without a capacity or readiness query after the first | Milliseconds since the last observed frame advance |
| `usb.irq` | Global interrupt status | Endpoint interrupt status |
| `usb.control` | Endpoint zero IN control register | Endpoint zero OUT control register |
| `usb.bulk_in`, `usb.bulk_out` | MSC endpoint control register for the indicated direction | Transfer-size register (remaining bytes and packets) |
| `usb.bulk_tx` | MSC IN interrupt status | TX FIFO empty interrupt mask |
| `usb.read_block` | Last card sector requested by MSC | Requested bytes |
| `usb.probe_error` | SDMMC status error | Reserved |
| `usb.media_unavailable` | Requested sector | 1 during local card ownership, 2 after eject, otherwise 0 |
| `usb.in_stall_begin`, `usb.in_stall_end` | Active IN endpoint address | Initial interrupt status on entry, elapsed microseconds on completion |
| `cpu.pc` | CPU 0 program counter | CPU 1 program counter |

The ESP32-S3 Debug Assistant samples the program counters. A USB gap transition records one pair. The task-watchdog interrupt saves three pairs to RTC memory. Use the ELF from the build ID in the same log section to resolve the addresses with `xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf`.

## Fonts

The firmware reads the 4-bit antialiased glyphs directly from flash. The text faces at 13, 14, 16, 17, 18 and 20 px include all 3,500 level-one characters from the 2013 Table of General Standard Chinese Characters. Both UI languages use these faces for task content. The pack also includes punctuation, prototype text, Inter for Latin and JetBrains Mono for the recording timer. It occupies 3,215,133 bytes including metrics and indexes. Each application slot is 7 MiB. The UI and its card errors work without a font file on the card.

To regenerate `assets/fonts.bin`, use Pillow 13.0.0 and fontTools 4.68.0 in the project virtual environment. Put the following inputs under `work/fonts/` and run `work/venv/Scripts/python firmware/tools/pack_fonts.py work/fonts` from the repository root.

| Input | Source |
| --- | --- |
| `inter.ttf`, `notosanssc.ttf`, `jetbrainsmono.ttf` | The upright variable TTF from each family's `ofl/` directory in [Google Fonts at 869789e](https://github.com/google/fonts/tree/869789e84da92782c144d83d69b64cec54905b5f/ofl). The accompanying OFL licenses are in `assets/`. |
| `Unihan.zip` | [Unicode 17.0 Unihan](https://www.unicode.org/Public/17.0.0/ucd/Unihan.zip), field `kTGH`, entries 2013:0001 through 2013:3500. |

Panel timing, pin assignments and ES7210 setup follow the [Waveshare board examples](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/tree/cf676aa6351b7580c79f6d50a1a296a68e149273/examples/ESP32-S3-Touch-LCD-1.54-demo).
