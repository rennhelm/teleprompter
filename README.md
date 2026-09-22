# Teleprompter

A two-board DIY teleprompter for YouTube and classes: an ESP32-S3 display
board that renders and scrolls the script behind beamsplitter glass, and a
separate ESP32-S3 remote that controls speed, play/pause and re-pacing over
ESP-NOW.

## Repo layout

```
teleprompter/
├── teleprompter-display/   ESP32-S3-Touch-LCD-7B firmware (the actual teleprompter)
├── teleprompter-remote/    ESP32-S3-N16R8 DevKitC firmware (handheld remote)
├── docs/                   Design notes, build history, parts list
└── images/                 Photos / video of the build
```

`teleprompter-display/` and `teleprompter-remote/` are each a complete, independent PlatformIO project
— open either one directly in VS Code (File > Open Folder, the folder
containing its `platformio.ini`) and Build/Upload as normal. See each
project's config header (`src/tp_config.h`, `src/ctl_config.h`) for pins
and tunables.

## What this is

- **Display**: 1024x600 IPS touch display, custom scanline text renderer
  (no frame buffer — see `docs/renderer-rewrite.md`), mirror-flip for
  beamsplitter glass, Wi-Fi AP with a web page to paste/edit the script and
  adjust speed/brightness live, and a "target run length" mode that paces
  the scroll to finish a script in a set time (`docs/pacing-and-run-model.md`).
- **Remote**: three buttons (faster / slower / play-pause), talks to the
  display over ESP-NOW, status LED. See `docs/remote-controller.md`.

## Getting started

1. Read `docs/hardware-gotchas.md` first. It has the known-good
   `platformio.ini` settings and the toolchain fixes (Windows long-path
   limit, wrong PlatformIO platform version) that everything else depends on.
2. Build and flash `teleprompter-display/` first. Join the "Teleprompter"
   Wi-Fi network (password `teleprompter123`) and open `http://192.168.4.1/`
   to confirm the control page loads, before touching the remote.
3. `teleprompter-remote/README.md` covers remote wiring, controls and the
   status LED.

## Parts list

See `docs/PARTS.md`.

## Design background

The `docs/` folder has the fuller story of *why* things are built this way,
including the dead ends (LVGL-based rendering, hardware mirror flip,
on-screen touch controls) so nobody re-walks them:

- `docs/architecture-plan.md` — overall hardware/software decisions and build phases
- `docs/renderer-rewrite.md` — why the display uses a custom scanline renderer instead of LVGL
- `docs/pacing-and-run-model.md` — the target-run-length / start-paused feature
- `docs/remote-controller.md` — remote hardware and the ESP-NOW protocol
- `docs/hardware-gotchas.md` — toolchain and hardware bring-up issues, in the order they were found

## License

See `LICENSE`.
