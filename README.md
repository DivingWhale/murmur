# Murmur

*Everyday voice capture in your pocket.*

Press a button, say what you want to remember, and put the device back in your pocket. A Waveshare ESP32-S3-Touch-LCD-1.54 records to microSD. A Raspberry Pi on campus turns each recording into tasks and notes, and the notes go to Obsidian. The handheld shows today's tasks and works offline.

## Start here

| If you are working on… | Read |
|---|---|
| Anything | [docs/product-design.md](docs/product-design.md): what the system does and why |
| USB serial, TF card access or post-flash mounting | [USB behaviour](docs/product-design.md#usb-access-to-the-tf-card) and [hardware tests](firmware/TESTING.md) |
| Handheld UI / firmware | [docs/ui-spec.md](docs/ui-spec.md): **source of truth** for screens, controls, sizes and text |
| Build or flash the handheld preview | [firmware/README.md](firmware/README.md) |
| Lock-screen notes (computer ↔ Pi ↔ handheld) | [docs/lockscreen-api.md](docs/lockscreen-api.md) |
| Claude usage page (5-hour / weekly limits) | [docs/claude-usage-api.md](docs/claude-usage-api.md) |
| Joining the campus Wi-Fi (WPA2-Enterprise, PEAP) | [docs/campus-wifi.md](docs/campus-wifi.md) |

## Repository layout

```
docs/
  product-design.md     system design (version 1)
  ui-spec.md            handheld UI implementation spec
  lockscreen-api.md     lock-screen notes API contract
  claude-usage-api.md   Claude usage page data contract
  campus-wifi.md        campus Wi-Fi (PEAP) setup and firmware notes
prototype/
  index.html            interactive UI prototype (reference only)
  api/v1/               mock API responses used by the prototype
firmware/               ESP32-S3 firmware, fonts and CLI build entry point
  TESTING.md            hardware test procedures and log
```

## Run the prototype

```sh
cd prototype && python3 -m http.server 8080
# open http://localhost:8080
```

The prototype simulates the 240×240 screen and the three buttons (PLUS, NEXT, OK). Its *Screen states* section shows every screen at actual pixel size, in English and Chinese.

## Working together

- Work on a branch and open a pull request. Don't push straight to `main`.
- Keep development environments, toolchains, download caches and build products inside this checkout and covered by `.gitignore`. The handheld builder uses `work/`, `firmware/.pio/` and `firmware/build/`.
- If a change alters the UI behaviour, update `docs/ui-spec.md` in the same PR. The spec and the code must not disagree.
- Keep secrets out of the repo, including API keys and campus Wi-Fi credentials. They belong on the Pi or in local config files that aren't committed.
