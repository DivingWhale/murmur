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

Provision HKUSTGZ once with the [USB campus login helper](../docs/campus-wifi.md#getting-credentials-onto-the-device). Online sessions automatically synchronize network time. After a fully dated login the device remembers the campus server certificate and can recover time following a cold shutdown, then reconnect with full date validation. Settings → Clock and USB provide a fallback after a certificate change. Recording and task controls work offline. Automatic Pi upload, transcription, Obsidian synchronization and date-based task scheduling are pending. The Claude page shows its empty state. Sleep currently turns off the backlight.

## Fonts

The firmware reads the 4-bit antialiased glyphs directly from flash. The Chinese faces at 13, 14, 17, 18 and 20 px include all 3,500 level-one characters from the 2013 Table of General Standard Chinese Characters. The pack also includes punctuation, prototype text, Inter for Latin and JetBrains Mono for the recording timer. It occupies 2,714,917 bytes including metrics and indexes. Each application slot is 7 MiB. The UI and its card errors work without a font file on the card.

To regenerate `assets/fonts.bin`, use Pillow 13.0.0 and fontTools 4.68.0 in the project virtual environment. Put the following inputs under `work/fonts/` and run `work/venv/Scripts/python firmware/tools/pack_fonts.py work/fonts` from the repository root.

| Input | Source |
| --- | --- |
| `inter.ttf`, `notosanssc.ttf`, `jetbrainsmono.ttf` | The upright variable TTF from each family's `ofl/` directory in [Google Fonts at 869789e](https://github.com/google/fonts/tree/869789e84da92782c144d83d69b64cec54905b5f/ofl). The accompanying OFL licenses are in `assets/`. |
| `Unihan.zip` | [Unicode 17.0 Unihan](https://www.unicode.org/Public/17.0.0/ucd/Unihan.zip), field `kTGH`, entries 2013:0001 through 2013:3500. |

Panel timing, pin assignments and ES7210 setup follow the [Waveshare board examples](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/tree/cf676aa6351b7580c79f6d50a1a296a68e149273/examples/ESP32-S3-Touch-LCD-1.54-demo).
