# Teleprompter — Hardware Architecture Plan

## Goal
DIY teleprompter for YouTube and classes, built on two ESP32 boards.

## Components
- **Display unit:** Waveshare ESP32-S3-Touch-LCD-7B
  - 1024x600 IPS RGB display, 5-point capacitive touch
  - ESP32-S3, 16MB flash, 8MB PSRAM, dual-core LX7 up to 240MHz
  - Wi-Fi 2.4GHz b/g/n + BLE 5, onboard antennas
  - TF/SD card slot, USB-C (power + flashing), CAN/RS485/I2C headers, PH2.0 LiPo connector
  - Supports Arduino and ESP-IDF
  - Source: https://docs.waveshare.com/ESP32-S3-Touch-LCD-7B
- **Controller unit:** a second, separate ESP32 devkit (e.g. ESP32-C3/S3 mini)
  - Rotary encoder (scroll speed), buttons (play/pause, reverse/restart)
  - Battery powered (single-cell LiPo), handheld/pedal-style remote

## Key architecture decisions
- **Text entry → Wi-Fi**, not Bluetooth. Display board hosts its own small web
  server with a page to type/paste a script directly (originally planned as a
  file upload; switched to an in-browser textarea — simpler, and BLE file
  transfer was impractical for text anyway due to MTU limits).
- **Display Wi-Fi mode → AP (its own hotspot)**, not joining the home LAN.
  This pins the Wi-Fi channel, which avoids channel-sync headaches with
  ESP-NOW (see below). STA + mDNS ("teleprompter.local") can be added later
  once the core works.
- **Scroll control → ESP-NOW**, not classic Wi-Fi or BLE, for the
  controller-to-display link. Peer-to-peer, no pairing ceremony, low enough
  latency that a physical speed knob feels instant. Message = simple struct
  like `{command, value}` (play/pause, speed, reverse).
  **On-screen touchscreen controls were built, tested, then deliberately
  removed** because they competed with the display's own redraw for
  CPU/PSRAM bandwidth. That reason no longer applies under the current
  renderer (see below), but the decision stands on ergonomics: control
  belongs on the wireless remote, not on glass you are reading through.
  Hooks for the remote's receive callback: `tp_display_set_speed(float)` and
  `tp_display_set_paused(bool)`, or `tp_app_set_speed()` /
  `tp_app_set_paused()` / `tp_app_rewind()` to also persist the value and
  reflect it on the web page. Speed is signed, so reverse is a negative
  value with no other change.
  Touch hardware itself is still unused, but the *bring-up* sequence it used
  to perform is load-bearing for the display and is preserved in
  `tp_board_init()` — see hardware-gotchas.md and renderer-rewrite.md.
- **Storage:** LittleFS on the onboard 16MB flash (not the SD card) — the
  current script is saved to `/script.txt` and reloaded on boot. Speed,
  brightness and mirror state live in NVS.
- **Mirror-flip mode is a must-have**, not a nice-to-have — horizontal
  text mirroring is what makes this usable behind teleprompter beamsplitter
  glass. **Resolved in software.** The hardware-level `esp_lcd_panel_mirror()`
  route was a dead end (see hardware-gotchas.md). It is now baked in at
  rasterisation time, once per script, so it costs nothing while scrolling.
- **Rendering → own scanline renderer, not LVGL** (2026-09-14). LVGL remains
  linked only as a font container. See renderer-rewrite.md for the full
  reasoning; the short version is that any design which composes a
  full-screen frame in PSRAM on this board is bandwidth-bound, so the
  firmware composes nothing and generates pixels as they are scanned out.

## Build order (phased)
1. ✅ Bring up display board — flash factory demo, confirm LCD + touch work.
2. ✅ Static renderer — hardcoded text, word wrap, white-on-black, custom
   96pt font.
3. ✅ Mirror-flip.
4. ✅ Auto-scroll engine — smooth, adjustable-speed scrolling.
   **Closed 2026-09-14**: the residual stutter was fixed by replacing the
   renderer outright (renderer-rewrite.md), not by mitigating LVGL. User
   confirmed on hardware that the stutter is gone.
5. ✅ Wi-Fi script entry — AP + web page + textarea + LittleFS save/reload.
   Now applies live; no reboot on save.
6. Controller board — encoder + buttons, ESP-NOW sender. **Not started —
   this is the next milestone.**
7. Pair and tune — connect the two, tune speed mapping/encoder feel, test
   range. Not started.
8. Rig it — mirror glass, mount/stand, battery, enclosure. Not started.

## Current firmware state (display board, as of 2026-09-14)

**Working and confirmed on physical hardware.** Black background, white
word-wrapped text in a custom 96pt Roboto font, optionally mirrored for
beamsplitter glass, auto-scrolling at an adjustable speed, with no stutter.

Rendering is a custom scanline renderer: the RGB panel runs in ESP-IDF's
no-frame-buffer mode and the script is rasterised once into a rolling 4bpp
alpha mask in PSRAM, which the panel's bounce-buffer callback expands
through a lookup table as the pixels are scanned out. Scroll position
advances inside that callback from `esp_timer_get_time()`, so it cannot be
stalled by Wi-Fi, HTTP, flash writes, or anything else running on the chip.
Full reasoning, measurements and verification notes: **renderer-rewrite.md**.

Wi-Fi AP ("Teleprompter" / "teleprompter123") serves a control page at
`http://192.168.4.1/`: script textarea, scroll speed, brightness, mirror
toggle, pause and back-to-top, all applied live.

Project layout:
- `lib/lvgl/` + `lib/lv_conf.h` — LVGL v8.4.0, sibling layout required by
  LVGL's internal includes. Used only for `lv_font_get_glyph_dsc()` and
  `lv_font_get_glyph_bitmap()`; `lv_init()` is never called.
- `src/` — `tp_config.h` (all tunables), `tp_board.cpp` (I2C IO expander:
  backlight, brightness, power-up sequence), `tp_text.cpp` (layout +
  rasterisation), `tp_display.cpp` (panel + scanline renderer + scroll
  clock), `tp_web.cpp` (AP + control page), `main.cpp`, `Roboto_96.c`.
- All Waveshare vendor driver files (`rgb_lcd_port`, `gt911`/`touch`,
  `io_extension`, `i2c`, `esp_lv_adapter_arduino`) have been **deleted**,
  along with all `[DIAG]` instrumentation.

## Headroom now available

Removing the frame buffers freed roughly 2.4 MB of PSRAM (the mask ring uses
2 MB of it), and scanline generation costs about 25% of one core on core 1,
with core 0 essentially idle apart from Wi-Fi. Features that were previously
rejected as too expensive should be re-evaluated against this budget rather
than against the old LVGL numbers.

## Next up
1. **Controller board (Phase 6)**: encoder + buttons, ESP-NOW sender. The
   display-side hooks are already in place (see decisions above).
2. Pair controller with display, tune scroll-speed mapping and encoder
   feel, test range (Phase 7).
3. Recording-workflow polish on the display side — start-paused/lead-in,
   position indicator, adjustable font size. Cheap now; see headroom above.
4. Rig it: mirror glass, mount/stand, battery, enclosure (Phase 8).

## Closed items (do not re-open)
- Scrolling stutter/flicker — closed by the renderer rewrite. The old
  "mitigate or accept the platform quirk" decision in this file is moot.
- `'0'` rendering as `'1'` — an LVGL cmap off-by-one plus a font generated
  without a zero. Fixed in `font_has()` and by regenerating `Roboto_96.c`.
  See renderer-rewrite.md.
- `ESP.restart()` on script save — gone, which also retires the observed
  Octal-PSRAM soft-reset watchdog quirk.

See `hardware-gotchas.md` for the known-good `platformio.ini`, every
toolchain fix, and the detailed history of the mirror-flip, performance,
memory, and backlight/touch-init investigations. See `renderer-rewrite.md`
for the current rendering architecture.

> Note (repo docs pass): this file is the original planning doc and predates
> the remote controller build. It still describes the controller as
> "not started" with an encoder — see `remote-controller.md` for what was
> actually built (three buttons, no encoder). The remote's project folder is
> `teleprompter-remote/`, not `controller/`.
