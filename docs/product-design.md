# Everyday Voice Capture - Version 1 Design

Press a button, say what you want to remember, and put the device back in your pocket. The device saves the recording locally. Once it connects to campus Wi-Fi, a Raspberry Pi turns the recording into tasks and notes. The next morning, you can take just the device, see what you planned for the day, and check things off offline.

## Recording a thought

The Waveshare ESP32-S3-Touch-LCD-1.54 is the sole capture device. The proposed control is the **PLUS** button. Press it once to start recording immediately. The screen shows that recording is active and displays the elapsed time. Press it again to stop.

Show **Saved** as soon as the recording has been written successfully to local storage. A microSD card is the proposed storage medium. If the card is full or a write fails, show the error on screen. Starting a new recording takes priority over background synchronization.

## Turning speech into notes and tasks

The Raspberry Pi sends the audio for transcription, then sends the transcript to an AI model to interpret the speaker's intent. The user can speak naturally and cover several things in one recording.

- **Tasks.** Extract actionable items and keep the details needed to carry them out. "Print the report and ask about binding while I'm at the print shop" can be one errand with both details.
- **Miscellaneous notes.** File observations, ideas, and reflections in this Obsidian category. If a recording also contains a task, keep the speaker's words in the note and extract the task separately. A shared recording ID can link the two.
- **Dates.** Interpret relative dates using the time of recording. If the user says "pick up the parcel tomorrow" on Monday and the device syncs on Wednesday, the task is dated Tuesday. The device timestamps recordings and synchronizes its clock when online. If the date cannot be established reliably, preserve the original phrase for review.

## The on-device task list

**Today** shows tasks scheduled for today and unfinished tasks carried over from previous days. Once a task enters Today, it stays there until the user checks it off. Completing a task offline updates the list immediately. The device sends the change to the Pi at the next sync.

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
