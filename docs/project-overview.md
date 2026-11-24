# Murmur project overview

Murmur is a pocket voice recorder for things you want to do or remember. Press **PLUS** to record a thought and press it again to save. You can put the handheld away at that point. When it next reaches the paired Raspberry Pi over campus Wi-Fi, the Pi sends the audio for transcription and turns the speaker’s words into tasks and notes. Tasks appear on the handheld’s **Today** list when relevant. Notes selected in Obsidian return as excerpts you can read on the lock screen, even offline.

Suppose you record this on Monday while offline: “Print the poster tomorrow. The blue lettering was easier to read from the back of the room.” If you first sync on Wednesday, the printing task belongs to Tuesday and carries over into Wednesday’s list until you complete it. The observation about lettering can appear separately on the lock screen. The recording time is what gives “tomorrow” its meaning.

## From first setup to a saved recording

1. **Prepare the handheld.** The firmware runs on a Waveshare ESP32-S3-Touch-LCD-1.54 with a 240 × 240 touch display. Insert a writable FAT32 microSD card for recordings and offline data. The three keys handle the frequent actions: PLUS records, NEXT browses and OK selects or confirms. [Handheld setup and controls](../firmware/README.md)
2. **Pair it once for sync.** Set up the Python service on the Pi with its Beijing Bailian API key and a device pairing key. Use the USB helpers to provision the handheld for the HKUSTGZ campus network and pair it with the Pi. The handheld uses the shared key for TLS on port 8443. The cloud API key stays on the Pi. [Campus login](campus-wifi.md) · [Pi setup and pairing](../backend/README.md)
3. **Capture speech anywhere.** Press PLUS to start, then press PLUS again or tap **Stop & save**. The handheld writes a 16 kHz, 16-bit mono WAV under `/murmur/` on the card. Cancel opens a confirmation so an accidental tap does not discard the recording. A saved WAV has a companion `.wav.send` file marking it for upload. Recording works without Wi-Fi.

## How a recording becomes tasks and notes

| Stage | What happens |
| --- | --- |
| Upload | During a Wi-Fi session, the handheld sends the WAV in chunks. The Pi acknowledges each chunk after saving it in SQLite. An interrupted transfer can resume. Once the Pi confirms the whole file, the handheld removes the upload marker and keeps the WAV. |
| Transcribe | A background worker on the Pi sends the audio to Qwen ASR Flash on Beijing Bailian. It splits long recordings into 180-second segments and saves the completed transcript before extraction. Failed cloud calls are retried with a delay. |
| Extract | DeepSeek Flash receives the transcript and recording time. It extracts tasks with a title, details, a planned date and a separate deadline where given. It keeps observations in the speaker’s language. One recording can yield both kinds of result. If the clock was unset, scheduling fields stay empty. |
| Store and return | The Pi validates the results and saves tasks and notes together, linked to the source recording. The handheld sends pending task edits and downloads the task list and changed note excerpts. If processing outlasts the Wi-Fi session, a later sync fetches the results. |

## Daily use on the handheld

- **Work through Today.** An unfinished task enters Today on its planned day or deadline, whichever comes first, and carries over until completed. Browse the **When I Have Time** strip to move an item into Today yourself. Tap a task to complete it, or switch to the completed list to undo. These changes take effect offline and go back to the Pi at the next sync.
- **Read a note without unlocking.** Choose which notes appear on the lock screen with the Obsidian bridge. The lock screen starts with the newest selected excerpt and rotates every six seconds. Press NEXT or swipe sideways to browse. The Pi keeps the full text. The handheld updates its card cache in pages of up to 20 notes and keeps the previous complete set if a refresh is interrupted.
- **Check the device.** Swipe down from the top of Today to open the quick panel, trigger a sync, change brightness or switch between English and Chinese. Settings also show free card space and battery status. Connect USB to play or copy WAVs from the read-only card reader. Over the separate serial interface, `sync status` reports pairing, queued items and the last sync result.

## Implementation status

The November 22, 2026 hardware log records a synthetic Mandarin WAV going through the deployed service and both cloud models. The paired handheld then received the expected Chinese excerpt and recording time. A separate test verified that a future-dated task reached the handheld’s card cache. [Hardware test log](../firmware/TESTING.md)

The Windows Obsidian bridge syncs task and note edits with the Pi and controls lock-screen selection. [Obsidian bridge setup](../pc_bridge/README.md) The Claude page has a layout for five-hour and weekly usage and currently shows its empty state on the device. The MD page is reserved for simulation progress. To explore the screens without a board, use the [interactive browser prototype](../prototype/index.html) or the [host simulator](../firmware/sim/README.md).
