# Everyday Voice Capture - Version 1 Design

Press a button, say what you want to remember, and put the device back in your pocket. The device saves the recording locally. Once it connects to campus Wi-Fi, a Raspberry Pi turns the recording into tasks and notes. The next morning, you can take just the device, see what you planned for the day, and check things off offline.

## Recording a thought

The Waveshare ESP32-S3-Touch-LCD-1.54 is the sole capture device. The proposed control is the **PLUS** button. Press it once to start recording immediately. The screen shows that recording is active and displays the elapsed time. Press it again to stop.

Show **Saved** as soon as the recording has been written successfully to the microSD (TF) card. If the card is full or a write fails, show the error on screen. Starting a new recording takes priority over background synchronization.

During recording, PLUS and the on-screen Stop & save button finalize the WAV. NEXT and the on-screen Cancel button open a confirmation with Keep recording selected. Continue capturing until the owner confirms Discard or saves. Discarding removes only the current temporary file and returns to the screen where recording started. A discarded recording creates no upload or transcription job.

## USB access to the TF card

With the card inserted and the USB cable left connected, a firmware update must restore both the serial console and read-only access to the whole card automatically. Windows assigns the COM number and drive letter. During recording, the handheld has exclusive access to the card. It makes the card available to the host again after closing the WAV.

The firmware first presents the serial console and an empty removable reader. After the host configures USB, wait at least four seconds before reporting the card as present. A background task initializes the card and checks its status. The USB readiness and capacity callbacks return the cached result immediately so the device can keep answering USB requests while the card comes online.

**Routine feature update.** Flash the finished application once. Verify the changed features, the serial response, and the existing recordings on the mounted volume. A cold-start feature can be tested by power-cycling the installed application.

**Targeted USB regression.** When changing USB enumeration, reader startup or download-mode transitions, inventory the WAVs, leave the card and cable in place, and flash the application four times in succession. After each restart, verify an actual serial response and automatic volume mounting. Check that the connection remains available for 45 seconds, then read every WAV end to end and compare file names and sizes with the inventory.

**Hardware baseline, 2026-11-21.** On the USB-powered test unit, four consecutive flashes restored COM5 and drive F automatically. Windows mounted the volume about 22 seconds after COM5 appeared. The final three runs passed the 45-second check and all 11 existing WAVs were readable. Repeating this test with the battery connected remains open.

The first update with the saved campus profile restored the application serial port and volume automatically. All 13 WAVs matched their pre-update SHA-256 hashes. In the next test the 1200-baud request removed the application port, but Windows reported Code 43 (device descriptor request failed) before the ROM port appeared. No flash write took place in that attempt. Distinguish a failure entering the loader from a failure mounting the card after a write. Record the application USB ID (303A:4003), ROM ID (303A:1001), and whether the display responds in each phase.

After this failure, a normal cable replug still showed Code 43. With the battery disconnected, a USB-only cold start restored the lit lock screen, COM5 and the card volume. The device reported `wifi saved` and `wifi waiting clock`. Setting the clock alone triggered an authenticated campus login and a successful connection to the Pi. All 13 WAVs still matched their original hashes. This recovery does not constitute a pass of the four-update regression check.

## Power during flashing and shutdown

**Owner's hardware check, 2026-11-21.** With USB connected, holding PWR brought up the release prompt. Releasing PWR brought up the unplug prompt. The owner confirmed the handheld shut down when the USB cable was removed. Battery connection status was not recorded for this check.

**Regression on the PWR-shutdown image, 2026-11-21.** Four consecutive application flashes restored the serial port and card with the cable left in place. Each cycle included a 45-second stability check, a serial response, and matching SHA-256 hashes for all 13 WAVs. The updater stopped the radio before requesting download mode. This run did not establish the cause of the earlier Code 43.

Hold PWR (OK) continuously for three seconds. Its usual 800 ms action still occurs first. The display then lights and prompts the owner to release PWR. The firmware finalizes an active WAV before offering shutdown and shows a write error if saving fails. On release it drops the battery latch. If USB still supplies the board, the display asks the owner to unplug USB. It stays in that state with the latch low, so pulling the cable completes a cold shutdown with the battery installed. The owner can start it again with PWR or by reconnecting USB. This gesture requires the application to be running.

On this board, GPIO2 (`BAT_EN`) controls the battery power latch. With USB unplugged and the PWR button released, firmware can release the latch to shut down the battery-powered unit while leaving the battery physically connected. USB VBUS also supplies VSYS through D4, so releasing the latch with powered USB attached leaves the regulator supplied. A flash reset and a complete loss of board power are different test conditions. Disconnecting the battery in the recovery test above was a diagnostic step. The current handheld sleep action only blanks the backlight. See the [board schematic, BAT, Type-C and Power sections](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf).

## Saved campus login

The development handheld must reconnect to HKUSTGZ without a computer or a repeated password prompt when its usual TF card is inserted and its clock is set. Keep the randomly generated AES-256-GCM key in internal NVS and the authenticated ciphertext on the card. Ordinary application updates preserve both. This is a reversible development choice and uses no eFuse slots. The owner can replace the campus password over USB and correct a lost clock in Settings. Provisioning and recovery are documented in [Campus Wi-Fi](campus-wifi.md#getting-credentials-onto-the-device).

After campus authentication and DHCP the handheld automatically synchronizes the clock via SNTP. It remembers the SHA-256 fingerprint of the leaf certificate after a successful, dated PEAP login. Following a cold start with an unset clock it can use that exact certificate for a time-only connection. This checks the chain, the RADIUS DNS name, and validity at the last successful check. It disconnects and authenticates again with current time before making normal network operations available. If the certificate has changed or no pin exists yet, set the date in Settings → Clock or over USB.

**Physical cold start, 2026-11-21.** The owner powered the handheld off and reconnected USB without setting the clock. Read-only diagnostics reported an SNTP update, `cold_sync=1`, and a fully authenticated HKUSTGZ connection. The handheld reached the Pi's SSH service. All 14 existing WAV files and the one pre-existing partial file matched their pre-update hashes. Windows had previously reported a USB descriptor failure on this image, so this result establishes cold-start Wi-Fi recovery only.

## Turning speech into notes and tasks

The Raspberry Pi sends the audio for transcription, then sends the transcript to an AI model to interpret the speaker's intent. The user can speak naturally and cover several things in one recording.

- **Tasks.** Extract actionable items and keep the details needed to carry them out. "Print the report and ask about binding while I'm at the print shop" can be one errand with both details.
- **Miscellaneous notes.** File observations, ideas, and reflections in this Obsidian category. If a recording also contains a task, keep the speaker's words in the note and extract the task separately. A shared recording ID can link the two.
- **Dates.** Interpret relative dates using the time of recording. If the user says "pick up the parcel tomorrow" on Monday and the device syncs on Wednesday, the task is dated Tuesday. The device timestamps recordings and synchronizes its clock when online. If the date cannot be established reliably, preserve the original phrase for review.

## The on-device task list

**Today** shows tasks scheduled for today and unfinished tasks carried over from previous days. Once a task enters Today, it stays there until the user checks it off. Completing a task offline updates the list immediately. The device sends the change to the Pi at the next sync.

Tap the count at the top left to switch between remaining and completed tasks. Tap a completed task to undo completion. Both lists scroll continuously under the finger with the header and the When I Have Time line fixed.

**When I Have Time** occupies a separate line that cycles through tasks without a scheduled day. The user can move the displayed task into Today. The proposed interaction pauses the rotation when the user selects an item, so confirmation applies to that same task.

A deadline and a planned workday have distinct effects. If the user says on Monday, "The report is due Friday," the task appears in the rotating line with its Friday deadline visible. The user can move it into Today on Wednesday and still see that it is due Friday. If it remains unfinished and has not already been selected, it enters Today automatically on Friday.

A task with an explicit planned day enters Today on that day. The device caches tasks and dates so the list can roll over at midnight while offline. Physical buttons support both moving a task into Today and marking it complete. The interaction prototype will establish the exact button assignments.

## Campus sync and Obsidian

1. **Connect after recording.** Once the recording is saved locally, the device enables Wi-Fi, authenticates with the campus account, and connects to the Pi on the campus network. Before implementation, verify 2.4 GHz availability, the required EAP method and certificates, and the device's access to the Pi's service port.
2. **Upload and process.** The Pi acknowledges receipt after it has saved the complete recording. It then handles transcription, classification, and storage of the results. API keys stay on the Pi.
3. **Update the handheld.** In the normal flow, the device retrieves the result, saves any tasks locally, and switches Wi-Fi off. If the network is unavailable or processing times out, it retains the pending state and switches Wi-Fi off. The proposed retry points are the next device wake or the end of the next recording. The Pi holds completed results until the device reconnects. Stable IDs for recordings and tasks ensure that retrying creates each intended note or task only once.
4. **Update Obsidian.** The computer's vault receives Markdown notes and task completion updates. If the computer is off, the Pi holds the updates until it returns. The handheld remains the primary place to manage tasks. Development and deployment should favor command-line tools.

## Architecture choice and open decisions

We considered calling the APIs directly from the handheld, running a public cloud service, and using the Raspberry Pi on campus. The Pi is the chosen backend because campus devices can reach one another and it can finish processing after the handheld turns off Wi-Fi.

Before implementation, choose the transcription service, classification model provider, audio retention period, and method for syncing the Pi with the Obsidian vault. A third-party transcription service receives the audio. The classification API receives the transcript. Configure both according to the chosen providers and retention policy.

## First acceptance test

- Record one dated task and one reflection that also contains an actionable idea.
- Verify that the tasks reach the handheld and the reflection appears under Miscellaneous in Obsidian.
- Interrupt synchronization and let it retry. Check that each intended task and note appears exactly once.
- Complete a task offline. Reconnect and verify that its completion reaches Obsidian.
- Simulate a change of date. Check that unfinished tasks carry over, future tasks enter Today on schedule, and moving a task early preserves its deadline.
