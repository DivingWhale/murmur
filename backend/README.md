# Murmur on the Pi

The paired handheld sends new, finalized WAVs over the campus network. The Pi acknowledges each chunk after a SQLite commit. When the last chunk arrives, a background worker calls Qwen ASR Flash and then DeepSeek Flash through Beijing Bailian. It stores the transcript, extracted tasks, notes and original audio in `~/.local/share/murmur/murmur.sqlite`. A recording filename identifies retries. The worker checkpoints speech recognition, retries failed cloud calls with backoff and publishes the tasks and notes together.

## Installation

Use Raspberry Pi OS with Python 3.13 or later and `ssl.HAS_PSK` enabled. Place `server.py`, `bridge_api.py` and `cloud.py` in `~/projects/murmur/backend/`. Create a mode-0600 `~/.config/murmur/service.env` with these two variables:

- `DASHSCOPE_API_KEY` is a Beijing Bailian key permitted to call `qwen3-asr-flash` and `deepseek-v4.1-flash`.
- `MURMUR_PSK` is a random 32-byte device pairing key encoded as 64 hexadecimal characters. Generate it once with `openssl rand -hex 32`.

Install the user service from this directory:

```sh
mkdir -p ~/.config/systemd/user
install -m 644 murmur.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now murmur
sudo loginctl enable-linger "$(id -un)"
```

On the computer with the handheld plugged into USB, run the existing project Python environment with `firmware/tools/pair_pi.py <SSH-alias> <Pi-campus-IPv4>`. Repeat pairing if the IP address or key changes. TCP port 8443 uses TLS 1.2 with a shared device key. A key is required for both uploads and task access.

## Use and diagnostics

The optional [Obsidian PC bridge](../pc_bridge/README.md) uses a separate certificate-authenticated HTTPS listener and PC bearer credential. Its task and memo changes share the handheld database. New voice and handwritten notes start unselected. Existing favorites keep their saved selection. Changing `show_on_device` or deleting a note invalidates the handheld snapshot.

Press PLUS to record and again to save. New recordings keep their WAV on the card. A sibling `.wav.send` marks an upload awaiting confirmation. Sync removes this marker after the Pi confirms the whole file. WAVs created by older firmware stay on the card. Recordings with an unset clock retain their original transcript and relative date phrases. The server leaves their scheduling fields empty.

DeepSeek Flash receives the transcript and recording time with a strict JSON Schema. Its two required arrays are `tasks` and `notes`. Each task has `title`, `details`, `planned_at` and `due_at`. Each note has a short `title` based on a specific detail and a `body` preserving the speaker’s words. The Pi validates both arrays, normalizes dates and commits the results together. Obsidian uses the title as the initial filename. The lock-screen excerpt comes from the body. SQLite retains the full note, transcript and source audio.

Every extracted note syncs by default. The paired handheld requests notes with its task edits, downloads changed snapshots in pages of 20 and saves them to `/murmur/notes.jsonl` on the TF card. An interrupted refresh leaves the last complete set available offline. The lock screen starts with the newest note and cycles every six seconds. A recording made without a clock shows an unknown time.

The handheld also stores its task list and unsent completion, undo and promotion changes on the TF card. The picker keeps its selection during a sync. An unfinished scheduled task remains in Today from its planned day, or its deadline if that comes first. Short recordings normally finish during one Wi-Fi session. A later sync fetches results if the handheld went offline first. Long recordings use 180-second transcription segments.

- `sync status` on the USB console reports pairing, last HTTP result, queued local items, task and note counts, and outstanding processing or download pages. Status values 0–5 mean waiting, syncing, synced, processing, failed and card unavailable.
- `sync now` starts a sync or a campus login. The same command is available in the quick panel.
- `systemctl --user status murmur` shows the service state. In SQLite, `SELECT state,count(*) FROM recordings GROUP BY state;` shows outstanding work. `error` and `retry_at` record the last processing failure and next attempt.
- `python3 -m unittest -v test_server` in this directory checks interrupted uploads, duplicate requests, worker retries and offline edits using a temporary database.
