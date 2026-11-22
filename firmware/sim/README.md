# UI simulator

Runs the real `src/main.cpp` and `src/fonts.cpp` on the host with LVGL and fake hardware, so screens and animations can be checked without a board. Board, audio, card and Wi-Fi are replaced by `sim.cpp`; the font pack is the real one.

```sh
python3 firmware/tools/build.py          # once, so PlatformIO fetches LVGL
firmware/sim/build.sh                    # builds work/sim/murmur-sim
mkdir -p work/sim/out
work/sim/murmur-sim firmware/sim/scenes/tour.txt work/sim/out
```

## Script commands

| Command | Effect |
|---|---|
| `wait <ms>` | Run the main loop for that long (virtual time) |
| `press <plus\|next\|ok>` | Press and release a key |
| `hold <key> <ms>` · `down <key>` · `up <key>` | Hold a key |
| `tap <x> <y>` · `swipe <x1> <y1> <x2> <y2> <ms>` | Touch gestures |
| `touch <x> <y>` · `move <x> <y> <ms>` · `lift` | Touch step by step |
| `serial <line>` | Send a USB serial command, such as `serial zh` |
| `notice <planned> <due> <title>` | Queue a new task notification. Use an ISO date/time or `-` for each date. |
| `clear-notes` · `lock-note <id> <stamp> <text>` | Simulate paired lock-screen notes. Supply IDs in ascending order and use `-` for unknown recording time. |
| `online 0\|1` · `card 0\|1` · `battery <pct>` · `charging 0\|1` · `clock unset` | Fake hardware state |
| `shot <name> [scale]` | Write `<name>.png`, 2× by default |
| `sheet <name>` then `snap` | Collect screens into one contact sheet |
| `film <name> <ms> <step>` | Capture a frame every `<step>` ms into a contact sheet, to check animations |

The simulated date is Wed 18 Nov 2026, 10:40 Asia/Shanghai. Pixel timing is exact, but frame rate is not: the host renders every frame instantly. Use the `perf` serial command on the device for the real frame rate.
