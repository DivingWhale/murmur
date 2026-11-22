# Everyday Voice Capture - Version 1 Design

Press a button, say what you want to remember, and put the device back in your pocket. The device saves the recording locally. Once it connects to campus Wi-Fi, a Raspberry Pi turns the recording into tasks and notes. The next morning, you can take just the device, see what you planned for the day, and check things off offline.

## Recording a thought

The Waveshare ESP32-S3-Touch-LCD-1.54 is the sole capture device. The proposed control is the **PLUS** button. Press it once to start recording immediately. The screen shows that recording is active and displays the elapsed time. Press it again to stop.

Show **Saved** as soon as the recording has been written successfully to the microSD (TF) card. If the card is full or a write fails, show the error on screen. Starting a new recording takes priority over background synchronization.

During recording, PLUS and the on-screen Stop & save button finalize the WAV. NEXT and the on-screen Cancel button open a confirmation with Keep recording selected. Continue capturing until the owner confirms Discard or saves. Discarding removes only the current temporary file and returns to the screen where recording started. A discarded recording creates no upload or transcription job.

## USB access to the TF card

With the card inserted and the USB cable connected, the handheld presents a USB serial console and a read-only reader for the whole card. After a firmware update both must come back without unplugging the cable. During recording the handheld has exclusive access to the card and hands it back to the host after closing the WAV.

The firmware first presents the serial console and an empty removable reader. After the host configures USB, it waits at least four seconds before reporting the card as present. A background task initializes the card and checks its status. The USB readiness and capacity callbacks return the cached result immediately, so the device keeps answering USB requests while the card comes online.

Update and regression procedures are in [firmware/TESTING.md](../firmware/TESTING.md).

## Power-off

Hold PWR (OK) for three seconds. Its usual 800 ms action happens first. The display then prompts the owner to release PWR. An active recording is finalized before shutdown is offered, and a failed save shows a write error instead. On release, the firmware drops the battery latch (GPIO2, `BAT_EN`). USB power bypasses the latch, so with a cable attached the display asks the owner to unplug USB, and pulling the cable completes the shutdown with the battery still installed. PWR or reconnecting USB starts the device again. This gesture needs the application to be running. Sleep currently only turns off the backlight.

## Saved campus login

The development handheld reconnects to HKUSTGZ without a computer or a repeated password prompt, provided its usual TF card is inserted. A randomly generated AES-256-GCM key lives in internal NVS and the encrypted login lives on the card. Ordinary application updates keep both. This is a reversible development choice and uses no eFuse slots. The owner can replace the campus password over USB.

After authentication and DHCP the handheld sets its clock via SNTP. After a successful login with a valid date, it remembers the SHA-256 fingerprint of the server certificate. After a cold start with no clock, it can use that exact certificate for a time-only connection, then disconnects and logs in again with full date checks before any other network use. If the certificate has changed or none has been saved yet, set the date in Settings → Clock or over USB. Details are in [Campus Wi-Fi](campus-wifi.md#getting-credentials-onto-the-device).

## Turning speech into notes and tasks

The Raspberry Pi sends the audio for transcription, then sends the transcript to an AI model to interpret the speaker's intent. The user can speak naturally and cover several things in one recording.

- **Tasks.** Extract actionable items and keep the details needed to carry them out. "Print the report and ask about binding while I'm at the print shop" can be one errand with both details.
- **Miscellaneous notes.** Keep observations, ideas and reflections in the speaker's language. All notes sync to the handheld's lock screen by default. If a recording also contains a task, keep the speaker's words in the note and extract the task separately. A shared recording ID links both. The Pi keeps the full body for future Obsidian integration.
- **Dates.** Interpret relative dates using the time of recording. If the user says "pick up the parcel tomorrow" on Monday and the device syncs on Wednesday, the task is dated Tuesday. The device timestamps recordings and synchronizes its clock when online. If the date cannot be established reliably, preserve the original phrase for review.

## The on-device task list

**Today** shows tasks scheduled for today and unfinished tasks carried over from previous days. Once a task enters Today, it stays there until the user checks it off. Completing a task offline updates the list immediately. The device sends the change to the Pi at the next sync.

Tap the count at the top left to switch between remaining and completed tasks. Tap a completed task to undo completion. Both lists scroll continuously under the finger with the header and the When I Have Time line fixed.

**When I Have Time** occupies a separate line that cycles through tasks without a scheduled day. The user can move the displayed task into Today. The proposed interaction pauses the rotation when the user selects an item, so confirmation applies to that same task.

A deadline and a planned workday have distinct effects. If the user says on Monday, "The report is due Friday," the task appears in the rotating line with its Friday deadline visible. The user can move it into Today on Wednesday and still see that it is due Friday. If it remains unfinished and has not already been selected, it enters Today automatically on Friday.

A task with an explicit planned day enters Today on that day. The device caches tasks and dates so the list can roll over at midnight while offline. Physical buttons support both moving a task into Today and marking it complete. The interaction prototype will establish the exact button assignments.

## Campus sync and Obsidian

1. **Connect after recording.** Once the recording is saved locally, the device enables Wi-Fi, authenticates with the campus account, and connects to the Pi on the campus network. The paired handheld uses TLS with a device-specific shared key on TCP port 8443.
2. **Upload and process.** The Pi acknowledges receipt after it has saved the complete recording. It then handles transcription, classification, and storage of the results. API keys stay on the Pi.
3. **Update the handheld.** In the normal flow, the device retrieves the result, saves any tasks locally, and switches Wi-Fi off. If the network is unavailable or processing times out, it retains the pending state and switches Wi-Fi off. The proposed retry points are the next device wake or the end of the next recording. The Pi holds completed results until the device reconnects. Stable IDs for recordings and tasks ensure that retrying creates each intended note or task only once.
4. **Update Obsidian.** The computer's vault receives Markdown notes and task completion updates. If the computer is off, the Pi holds the updates until it returns. The handheld remains the primary place to manage tasks. Development and deployment should favor command-line tools.

## Architecture choice and open decisions

We considered calling the APIs directly from the handheld, running a public cloud service, and using the Raspberry Pi on campus. The Pi is the chosen backend because campus devices can reach one another and it can finish processing after the handheld turns off Wi-Fi.

The Pi uses Beijing Bailian’s Qwen ASR Flash for transcription and DeepSeek Flash for task and note extraction. It retains the original audio and transcript. Automatic expiry and the method for syncing the Pi with the Obsidian vault remain open. See [Pi service and pairing](../backend/README.md).

## First acceptance test

- Record one dated task and one reflection that also contains an actionable idea.
- Verify that the tasks reach the handheld and the reflection appears under Miscellaneous in Obsidian.
- Interrupt synchronization and let it retry. Check that each intended task and note appears exactly once.
- Complete a task offline. Reconnect and verify that its completion reaches Obsidian.
- Simulate a change of date. Check that unfinished tasks carry over, future tasks enter Today on schedule, and moving a task early preserves its deadline.
