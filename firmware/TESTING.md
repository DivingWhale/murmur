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

If a run fails, tell apart a failure to enter the ROM loader from a failure to mount the card after a write. Record in each phase the application USB ID (`303A:4003`), the ROM ID (`303A:1001`), and whether the display responds.

A flash reset and a complete loss of board power are different test conditions. USB VBUS also supplies VSYS through D4, so releasing the battery latch with powered USB attached leaves the regulator running. See the [board schematic, BAT, Type-C and Power sections](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf).

## Open items

- Measure `perf` on hardware with the DMA display driver (not yet flashed).

- Repeat the targeted USB regression with the battery connected.
- The cause of the Windows Code 43 seen on 2026-11-21 is not established.
- USB mounting has only been tested on Windows.

## Log

All tests on a Windows PC unless noted.

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
