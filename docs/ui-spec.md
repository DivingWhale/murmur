# Handheld UI — Implementation Spec

This is the UI spec for the handheld firmware. Read it **before** writing any UI code. It is the source of truth for screens, controls, timings, sizes and text. If this spec and the prototype disagree, the spec wins.

| File | What it is |
|---|---|
| `docs/product-design.md` | Product design (recording, sync, tasks, Obsidian). |
| `docs/ui-spec.md` | **This spec.** |
| `prototype/index.html` | Interactive reference prototype. Run `cd prototype && python3 -m http.server 8080`, then open `http://localhost:8080`. The *Screen states* section shows every screen at actual pixel size. |
| `docs/lockscreen-api.md` | API for the lock-screen notes (Pi ↔ computer ↔ handheld). |
| `docs/claude-usage-api.md` | Data for the Claude usage page. |
| `prototype/api/v1/lockscreen.json` | Mock response for that API. |

---

## 1. Hardware and stack

- **Board:** Waveshare ESP32-S3-Touch-LCD-1.54, a **240×240** touch LCD.
- **Physical size:** 27.7 mm per side, about **220 ppi**, so **1 px ≈ 0.115 mm**. All sizes in this spec are panel pixels.
- **Buttons:** three physical buttons, **PLUS**, **NEXT** and **OK**, plus the touchscreen. GPIO assignment is still open (see §11).
- **Suggested stack:** LVGL. Several behaviours below correspond directly to LVGL features, for example `LV_LABEL_LONG_SCROLL_CIRCULAR` for scrolling text.

## 2. Design tokens

| Token | Value | Use |
|---|---|---|
| bg | `#0B0C0E` | Screen background |
| surface | `#1C1F24` | Highlighted row / box fill |
| line | `#262A30` | Dividers |
| text | `#F4F3EF` | Main text |
| muted | `#9CA0A8` | Secondary text, icons |
| dim | `#6B7079` | Disabled text, checkbox outline, done tasks |
| focus | `#FFD23F` | Highlight outline (2px), "Move" button, question labels |
| rec | `#FF5C4D` | Recording, critical battery, errors |
| due | `#FFB067` | Deadline text, due chips (chip fill = due at 14% alpha), low battery |
| sync | `#6CB6FF` | Sync in progress |

- **Highlight style:** surface fill, a 2px inset outline in focus colour, 9px corner radius.
- **Avoid red vs green:** never use colour alone to tell states apart.

## 3. Type

Fonts: **Inter** for Latin text and **Noto Sans SC** for Chinese, both bundled as LVGL fonts, 4 bpp.

| Element | Latin | Han (Chinese) |
|---|---|---|
| Header "Today" | 16px bold | 17px bold |
| Header date, status, hints, meta, chips, small labels | 13px (labels 11px bold uppercase, +0.07em tracking) | 14px (labels 13px bold, **no** uppercase, no tracking) |
| Task title | 16px medium, 1 line | 17px medium, 1 line |
| Lock-screen note | 17px medium, line height 1.38, max 5 lines | 18px, line height 1.5, max 4 lines |
| Lock-screen clock | 34px semibold, tabular digits | same |
| Picker title | 19px semibold, max 2 lines | 20px, line height 1.35 |
| Recording timer | 56px semibold, monospace digits | same |

- **No text smaller than 13px, and no Chinese text smaller than 13px**, labels included. The sizes come from the physical panel (13px ≈ 1.5 mm), not from how the prototype looks on a monitor.
- **Choosing the Chinese size:** check each text string for Han or full-width characters (U+3000–303F, U+3400–9FFF, U+F900–FAFF, U+FF00–FFEF). If the string contains any, use the Han size and line height for that element.
- **Line breaking:** use strict rules, so no line starts with `。，、？！：；` or a closing bracket.
- **Font coverage:** Noto Sans SC subset to the 3,500 common characters (通用规范汉字表 level 1) plus ASCII and CJK punctuation, at the sizes above. That is roughly 3–4 MB of flash; fewer sizes means less space. Missing glyphs show as `□`.
- **Touch targets:** at least 60px (≈ 7 mm). Task rows are 60px tall, the bottom line is 64px, and the Move/Cancel buttons are 72px.

## 4. Screens

Coordinates are from the screen's top-left corner. The prototype has an exact example of each screen.

### 4.1 Lock screen (shown at boot)
- **Header** (padding 12px top, 14px sides), with the two sides vertically centred:
  - Left: clock `HH:MM`.
  - Right, right-aligned and stacked: the battery indicator, with the date under it (`Wed 18 Nov` / `11月18日 周三`).
- **Note:** the excerpt of one lock-screen note, starting 10px below the header.
- **Recording time** under the note: the exact time it was recorded, `15 Nov · 09:22` / `11月15日 09:22`. Use the wall-clock time from `recorded_at` as written; don't convert time zones. Notes have **no title**.
- **Footer** (10px from the bottom): position dots on the left (only if there is more than one note) and the hint `OK/↑ unlock · + record` / `OK/上滑 解锁 · + 录音` on the right.
- **No notes:** show `No notes chosen yet. Pick Miscellaneous notes on your computer.` / `还没有选择笔记。请在电脑上挑选随记。`
- **Rotation:** move to the next note every `interval_s` seconds (default 6).
- **Touch:** only deliberate swipes count, and taps are ignored (pocket safety).
  - **Swipe left/right** of ≥ 24 px (≈ 2.8 mm) shows the next/previous note (slides in, 0.28 s) and restarts the rotation timer.
  - **Swipe up** of ≥ 60 px (≈ 7 mm, a quarter of the screen) unlocks. Today rises into place (40 px, 0.3 s).
  - Pages (§4.10) can't be opened from here.

### 4.2 Today
- **Header (32px tall):**
  - Left: `Today` / `今天`, then a count badge of unfinished tasks (only when there are more than 2 tasks), then the date (`Wed 18`, or the weekday only in Chinese: `周三`).
  - Right: sync status, then battery, 8px apart.
- **Sync status (compact):**
  - All synced: a check icon in muted colour.
  - Waiting, online: a sync icon and a count, in sync colour.
  - Waiting, offline: a Wi-Fi-off icon and a count, in muted colour.
- **Task list:** **2 rows visible** (60px each, 3px gap). The window scrolls so the highlighted row is always visible.
- **Task row:** a 20px circle checkbox, then the title, then a meta line underneath. The meta line can contain `Carried over`, a detail such as `Print shop`, and a deadline in due colour (`Due Fri`, `Due today` or `Overdue`), joined with ` · `.
- **Finished tasks:** a filled checkbox, and the title in dim colour with a line through it.
- **Empty list:** `All clear` / `全部完成`, with `Press + to capture a thought` underneath.
- **Bottom line, "When I Have Time":** pinned to the bottom, 74px tall (76px for Chinese labels), with a 1px divider above. It shows two rows:
  - **Label row:** `WHEN I HAVE TIME` / `有空再做` with position dots. When highlighted, the label changes to `PAUSED · OK TO CHOOSE` / `已暂停 · 按 OK 选择` in focus colour.
  - **Title row:** the task title, then a due chip (`Due Fri`) if the task has a deadline.
  - **Rotation:** show the next task every 3 s. If the title scrolls (§6), wait until one full loop has finished before rotating. Rotation pauses while the line is highlighted.
- **Toast:** after a sync, a pill 82px from the bottom shows the result, for example `+1 task`, `+1 note · +1 task`, `+1 task for tomorrow` or `Synced`. It stays for 3 s.

### 4.3 "Move to Today?" picker (full screen)
- **Contents, from top to bottom:**
  - `MOVE TO TODAY?` / `移到今天？` in focus colour, centred.
  - The task title (max 2 lines).
  - A row with the due chip and position dots.
  - Two buttons side by side: **Cancel** (surface fill, hint `Hold OK`) and **Move** (focus fill, hint `OK`).
  - At the bottom: `Cancels by itself in Ns` / `N 秒后自动取消`.
- **Browsing:** each new task slides in, from the right for next and from the left for previous (0.28 s).
- **Auto-cancel:** after 8 s with no input. Any browsing restarts the 8 s.

### 4.4 Recording
- **Top row:** a red dot that blinks once a second, `REC` / `录音中` on the left, and the date on the right. Below 20% battery (when not charging), the battery indicator replaces the date.
- **Middle:** the timer `m:ss`, with a live level meter of 20 bars underneath.
- **Bottom:** `Press + to stop` / `按 + 停止`, prefixed with `Sync paused · ` if this recording interrupted a sync.

### 4.5 Saved
A circle with a check mark, `Saved` / `已保存`, and `0:12 · on microSD`. At the bottom: `Syncing next` (online) or `Will sync when online`. Show this screen **only after** the file write has succeeded. It stays for 2 s.

### 4.6 Syncing
`Syncing` / `同步中`, then three steps, each pending, active (sync colour with a spinner) or done (check mark):
1. `Campus Wi-Fi` / `连接校园 Wi-Fi`
2. `Uploaded to Pi` / `上传到 Pi`
3. `Turning into tasks` / `整理成任务`

At the bottom: `Press + to record anytime` / `随时按 + 录音`.

### 4.7 Errors
- **Card full or write failed:** a warning triangle in rec colour, `Card full` / `存储卡已满`, then `This recording wasn't saved. Free up space on the microSD card.` At the bottom: `OK to dismiss` / `按 OK 关闭`.
- **Battery too low:** a large battery icon, `Battery too low` / `电量过低`, then `Charge the device to record. Recording now could damage the file.`

### 4.8 Sleep
The backlight is off and nothing is drawn. Touch is ignored.

### 4.9 Hold overlay
While a button is held (§5), a dark overlay (black at 72%) covers the screen, showing an icon, a label and a progress bar that fills over the last 550 ms. The destinations are:
- **Back:** arrow icon, `Back` / `返回`.
- **Sleep:** moon icon, `Sleep` / `休眠`.

### 4.10 Pages (touch only)
Not every feature needs a button. Only frequent actions are on keys. Pages are reached **only by touch**:

- **Navigation:** on Today, swipe left or right anywhere except the bottom line to move Today ↔ **Claude** ↔ **MD**. The bottom line keeps its own swipe for browsing tasks. Each page change slides in (0.26 s).
- **Page layout:**
  - Header: the page title on the left, battery on the right (same as Today).
  - Footer (data pages only), 10px from the bottom: `Updated HH:MM` / `更新于 HH:MM`. No page dots.
- **Edge bounce:** swiping right on Today (first page) or left on MD (last page) nudges the screen 14px toward the swipe, overshoots 4px back, then settles (0.34 s). That shows there are no more pages.
- **Claude page:** two blocks, 16px apart. Data comes from `docs/claude-usage-api.md`.
  - **Labels:** `5-HOUR` / `5 小时` and `WEEK` / `本周`. Label style as in §3, with the percentage right-aligned at 30px bold, using fixed-width digits.
  - **Bar:** 8px tall, rounded, with an empty track of `#2A2D33`.
  - **Reset line:** `Resets 14:30 · in 3 h 50 m` / `14:30 重置 · 3 小时 50 分后`. For the week, the weekday goes before the time (`Mon 09:00` / `周一 09:00`).
  - **Colour:** the percentage and bar use text colour, due colour at ≥ 80%, and rec colour at ≥ 95%.
  - **No data:** `No usage data yet` / `暂无用量数据`.
- **MD page:** a placeholder for now. Show an atom icon, `Coming soon` / `即将上线`, and `Simulation progress will show here.` / `模拟进度将显示在这里。`
- **Keys on a page:** PLUS records. OK or NEXT press returns to Today. Hold OK or hold NEXT sleeps.
- **Lock screen:** taps are ignored and pages can't be opened from it. Only its own swipes (notes, unlock) work.
- **Freshness:** page data refreshes only when the device syncs. That's why every data page shows when it was last updated.

### 4.11 Quick panel (swipe down)
- **Open:** swipe down (≥ 30 px) starting in the top 32 px (header band) of Today or a page. It slides down (0.28 s).
- **Close:** swipe up (≥ 30 px), press OK or NEXT, or hold OK. Closing returns to the screen it was opened from.
- **Layout** (10px padding, 8px gaps, background `#121418`):
  1. **Status row (24px):** Wi-Fi icon (or Wi-Fi-off), network name (SSID), and `Synced 10:40` / `已同步 10:40` or `Offline` / `离线`.
  2. **Two tiles (66px each, 12px radius):**
     - **Sync now** / `立即同步`: connects, syncs and returns to Today. Offline: shows the toast `Offline`.
     - **Language** / `语言`: shows `EN` or `中` and toggles the screen-label language.
  3. **Brightness bar (52px tall, full width):** see below.
  4. **All settings ›** / `全部设置` (48px): opens Settings.
  5. A 36 × 4 px grab handle at the bottom, hinting that a swipe up closes the panel.
- **Brightness:**
  - **20 levels** in 5% steps (minimum 5%), default 80%. These map to backlight PWM.
  - The **whole bar is the touch target**: tap or drag anywhere on it to set the level.
  - The fill is in text colour on a `#2A2D33` track, with a sun icon on the left and the percentage on the right. The label turns dark once the fill reaches it (≥ 80%).

### 4.12 Settings
- **Header:** `‹ Settings` / `‹ 设置` (tap to go back) and battery.
- **List:** rows 56px tall (≈ 6.5 mm). 3½ rows are visible, so the cut-off last row hints that the list scrolls.
  - **Touch, scrolling:** continuous, like a phone list.
    - **Drag:** the list follows the finger pixel for pixel.
    - **Flick:** keeps the velocity and slows down (×0.94 per 16 ms). Releasing after a pause (> 80 ms) doesn't fling.
    - **Edges:** past the top or bottom, the list moves at 35% of the finger, then springs back. In LVGL, use a scrollable container with momentum and elastic edges (`LV_OBJ_FLAG_SCROLL_MOMENTUM`, `LV_OBJ_FLAG_SCROLL_ELASTIC`).
  - **Touch, other:** movement under 6 px counts as a tap and changes the row. Swipe right (≥ 40 px) goes back.
  - **Keys:** NEXT highlights the next row (wrapping). The list scrolls smoothly (200 ms ease-out) to keep that row fully in view. OK changes the highlighted row. Hold OK goes back.

| Row | Value | Action |
|---|---|---|
| Wi-Fi / `Wi-Fi` | SSID; sub-line `Last sync 10:40` or `Unavailable` | Status only. Credentials are set over USB (`docs/campus-wifi.md`) |
| Sync now / `立即同步` | sub-line: last sync time | Sync |
| Language / `语言` | `English` / `中文` | Toggle |
| Brightness / `亮度` | `80%` | Opens the Brightness screen |
| Auto-sleep / `自动休眠` | `30 s` · `1 min` · `5 min` · `Never` (default 1 min) | Cycle |
| Storage / `存储` | `12.4 GB free` (rec colour `Card full` when full) | — |
| Battery / `电量` | `80%`, `· charging` | — |
| Clock / `时钟` | `HH:MM`; sub-line: date | — |
| Firmware / `固件` | `Murmur 0.1.0` | — |

- **Brightness screen:**
  - **Header:** `‹ Brightness`.
  - **Contents:** the percentage at 40px bold, and below it a full-width bar 72px tall (≈ 8.3 mm). Tap or drag anywhere on the bar to set the level.
  - **Hint:** `Drag or tap · NEXT +5%`.
  - **Keys:** NEXT adds 5% (after 100% it wraps to 5%). OK or hold OK returns to Settings.
- **Auto-sleep:** after the chosen idle time on Lock, Today, a page, the panel or settings, go to Sleep. Any button press or touch resets the timer. The timer is paused while the Move to Today? picker is open (it has its own 8 s timeout).

## 5. Controls

**There is no double-press anywhere.** A press acts **on release**. A hold acts at **800 ms**. The overlay appears at 250 ms, so the user can see what will happen and release early to cancel.

| Mode | PLUS | NEXT press | NEXT hold | OK press | OK hold | Touch |
|---|---|---|---|---|---|---|
| Sleep | Wake **and start recording** | Wake to Lock | — | — | — | Ignored |
| Lock | Start recording | Next note | Sleep | Unlock to Today | Sleep | Swipe left/right (≥ 24 px): next/previous note. Swipe up (≥ 60 px): unlock. Taps ignored |
| Today | Start recording | Move highlight: task 1 → … → task n → bottom line → task 1 | Sleep | Task: check off or undo. Bottom line: open picker | Sleep | Tap a task: check it off. Tap the bottom line: open picker. Swipe the bottom line left/right: next/previous task (highlights the line and pauses rotation). **Swipe elsewhere: pages (§4.10). Swipe down from the top: quick panel (§4.11)** |
| Picker | Start recording (closes picker) | Next task | — | Move the task shown into Today | Back to Today (highlight stays on the bottom line) | Swipe: next/previous. Cancel/Move buttons |
| Recording | Stop and save | — | — | — | — | — |
| Saved, Syncing | Start recording (pauses the sync) | — | — | — | — | — |
| Error | Start recording (if possible) | — | — | Dismiss | — | — |
| Page (Claude, MD) | Start recording | Back to Today | Sleep | Back to Today | Sleep | Swipe left/right: next/previous page. Swipe down from the top: quick panel. Taps do nothing |
| Quick panel | Start recording | Close | Sleep | Close | Close | Tap tiles / All settings; tap or drag the brightness bar; swipe up: close |
| Settings | Start recording | Highlight next row | Sleep | Change highlighted row | Back | Tap a row; swipe up/down: scroll; swipe right: back |
| Brightness | Start recording | +5% (wraps) | Sleep | Back to Settings | Back to Settings | Tap or drag the bar |

- **What a hold of OK does:** at the top level (Today, a page, the lock screen), it **sleeps in one hold**, and waking always shows the lock screen. Inside a sub-screen (the picker, the quick panel, settings, brightness), it goes **back** one level. The hold overlay shows `Sleep` or `Back` so the user knows which before releasing.
- **Swipe:** a horizontal movement of more than 12 device px that is larger than the vertical movement (lock screen: 24 px sideways, 60 px up). A swipe must never also count as a tap.
- **Bottom line timeout:** if the bottom line stays highlighted for 10 s with no input, the highlight returns to task 1 and the rotation resumes.
- **Recording from sleep:** if a recording was started from Sleep, return to Sleep once its save and sync (or the offline fallback) have finished.

## 6. Long single-line text

Applies to task titles and the bottom-line title.

- **Not highlighted:** cut at the end with "…".
- **Highlighted task, and the bottom-line title:** if the text doesn't fit, show the start for **1 s**, then scroll left **continuously** at **30 px/s** (≈ 3.5 mm/s). The text repeats after a **40 px** gap, so the loop never reverses (`LV_LABEL_LONG_SCROLL_CIRCULAR`).
- **Restarts:** the scroll starts again only when the text changes, never because the screen redraws.
- **Lock-screen notes** wrap onto more lines instead of scrolling.

## 7. Task rules (from `docs/product-design.md`)

- **Carry-over:** at midnight, unfinished tasks stay in Today and get the `Carried over` label. Finished tasks leave Today.
- **Planned tasks:** a task with a planned day enters Today on that day.
- **When I Have Time tasks:** these have no planned day.
  - A task whose deadline arrives enters Today automatically on that day.
  - A task moved into Today early keeps its deadline chip.
- **Offline changes:** checking off or moving a task updates the screen immediately and queues the change for the next sync.
- **Recording time:** all dates come from the device clock at the moment of recording.

## 8. Recording and sync

- **PLUS always wins.** A recording interrupts a sync, the picker or any other screen.
- **Recording is refused:**
  - when the battery is ≤ 3% and not charging (Battery too low screen);
  - when the microSD card is full, or a write fails (Card full screen).
- **Sync flow:** after Saved, turn Wi-Fi on, then: campus authentication → upload → wait for the Pi to process → fetch results and lock-screen notes (`GET /v1/lockscreen` with `If-None-Match`) → turn Wi-Fi off → return to Today with a toast.
- **If sync fails:** offline, timed out or connection lost, keep the items pending, turn Wi-Fi off, and retry at the next wake or the end of the next recording.

## 9. Battery

- **Indicator:** an outlined battery icon with a fill level, plus the percentage, rounded to 5% steps so it doesn't flicker.
- **Colour:** muted at ≥ 21%, due colour at 11–20%, rec colour at ≤ 10%.
- **Charging:** a lightning bolt over the fill.
- **Percentage:** map battery voltage to percent with a LiPo discharge curve, not a straight line.

## 10. Text strings

Every visible string has an English and a Chinese version; the full table is the `T` object in `prototype/index.html`. The screen-label language is a setting. Content (tasks and notes) shows in whatever language it was spoken, and its type size follows §3.

## 11. Open hardware questions (check before building)

Campus Wi-Fi (WPA2-Enterprise, PEAP) is covered separately in `docs/campus-wifi.md`.

1. **Buttons:** which GPIOs PLUS, NEXT and OK use, and whether all three can wake the ESP32-S3 from deep sleep. If they can't, use light sleep instead.
2. **Charging:** whether the board reports charging status, which the lightning bolt needs.
3. **Battery voltage:** whether the board can read it through an ADC divider.
4. **Fonts:** whether the font subset at five sizes fits in flash alongside the firmware and OTA partitions. If it doesn't, drop the 13px and 20px Chinese sizes.

## 12. Acceptance checks

- **Every screen in §4:** renders at 240×240 in English and in Chinese, with nothing overlapping or clipped. Compare against the prototype's *Screen states* section.
- **The controls table in §5:** each row behaves as written. Fast NEXT presses never leave Today. Holding OK goes Picker → Today → Lock → Sleep.
- **Waking:** PLUS from Sleep starts recording within one button press, and the device goes back to Sleep after the sync.
- **Long titles:** a long English title and a long Chinese title both scroll continuously when highlighted, and are cut with "…" when not.
- **Offline:** a task checked off offline appears checked immediately and reaches Obsidian after the next sync.
- **Date change:** after a simulated midnight, carry-over, planned tasks and deadline auto-entry follow §7.
