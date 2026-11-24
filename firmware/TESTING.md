# Handheld hardware testing

How to check a firmware change on the real board, and a log of what has been verified so far. The required behaviour is in [docs/product-design.md](../docs/product-design.md). Build and flash steps are in [README.md](README.md).

## After a routine feature update

Flash the finished application once. Verify the changed features, the serial response, and the existing recordings on the mounted volume. To test a cold-start feature, power-cycle the installed application.

## After a UI change

1. Run the simulator scenes (`sim/README.md`) and compare the contact sheets with the prototype's *Screen states*. `scenes/interact.txt` should produce identical images after a change that is not meant to alter the UI.
2. On the device, swipe between Today, Claude and MD, open and close the quick panel, and unlock from the lock screen. Send `perf` right after: the frame rate during those animations should stay above 30 fps, and `loop_max_ms` should stay under about 50 ms.

## Targeted USB regression

Run this when changing USB enumeration, reader startup, or the transition into and out of download mode.

1. Inventory the WAVs on the card (names, sizes, and ideally SHA-256 hashes).
2. Leave the card and cable in place and flash the application four times in a row.
3. After each restart, check for an actual serial response and that the volume mounts by itself.
4. Check that the connection stays up for 45 seconds.
5. Read every WAV end to end and compare names and sizes with the inventory.

For sustained bulk-read changes, also read a large existing file with a known reference digest. On Windows, run `python tools/check_usb_read.py F:\large-file.bin <expected-sha256>` from the firmware directory, using the actual file and drive letter. This sends unbuffered 256 KiB reads and checks the original serial handle and boot ID every eight seconds. Record the byte count and SHA-256. Short recording files alone passed on several builds that failed this test.

If a run fails, tell apart a failure to enter the ROM loader from a failure to mount the card after a write. Record in each phase the application USB ID (`303A:4003`), the ROM ID (`303A:1001`), and whether the display responds.

A flash reset and a complete loss of board power are different test conditions. USB VBUS also supplies VSYS through D4, so releasing the battery latch with powered USB attached leaves the regulator running. See the [board schematic, BAT, Type-C and Power sections](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf).

## Open items

- Measure `perf` on hardware with the DMA display driver (not yet flashed).

- The cause of the Windows Code 43 seen on 2026-11-21 is not established.
- USB mounting has only been tested on Windows.

## Log

All tests on a Windows PC unless noted.

### 2026-11-24 USB DMA fix with battery logging, Pi sync and lock-screen notes

On the Intel xHCI port, the final image (`bc5d4c08505bd561` ELF ID prefix) passed four consecutive application-only flashes at `0x10000` with the cable, battery and card in place. Each went from application `303A:4003` (COM5), through ROM `303A:1001` (COM3), and back to COM5 with an automatically mounted F drive. Esptool verified all four writes. The boot IDs were `760140bd`, `0cde403e`, `6a185597` and `4941e210`. Serial responded throughout each 45-second mounted check. All 26 WAV names, lengths and SHA-256 hashes matched in each cycle. Each boot reported a minute sample, 80 MHz with the screen asleep, and paired Pi sync with HTTP 200, one task and two lock-screen notes. Physical button and display interaction was not independently checked during these cycles.

On the first and fourth boots, Windows read all 74,547,600 bytes of `R6211200.FIR` through unbuffered 256 KiB requests. Both reads matched the pre-flash SHA-256 `e72dd74eeaeef56130f8a88fab117c1bc08b9bf43d0b1ae95552600a6f7269ca`. Each took about 101 seconds with Wi-Fi off. The same open serial handle answered every eight seconds and after each read. The boot ID stayed constant, the device recorded no additional USB configuration, and the Windows System log contained no Disk 3 warnings during either read. The last boot's second power-log minute recorded 4,156 mV, zero seconds awake and 60 seconds on USB. Testing ended on the Intel port at the owner's request. The final image was not retested on the original ASMedia port.

### 2026-11-24 USB disconnect investigation with the battery installed

The owner could operate the UI during a USB outage. Windows recorded both a port reset failure and a device descriptor failure on port 4 of the ASMedia USB 3.1 controller. Disk 3 had 78 read warnings during the initial test. The final statuses were 53 verify-required, nine no-media and 16 no-such-device. The retained incident on the battery-logger image showed a task watchdog in phase 8. Wi-Fi had stopped at uptime 13.514 s and the last USB configuration event was at 23.551 s. That incident predates the final disconnect. An earlier firmware build also recorded the phase-8 watchdog.

The diagnostic build (`833a09c49a3a7f07` ELF ID prefix) passed four consecutive application-only flashes with the same card, battery and USB cable. Each cycle went from application `303A:4003` on COM5 to ROM `303A:1001` on COM3 and back to COM5, passed esptool verification and mounted F automatically. The four boot IDs were `f3dc944f`, `25dd961e`, `67420802` and `afd55552`. In each cycle, serial answered before and after a 45-second mounted-volume check, all 26 WAV names, sizes and SHA-256 hashes matched, and `power` reported a minute sample with the screen then asleep at 80 MHz. No physical key or display check was performed during these four cycles. Campus authentication failed during these boots, so the four-cycle result covers USB recovery without a successful Pi sync.

Immediately afterward, a Windows unbuffered read of a 74,547,600-byte file with Wi-Fi switched off and periodic serial queries stopped after 5,505,024 bytes. Boot `afd55552` recorded `usb.configured` at 132.947 s. Read callbacks stopped advancing at 4,977. At 141.791 s host frames still advanced and the read callback was idle. At 156.157 s the watchdog interrupted CPU 0 running `IDLE0` and CPU 1 running `usbd`. The next boot was `28198987`, reset 6, phase 8. This reproduces a firmware reset with Wi-Fi off. Other outages where the UI stayed responsive may have a different cause.

The device reported a date in February 2031 when campus authentication failed. Correcting the clock from the host restored a paired HTTP 200 sync with one task and two lock-screen notes on the subsequent build.

The build with hardware program-counter capture (`21b66785e5dcc631`) also passed four consecutive application flashes with the cable and battery in place. COM5 and F returned on each cycle through the same application and ROM USB IDs. All four 45-second checks passed with matching SHA-256 for every one of the 26 WAVs, a working battery logger, paired HTTP 200 and two lock-screen notes. The boot IDs were `537aaf4c`, `5f6f889d`, `973dd46e` and `ebbd1b3a`. Program counters from an ordinary pause in capacity queries resolved against the matching ELF to the diagnostic worker and CPU idle code.

The extended read again stopped at 5,505,024 bytes with Wi-Fi off. At uptime 142.107 s the USB worker was at `0x420853cd`. All three watchdog samples at 156.379 s placed it at `0x420853cb`. Both instructions resolve to line 462 of `dcd_esp32sx.c` in `dcd_edpt_stall`. The driver writes the SNAK request in DIEPCTL and then polls its bit position (27) in DIEPINT. The IN-endpoint NAK-effective acknowledgement in DIEPINT is bit 6. The loop starved the application until the watchdog reset it. The board rebooted as `393c8469`. [Upstream correction](https://github.com/espressif/esp32-arduino-lib-builder/commit/3eb9cb06afb534d37f47e98bca9f45e99126f81a)

The first build with the upstream IN-endpoint fix (`56c65dfc7b4fe9e2`) verified its application flash and matched all 26 WAVs. On the ASMedia port, a serial handle opened before automount failed its later check. Reopening the port returned the same boot ID. The extended read again stopped after 5,505,024 bytes and Windows reported a descriptor failure. Its saved gap showed advancing USB frames, an idle SD callback and CPU 1 waiting for interrupts.

With the same cable on the Intel xHCI controller and a cold start, a direct 1 MiB read starting at offset 5,505,024 succeeded. The full uncached read with serial polling failed after 7,864,320 bytes. The storage-only run failed after 28,049,408 bytes. Windows logged a READ(10) timeout and reconfigured the reader. During recovery the board completed the active MSC IN stall (`0x81`) in 17 microseconds, and later answered on the same boot (`26568f27`) without a power cycle. These runs verify the correction to the watchdog loop and reproduce a separate bulk-transfer failure.

On the Intel port, boot `604dd299` kept an already-probed card visible during repeated configuration. One flash, a 45-second post-mount serial check and 26 WAV hashes passed. The large read still failed at 5,767,168 bytes. The saved snapshot at 83.631 s showed host frames advancing and an idle SD callback. Bulk IN endpoint 1 remained enabled, with 320 bytes and seven packets outstanding (`DIEPTSIZ=0x00380140`). Its FIFO-empty interrupt was enabled in the mask. A second image used the same SD multi-sector reads but submitted a single 64-byte USB packet per callback. It passed the short check and failed the extended read at 12,058,624 bytes. The packet-size change was reverted. The owner had also reproduced the disconnect with different cables.

The TinyUSB 0.16 DWC2 controller in interrupt-driven mode still lost the uncached read at 29,097,984 bytes. Boot `8522b523` recorded an enabled bulk IN endpoint with zero bytes still to load and eleven outstanding packets, followed by a panic reset. A buffered read also failed. Holding the CPU at 240 MHz on that driver let Windows finish the file with the correct digest, but Windows reconfigured USB during the read and the original serial handle timed out. Reopening serial returned the same boot (`f7901ad6`). The fixed-clock comparison was removed.

### 2026-11-24 battery logging with Pi sync and lock-screen notes

Flashed the application partition once with the battery logger from `4baf5ae` integrated into the Pi-sync working tree. Esptool verified the write at `0x10000`. The device subsequently hit a task watchdog (phase 8, Wi-Fi step 0). Windows reported Code 43 on the board's USB port. That build did not capture the program counter.

After a physical reconnect, COM5 returned and the device completed a paired HTTP 200 sync. Serial and the active card snapshot both showed two lock-screen notes. The previous note snapshot matched the pre-flash active snapshot. All five simulator frames for the lock-screen note scene matched their existing references.

The first complete `power log` row showed 4154 mV, 60 seconds awake, 25% mean backlight, 13 seconds of Wi-Fi, 60 seconds on USB and 49 seconds charging. The same boot later reported two samples, 80 MHz and the backlight off. A host test of the logger with simulated time and storage verified the 720-sample ring and retrieval of the last 10-minute checkpoint in a fresh process. The hardware sample was collected on USB power.

Card verification stopped when Code 43 recurred. The pre-flash inventory covered 33 files including 25 WAVs. All 25 WAV names were present after flashing. In the interrupted session, 22 matched their original SHA-256, two lacked a completed comparison, and one (`rec-20261121-002647-6717d862.wav`, 66,092 bytes) returned a different digest. After reconnecting the board for diagnosis, a complete reread through its USB reader matched the original hashes for all 25 WAVs and found one additional recording. The last successful sync reported HTTP 200, one task, two notes and an empty queue.

### 2026-11-23: Obsidian selection and task edits

One application-only flash verified the image hash and restored COM5. The paired board reported HTTP 200, two tasks, one note and an empty queue. Using a temporary Vault and the deployed Pi HTTPS listener, a manual memo was created hidden, enabled, hidden again and deleted. Across three device syncs, the active card snapshot contained one, two and one notes respectively. The fixture appeared only in the middle snapshot. All 27 card filenames remained present. The 25 files apart from the two rotating note snapshots matched their original SHA-256 hashes, including all 21 WAVs. Selection was verified through serial and the active card file. Physical key interaction during concurrent edits and loss of power during a page write were not exercised in this session.

### 2026-11-22: paired lock-screen notes and strict extraction

Four synthetic transcripts sent from the Pi to Beijing Bailian with strict JSON Schema passed the field checks and expected task/note counts for note-only, task-only, mixed and filler speech. An additional synthetic Mandarin WAV went through the deployed TLS service, Qwen ASR and DeepSeek Flash, producing one note and zero tasks.

One application-only flash passed flash verification. COM5 reported HTTP 200, two existing tasks and one new note. The card snapshot included the expected Chinese excerpt and original recording time. After removing the owned synthetic recording from the Pi and syncing again, COM5 reported two tasks and zero notes. The active card snapshot was empty. All 24 pre-existing files matched their original SHA-256 hashes.

The firmware simulator rendered Chinese content on an English lock screen, switched date language, handled an unknown timestamp and returned to the same frame after a rotation. The existing English tour and interaction screenshots matched their previous hashes. A native run of the cache code on the Pi with an isolated directory checked two-page publication, restoration in a new process and recovery from a damaged primary file.

### 2026-11-22: Chinese task titles in English and new-task feedback

The host simulator rendered a Chinese task and a mixed Latin/Chinese title in English Today using the shipped 16 px face. The six body text sizes contained the same 3,767 code points. The sync notice scene showed a next-day task after unlocking, cleared it after three seconds, and showed a count for two arrivals during recording after Saved. The standard interaction and English tour screenshots matched their previous hashes.

One application-only flash passed the device hash check. COM5 reported a paired HTTP 200 sync, two tasks and an empty queue. The card cache retained the current-day and next-day tasks with their original Chinese titles and local times. All 24 existing card files, including 20 WAVs, matched their pre-flash SHA-256 hashes. Visual checks used simulator frames.

### 2026-11-22: Pi voice sync

One application-only flash preserved the saved campus login. COM5 answered the sync, Wi-Fi, clock and performance commands. After USB pairing, the board reached the Pi over campus Wi-Fi, completed a TLS sync with HTTP 200 and turned the radio off. All 20 pre-existing card files matched their SHA-256 hashes, including 18 WAVs. The simulator interaction contact sheet matched the previous shade-fixed baseline.

A separate database on the Pi received a synthetic WAV in two chunks, including a repeated chunk. The installed service called both cloud models, returned one task naming Obsidian for 09:00 two days after capture, and accepted a repeated completion update. The fixture stayed out of the live task list.

A second, future-dated fixture exercised the production service and actual handheld download. The card cache contained the expected title and planned time. Removing only the test recording and its results, then syncing, restored the empty live task cache. Both cloud fixtures used synthetic PCM.

Two wired sessions with Windows drive access around sync recorded a task-watchdog reset after the radio went off. Both retained phase 8 and Wi-Fi step 0; the last recorded event was USB configuration. A pre-update diagnostic log contains the same reset and phase. These logs do not establish a cause. With no host drive queries during the next three consecutive sync cycles, the handheld returned HTTP 200, turned off Wi-Fi and retained the same boot ID each time.

### 2026-11-21: USB baseline

On the USB-powered test unit, four consecutive flashes brought back COM5 and drive F automatically. Windows mounted the volume about 22 seconds after COM5 appeared. The final three runs passed the 45-second check and all 11 existing WAVs were readable.

### 2026-11-21: first update with saved campus profile, then Code 43

The first update with the saved campus profile brought back the application serial port and volume automatically. All 13 WAVs matched their pre-update SHA-256 hashes.

In the next test, the 1200-baud request removed the application port, but Windows reported Code 43 (device descriptor request failed) before the ROM port appeared. No flash write took place.

After this failure, a normal cable replug still showed Code 43. With the battery disconnected, a USB-only cold start brought back the lit lock screen, COM5 and the card volume. The device reported `wifi saved` and `wifi waiting clock`. Setting the clock alone triggered an authenticated campus login and a successful connection to the Pi. All 13 WAVs still matched. This recovery is not a pass of the four-update regression check. Disconnecting the battery was a diagnostic step.

### 2026-11-21: PWR shutdown

With USB connected, holding PWR brought up the release prompt and releasing it brought up the unplug prompt. The owner confirmed the handheld shut down when the cable was removed. Battery connection status was not recorded.

### 2026-11-21: regression on the PWR-shutdown image

Four consecutive application flashes brought back the serial port and card with the cable left in place. Each cycle included a 45-second stability check, a serial response, and matching SHA-256 hashes for all 13 WAVs. The updater stopped the radio before requesting download mode. This run did not establish the cause of the earlier Code 43.

### 2026-11-21: cold-start Wi-Fi recovery

The owner powered the handheld off and reconnected USB without setting the clock. Diagnostics reported an SNTP update, `cold_sync=1`, and a fully authenticated HKUSTGZ connection. The handheld reached the Pi's SSH service. All 14 existing WAVs and the one pre-existing partial file matched their pre-update hashes. Windows had previously reported a USB descriptor failure on this image, so this result establishes cold-start Wi-Fi recovery only.
