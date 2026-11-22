# Lock Screen Notes — API Contract

The paired handheld cycles through all recorded miscellaneous notes by default. The Pi and handheld use their existing TLS sync connection. Computer-side selection and the prototype endpoints below describe a future integration.

## Deployed Pi and handheld protocol

The handheld sends `POST /v1/sync` with `edits`, `notes_version` and `notes_after`. An empty version requests the first page. The Pi returns `notes: null` for an unchanged complete snapshot. A changed snapshot returns an object with its opaque `version`, starting `after` ID, `more` flag and `items`. Each item is an array of stable note ID, original recording timestamp (empty when unknown), and a plain-text excerpt. Pages contain up to 20 items in ascending ID order. Subsequent requests use the version and last ID received. A version change during download restarts the snapshot at ID zero. Older handhelds can still send an array of task edits.

The Pi keeps the full note and cuts display excerpts at 88 display units using the rules below. The handheld caches the complete snapshot and browses it newest ID first. A failed download keeps the previous cache. The screen rotates every six seconds and retains the visible ID across updates when possible.

## Future computer selection and prototype API

```
Computer (Obsidian vault)  --PUT /v1/lockscreen-->  Raspberry Pi  <--GET /v1/lockscreen--  Handheld
        chooses notes                               stores the set                         caches on microSD,
                                                                                           shows it offline
```

## What exists now

| Piece | Location | Status |
|---|---|---|
| Lock screen UI + prototype | `prototype/index.html` | Done. Loads notes from the mock below. |
| Mock response | `prototype/api/v1/lockscreen.json` | Static file. The real `GET` must return this shape. |
| Computer-side selection | — | **To build** |
| Prototype GET and computer PUT | — | Proposed. The paired handheld uses `POST /v1/sync` above. |

To point the prototype at a real Pi, change `API_BASE` in `prototype/index.html`.

## 1. Choosing notes on the computer

How the user picks notes is up to the implementer. The suggested approach is an Obsidian frontmatter flag on notes in `Miscellaneous/`:

```yaml
---
lockscreen: true
lockscreen_excerpt: "Optional shorter text to show instead of the note body."
---
```

A script or plugin collects the flagged notes, builds the payload below, and sends it with `PUT` to the Pi whenever the selection changes. The order of the `notes` array is the display order.

## 2. `PUT /v1/lockscreen` (computer → Pi)

This replaces the whole set. There's no partial update.

Headers: `Authorization: Bearer <computer token>`, `Content-Type: application/json`

```json
{
  "interval_s": 6,
  "notes": [
    {
      "id": "note_2026-11-12_1804",
      "recording_id": "rec_2026-11-12_1804",
      "excerpt": "The best ideas this week came when I wasn't at the desk. Walk first, then write.",
      "recorded_at": "2026-11-12T18:04:00+01:00",
      "source": "Miscellaneous/2026-11-12 Evening walk.md"
    }
  ]
}
```

Response `200`: `{"version": "<opaque string>"}`. The Pi sets a new `version` each time the set changes.
Response `400`: `{"error": "...", "field": "notes[2].excerpt"}` when a validation limit below is broken.

## 3. `GET /v1/lockscreen` (handheld → Pi)

The handheld calls this during each normal sync, after its recording results have been fetched and before Wi-Fi is switched off.

Headers: `Authorization: Bearer <device token>`, `If-None-Match: <version it has cached>`

- `200`: the body from section 2, plus `"version"` at the top level (see the mock file).
- `304`: nothing changed. The device keeps its cache.
- Network error or timeout: the device keeps its cache and tries again at the next sync. This must never block recording.

## Note fields

| Field | Type | Required | Notes |
|---|---|---|---|
| `id` | string | yes | Stable across updates. The device uses it to keep its place in the rotation. |
| `recording_id` | string | no | Links back to the original recording. |
| `excerpt` | string | yes | **Plain text, ≤ 88 display units** (see below). No Markdown. This is the only body text the device shows. |
| `recorded_at` | ISO 8601 with offset | yes | Shown under the text as the exact recording time, e.g. "12 Nov · 18:04" / "11月12日 18:04". The device shows the wall-clock time in the string as recorded; it is not converted to another time zone. |
| `source` | string | no | Vault path, for debugging only. |
| `audio_url` | string | reserved | For playing the original recording later. The device ignores it for now. |

## Limits the Pi should enforce

- The proposed computer-selected set holds at most **20 notes**. The deployed speech-note feed sends all notes, 20 per page. `interval_s` from 3 to 60, default 6.
- `excerpt` must be ≤ 88 **display units** after stripping Markdown. If it's longer, either reject it or cut it and add "…". Pick one approach and document it.

### Chinese and mixed text

Notes can be in English, Chinese, or both. Limits are counted in **display units**, not characters, because a Han character takes about twice the width of a Latin letter:

- A Han character, CJK punctuation (`U+3000–303F`) or full-width character (`U+FF00–FFEF`) = **2 units**.
- Anything else = **1 unit**.
- 88 units ≈ **44 Chinese characters** (4 lines at 18px) or ≈ 88 Latin characters (4–5 lines at 17px). The panel is 27.7 mm wide (≈220 ppi), so text can't be smaller without becoming hard to read.

Reference implementation (the same rule is in `prototype/index.html`, function `units`):

```python
import re
WIDE = re.compile(r'[　-〿㐀-鿿豈-﫿＀-￯]')
def units(s: str) -> int:
    return sum(2 if WIDE.match(c) else 1 for c in s)
```

Other rules:
- **Cutting text:** never cut between a character and the punctuation that follows it (`。，、？！：；`), and never cut inside a Latin word. Use "…" (`U+2026`).
- **Encoding:** UTF-8, NFC-normalised. Use full-width Chinese punctuation in Chinese text, as people normally type it.
- **Font coverage:** the firmware font is planned as Noto Sans SC covering the 3,500 common Chinese characters (通用规范汉字表 level 1) plus ASCII and CJK punctuation. The Pi should check each `excerpt` against that character list (`fonts/charset.txt`, to be shipped with the firmware) and return them in a `"warnings"` array on the `PUT` response, so the computer side can tell the user. The device shows a missing glyph as `□`.
- **No language field needed:** the device chooses font size and line spacing for each line from the script of its text (Han → 18px / 1.5 line height, otherwise 17px / 1.38).

## How the handheld behaves (already built)

- The lock screen shows the current time and date, one note's excerpt, and the exact date and time it was recorded. Notes have no title. It moves to the next note every `interval_s` seconds.
- **Touches are ignored** on the lock screen, so the device does nothing in a pocket.
- **OK** unlocks to Today. **NEXT** shows the next note. **PLUS** starts recording right away.
- If the paired device has no notes, it shows the empty state.
