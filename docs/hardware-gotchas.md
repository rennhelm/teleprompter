# Teleprompter — Hardware Gotchas (ESP32-S3-Touch-LCD-7B)

Hard-won fixes, worth checking before re-diagnosing from scratch.

## UART1 / UART2 switch must match the USB-C port in use
The board has a physical switch selecting between two UART routings (UART1 /
UART2) to the onboard USB-C UART bridge (CH340, appears in Windows Device
Manager as "USB-Enhanced-SERIAL CH340"). If the switch position doesn't match
the port you're actually plugged into, you get a fully-enumerated COM port
that silently fails to talk to the chip — esptool reports:

```
A fatal error occurred: Failed to connect to ESP32-S3: No serial data received.
```

This looks identical to a bad cable, wrong driver, or boot-mode timing issue
(all of which we ruled out first) — check the switch position first.

## Reset button triggers a full POWERON reset, not just a chip reset
Pressing the physical RESET button on this board shows up in the Serial log
as `rst:0x1 (POWERON)` — the *same* reset-reason code as an actual power
cycle — rather than a simple reset-pin toggle (which normally shows up
differently, e.g. `RTC_SW_CPU_RST`). Seeing several `POWERON` resets in a
row during testing does **not** by itself mean the board's power supply is
failing — check whether the reset button was being pressed before chasing a
power-delivery theory (USB cable, port, hub, etc.).

## RTS-based auto-reset after flashing is not fully reliable on this board
esptool reports "Hard resetting via RTS pin..." at the end of a successful
upload. Early on this didn't actually restart the chip into the new firmware
and a manual press of the physical **RESET** button was needed — but this
became unnecessary once we moved to the pioarduino platform / newer esptool
(v5.3.0). If a flash ever seems to succeed but nothing happens on screen or
in the Serial Monitor, press RESET once before assuming something's wrong.

**Also note (2026-09-14):** running PlatformIO's "device monitor" task by
itself does **not** build or flash anything — it only opens the serial port
(which itself reboots the board via DTR/RTS toggling, producing a fresh
`POWERON` boot sequence that can look like a real reflash happened). If a
code change doesn't seem to take effect, confirm the *build/upload* task
actually ran (look for `Compiling ...` and `Writing at 0x...` in the output)
rather than assuming a "monitor" session proves the new code is running.

## Official PlatformIO `espressif32` platform is too old for Waveshare's demo code
Waveshare's example code (in their `ESP32-S3-Touch-LCD-7B` GitHub repo,
`Arduino/examples/` folder) uses `driver/i2c_master.h`, part of ESP-IDF's
newer I2C driver component. The official PlatformIO `espressif32` platform
bundles an older Arduino-ESP32 core that predates this header, causing:

```
fatal error: driver/i2c_master.h: No such file or directory
```

**Fix:** use the **pioarduino** community fork of the platform instead, which
tracks Espressif's current Arduino-ESP32 releases (3.3.x as of this writing,
built on a modern ESP-IDF that includes this driver):

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
```

This is a drop-in replacement — same board IDs, same workflow. Switching
platforms triggers a full toolchain re-download (several minutes, not a
hang).

## Windows long paths must be enabled before installing the pioarduino toolchain
The Arduino-ESP32 3.x libs package includes deeply-nested paths (from
Espressif's Matter/connectedhomeip code, for unrelated chip variants) that
exceed Windows' default 260-character path limit, failing with
`WindowsLongPathError`. Fix (one-time, needs a real Administrator PowerShell
— check the title bar literally says "Administrator", not just "run as
administrator" clicked from a non-elevated shell):

```powershell
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force
```

Requires a reboot to take effect.

## ESP_LOGI/ESP_LOGW output is silently suppressed by default
Arduino-ESP32's "Core Debug Level" defaults to a level that hides
`ESP_LOGI`/`ESP_LOGW` calls entirely — they just never show up in the Serial
Monitor, with no error. Fix: add `-DCORE_DEBUG_LEVEL=3` to `build_flags`.
Must go in the *same* `build_flags =` block as any other flags — a second,
separate `build_flags =` line causes `InvalidProjectConfError` (duplicate
key).

**Important addendum (2026-09-14) — this fix only works in files that
`#include <Arduino.h>`.** `CORE_DEBUG_LEVEL` only affects `ESP_LOGx` output
in translation units where `Arduino.h` (which pulls in
`esp32-hal-log.h`, the shim that redefines `ESP_LOGx` to route through
Arduino's own formatted logger) is actually included, directly or
transitively. Two of this project's vendor files —
`rgb_lcd_port.cpp` and `esp_lv_adapter_arduino.cpp` — only include raw
ESP-IDF headers and never pull in `Arduino.h`. Their `ESP_LOGx` calls
(including a pre-existing, unconditional `ESP_LOGI(TAG, "Create LVGL
task")` in `esp_lv_adapter_init()` that runs every boot) have **never**
produced any visible Serial output, in any log captured across this whole
project — confirmed to be unrelated to log *level*, since explicitly adding
`esp_log_level_set("*", ESP_LOG_INFO)` as the very first line of `setup()`
(confirmed via the edit shifting `main.cpp`'s own log line numbers by
exactly 1, so the rebuild definitely took effect) still produced zero
output from either file. The actual mechanism wasn't fully pinned down
before this investigation paused, but is most likely a compile-time
`LOG_LOCAL_LEVEL` bound specific to translation units that never see the
Arduino shim, which a runtime `esp_log_level_set()` call cannot override.
**Workaround: use raw `printf()` instead of `ESP_LOGx` when instrumenting
`rgb_lcd_port.cpp` or `esp_lv_adapter_arduino.cpp` (or any other vendor file
that doesn't include `Arduino.h`).** `printf()` writes straight to the
console UART independent of the ESP-IDF/Arduino logging subsystem
entirely, and is confirmed as the reliable way to add diagnostics to these
two files.

## Known-good, confirmed-working `platformio.ini`
```ini
[env:esp32-s3-devkitc-1]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
upload_port = COM4
monitor_port = COM4
upload_speed = 115200
board_build.arduino.memory_type = qio_opi
board_upload.flash_size = 16MB
board_build.partitions = default_16MB.csv
build_flags =
    -DBOARD_HAS_PSRAM
    -DCORE_DEBUG_LEVEL=3
```
(`qio_opi` = Quad I/O flash + Octal I/O PSRAM, correct for this board's
16MB flash / 8MB PSRAM ESP32-S3 module.)

## Display + touch bring-up (Waveshare's own demo, not hand-rolled)
Rather than writing our own RGB-LCD panel init (very finicky — pixel clock,
porches, sync pulses are panel-specific), we're building on top of
Waveshare's own vetted example instead:
- Source: `waveshareteam/ESP32-S3-Touch-LCD-7B` GitHub repo →
  `Arduino/examples/13_lvgl_v8_demo/`
- Driver files (`rgb_lcd_port`, `gt911`/`touch`, `io_extension`, `i2c`,
  `esp_lv_adapter_arduino`) copied into `src/`, left untouched.
- LVGL v8.4.0 + its `lv_conf.h` (from the repo's bundled
  `LVGL_v8.4.0.7z`) copied into `lib/lvgl/` and `lib/lv_conf.h`
  respectively — `lv_conf.h` must sit as a **sibling** of the `lvgl` folder,
  not inside it (LVGL's internal includes expect this relative layout).

**Confirmed working (2026-09-13):** compiled, flashed, and the stock LVGL
widgets demo (`lv_demo_widgets()`) renders on the physical screen with
working touch — full display + touch stack proven before writing any of our
own rendering code.

## RESOLVED: blank screen / no backlight after replacing the demo's setup() (2026-09-14)
**Symptom:** after swapping the stock demo's `setup()`/`loop()` for our own
teleprompter code (Wi-Fi AP, web-based script editor, sliding-window
scrolling canvas), the screen went completely black with **no backlight at
all** — not even a dim glow. This happened on every boot, including a clean
`POWERON` reset with zero errors or crashes in the serial log (confirmed via
diagnostic logging that setup() ran start to finish without issue). Multiple
false leads chased first: a suspected loose FPC ribbon cable (physical
inspection ruled this out), Wi-Fi/AP radio power draw or timing coinciding
with panel init (a real, plausible ESP32-S3 phenomenon, tested by
reordering init and by fully disabling Wi-Fi — screen stayed black either
way, ruling it out), and an unchecked `esp_lv_adapter_lock()` return value
(added logging — the lock was never actually failing).

**Actual root cause, found by diffing our `setup()` against the *original*
working demo's `setup()`:** two real bugs, both in code we hadn't touched
but had never actually run correctly:

1. `wavesahre_rgb_lcd_bl_on()` (in Waveshare's own `rgb_lcd_port.cpp`) is a
   **stub with the real hardware write commented out**:
   ```cpp
   void wavesahre_rgb_lcd_bl_on()
   {
       // IO EXTENSION_io_output(IO EXTENSION_IO_2, 1);  // Backlight ON configuration
   }
   ```
   The commented line doesn't even use real function/identifier names
   (`IO EXTENSION_io_output` isn't a thing — the real function is
   `IO_EXTENSION_Output`). This is a placeholder Waveshare left unfinished in
   their own example. Calling it did nothing.

2. **`touch_gt911_init()` was never called.** The stock demo's `setup()`
   calls it *before* `waveshare_esp32_s3_rgb_lcd_init()`:
   ```cpp
   tp_handle = touch_gt911_init();
   panel_handle = waveshare_esp32_s3_rgb_lcd_init();
   wavesahre_rgb_lcd_bl_on();
   ```
   `touch_gt911_init()` is what actually brings up the I2C bus and the
   IO_EXTENSION GPIO expander chip that the backlight (`IO_EXTENSION_IO_2`),
   touch reset (`IO_EXTENSION_IO_1`), and **LCD reset**
   (`IO_EXTENSION_IO_3`) are all wired through — `EXAMPLE_PIN_NUM_BK_LIGHT`
   is `-1` in `rgb_lcd_port.h` precisely because backlight isn't a direct
   ESP32 GPIO. When we trimmed the demo's `setup()` down for our own use, we
   dropped the touch controller entirely (we don't need touch — see the
   on-screen-controls-removed decision below) and, with it, this
   load-bearing init call. Without it, nothing downstream (backlight, and
   likely the panel's own reset line) ever gets brought out of its power-on
   state, regardless of what the panel/LVGL code does afterward — hence a
   totally clean, crash-free boot with a permanently black screen.

**Fix applied:**
- `waveshare_esp32_s3_rgb_lcd_init()` now calls `DEV_I2C_Init()` (guarded —
  only if the I2C bus isn't already up) and `IO_EXTENSION_Init()` itself, as
  a safety net.
- `wavesahre_rgb_lcd_bl_on()` / `_bl_off()` now actually call
  `IO_EXTENSION_Output(IO_EXTENSION_IO_2, ...)`.
- `setup()` calls `touch_gt911_init()` before
  `waveshare_esp32_s3_rgb_lcd_init()`, exactly matching the proven-working
  demo's order, and passes the resulting `tp_handle` into
  `esp_lv_adapter_init()` instead of `NULL`. We still don't use touch
  functionality (on-screen controls were deliberately removed — see
  architecture-plan.md), but the init call itself is load-bearing for the
  display, not just for touch.

**Lesson for next time:** when a hand-rolled `setup()` diverges from a
vendor demo that's *known* to work on the same hardware, and the failure is
silent (no crash, no error) rather than a crash/assert, diff against the
original demo's init call order line-by-line rather than auditing the new
application logic — the bug can easily be a dropped call that looked
irrelevant (like touch init) rather than anything in the code that was
actually being suspected.

## Mirror-flip: `esp_lcd_panel_mirror()` is a dead end under this board's default render mode
Waveshare's `esp_lv_adapter_arduino` port has three "avoid tearing" modes,
set via `LVGL_PORT_AVOID_TEAR_MODE` in `esp_lv_adapter_arduino.h`:
- Mode 1: double-buffer + full-refresh (redraws the *entire* screen every
  frame)
- Mode 2: triple-buffer + full-refresh
- Mode 3: double-buffer + "direct mode" (**the vendor's default**) — LVGL
  writes straight into the panel's own physical frame buffer

Under mode 3, `esp_lcd_panel_mirror(panel_handle, true, false)` returns
`ESP_OK` but has **no visible effect**, because direct mode bypasses the
`esp_lcd_panel_draw_bitmap()` call where that mirror flag would normally be
honored. Switching to mode 1 does route through that path, but mode 1's
full-screen-every-frame redraw was far too slow on this hardware (PSRAM
bandwidth bound) to be usable. **Don't chase this further** — the working
fix is a software-level pre-mirror instead (next entry), which needs no
render-mode change at all.

## Mirror-flip: working approach — pre-mirror once into an off-screen canvas
Render the script text once into an `lv_canvas` (an off-screen pixel
buffer), flip that buffer's pixels horizontally exactly once (for a
`LV_IMG_CF_TRUE_COLOR` buffer this is a simple per-row swap:
`row[x] ↔ row[w-1-x]`), then scroll the canvas object like any normal
widget. Because the mirror only runs once at creation/refill time — never
per animation frame — it costs nothing while scrolling. Any buttons/sliders
should stay as separate, non-canvas objects so they render normally
(correct, since those are read directly, not through the beamsplitter
glass).

## `lv_canvas` buffer-size macros: use LVGL's own macro, don't hand-compute the stride
Learned the hard way, twice, while trying a memory-saving 1-bit format:
`LV_CANVAS_BUF_SIZE_ALPHA_1BIT(w, h)` (and the sibling indexed/alpha macros)
use a row stride of `(w >> 3) + 1` bytes — **not** the more "obvious"
`(w + 7) / 8`. Getting this wrong causes silently corrupted/blank canvas
content with **no crash**, and a related second bug: if a hand-rolled
scratch buffer for mirroring is sized off the wrong (smaller) stride, it
overflows and triggers `Stack smashing protect failure!` on return. Always
pull the real per-row size from LVGL's own macro and size any scratch
buffers from that same number. (`LV_IMG_CF_TRUE_COLOR` has no such trap —
its stride is just `w * sizeof(pixel)`.)

## `LV_IMG_CF_ALPHA_1BIT` + `img_recolor` styling: tried, abandoned as unreliable
To fit a long script's rendered image into available PSRAM, tried switching
the mirror canvas from full RGB565 color (2 bytes/pixel) to a 1-bit alpha
mask (16x smaller) plus `lv_obj_set_style_img_recolor()` /
`_recolor_opa()` to paint the mask white. After fixing the stride bug above,
it stopped crashing, but **never actually rendered any visible content** —
confirmed via a pixel-count diagnostic that the buffer genuinely contained
correctly-drawn glyph data, yet nothing appeared on screen. Root cause not
found. **Went back to `LV_IMG_CF_TRUE_COLOR`** (the format proven to work)
and solved the memory problem a different way instead (next entry) rather
than continuing to debug this blind.

## Long-script memory problem: use a small sliding window, not one giant pre-rendered image
Pre-rendering the *entire* script as one full-color image doesn't scale — a
590-byte test script alone wraps to ~2987px tall, needing ~5.8MB as a
`LV_IMG_CF_TRUE_COLOR` canvas, more than the ~4.4MB of PSRAM actually free
after the display's own frame buffers and other allocations. A real script
would only be worse.

**Fix:** allocate one small, **fixed-size** canvas (2 screen-heights tall —
independent of script length) and treat it as a window sliding through the
full script's virtual height. The key trick: `lv_canvas_draw_text()` will
happily draw starting from a *negative* Y coordinate and clip everything
above the canvas automatically, so "render the slice starting N pixels into
the document" is just `lv_canvas_draw_text(canvas, 0, -N, ..., full_script)`
— no manual line-break/pagination bookkeeping needed, LVGL's own word-wrap
does it every time. Refill (clear, redraw, re-mirror) only when the visible
viewport gets within one screen-height of the edge of what's currently
rendered — at typical scroll speeds that's roughly once every several
seconds, cheap enough not to affect frame rate.

Status: position math and buffer content both verified correct via
diagnostic logging, and — as of the backlight/touch-init fix above — now
confirmed actually rendering on the physical display too.

### Useful diagnostic pattern for "is it a position bug or a content bug?"
When something won't show up on screen, add periodic logging of the actual
state (`lv_obj_get_y()`, a manual pixel-count scan of the canvas buffer for
non-zero values) rather than re-reading the code blind. This cleanly
separated "the scroll math is right and the buffer genuinely has drawn
content in it" from "but nothing reaches the physical screen" in one round,
instead of several more rounds of guessing.

## RESOLVED (root cause identified 2026-09-14): scrolling flicker/choppiness — LVGL's own render+flush cost climbs to ~300ms+ under continuous scrolling

**Symptom:** user-reported "flicker, makes it hard to read" while scrolling.

**Real fixes made along the way (confirmed via log evidence, worth keeping
regardless of the root-cause finding below):**
1. `lv_canvas_fill_bg()` cost ~768,000us per call. Replaced with a direct
   `memset()` (fill color is always solid black = all-zero bytes on
   RGB565) — now ~207,000-320,000us, matching a raw PSRAM memset benchmark
   of the same buffer size. The remaining cost is genuine PSRAM bus
   contention from the RGB panel's continuous framebuffer DMA (confirmed
   via an early pre-panel-init vs. post-panel-init memset benchmark showing
   a 3-4.4x slowdown once the panel starts driving the display), not
   further software inefficiency.
2. `g_window_height` was `screen_height * 2`, which worked out to *zero*
   effective refill margin (the trigger math landed exactly at the
   threshold), causing back-to-back refills instead of the intended ~8s
   spacing. Changed to `* 3`; log-confirmed refill spacing now matches
   predicted math for the test script.
3. `scroll_timer_cb()` originally advanced the scroll position by a
   **fixed** per-tick amount regardless of actual elapsed time — silently
   cutting the real average scroll speed to roughly 1/10th of the
   configured px/sec once it became clear ticks were actually arriving
   every ~300ms+ instead of the intended ~33ms. Fixed to scale the step by
   real elapsed time (`gap_ms`) instead of assuming a fixed period.
4. Added a guard to skip a wasted "refill an already-blank window" event
   that happened right before wrap-to-top, when the test script (1066px)
   was shorter than the (now larger) render window (1800px). **Not yet
   independently re-verified with a dedicated log** — present in the
   flashed firmware but its effect hasn't specifically been confirmed.

**Diagnostic method:** added `g_max_tick_gap_ms` to `main.cpp` (actual
elapsed time between `scroll_timer_cb()` firings, via `millis()`), which
consistently settled around ~306-320ms during normal scrolling (vs. the
intended ~33ms), with spikes to ~950-1040ms lining up with
`refill_window()` calls. To find out *why*, added `printf()`-based
instrumentation (see the `ESP_LOGI/ESP_LOGW` gotcha above for why `printf`
and not `ESP_LOGx`) directly inside the vendor `esp_lv_adapter_arduino.cpp`,
measuring three things separately: (a) `flush_callback()`'s
`ulTaskNotifyTake()` vsync-wait time, (b) `lv_timer_handler()`'s own total
execution time per call, and (c) the full `lvgl_port_task` loop iteration
time.

**Findings, definitive:**
- **Ruled out: Wi-Fi/AP/webserver polling.** Direct paired A/B test
  (`ENABLE_WIFI=1` vs. `0`) showed statistically identical
  `max_tick_gap_ms` in both; only `free_heap` differed.
- **Ruled out: the RGB panel's own vsync timing, and the `flush_callback()`
  blocking-wait theory.** The instrumented vsync wait stayed a steady
  ~23-30ms throughout, matching the calculated panel refresh rate
  (`EXAMPLE_LCD_PIXEL_CLOCK_HZ` + porch/pulse-width constants ≈ 32.75Hz ≈
  30.5ms/frame) almost exactly. This is the *opposite* of the ~300ms
  anomaly, so the flush-blocking mechanism is not the cause.
- **Ruled out: task-scheduling/`vTaskDelay` slippage.** `task_delay_ms`
  (what `lv_timer_handler()` asks the task to sleep) was always `0`
  (clamped up to the 10ms floor) — the loop is not waiting on anything
  external to `lv_timer_handler()` itself.
- **Confirmed root cause: `lv_timer_handler()`'s own execution time climbs
  over time, independent of refills, until it plateaus around the observed
  ~295-320ms** — e.g. one capture showed it climb steadily across roughly
  10-15 seconds (119ms → 173ms → 234ms → 295ms) then hold there, tracking
  `max_tick_gap_ms` almost exactly. This is the actual rendering/flush cost
  of moving our canvas each tick — not a blocked wait, not a scheduling
  gap, just genuinely slow.

**This is almost certainly the same phenomenon as the "periodic ~2-minute
redraw slowdown" entry above** (a documented, unresolved community report —
suspected PSRAM cache-coherency mismatch between the Arduino-ESP32 core and
Octal PSRAM burst behavior), just captured here with much finer
granularity via direct instrumentation rather than coarse tick-interval
sampling. Both entries should be read together; they're very likely one
underlying platform limitation, not two separate bugs. See the same two
reference links there.

**Practical takeaway:** the ~300ms+ stall is not fixable by anything in our
own `main.cpp` scroll/render logic (already-fixed items 1-4 above were
worthwhile independently, but don't touch this), and multiple external
reports treat the underlying cause as an unresolved platform/core
limitation rather than something fixable from application code. **Decision
point, not yet made:** whether to (a) look for an application-level
mitigation that reduces how much area gets re-flushed per tick (e.g.
smaller per-tick invalidation), accepting this is a mitigation and not a
fix, or (b) accept this as a documented platform quirk, same as the
2-minute-oscillation entry above, and move on to the remaining build
phases (controller board, ESP-NOW, enclosure). Revisit this decision before
doing any further internals-level work here — don't restart the
investigation from scratch.

**Diagnostics left in place:** `main.cpp` still has
`g_max_tick_gap_ms`/`diagnostics_timer_cb()`, and
`esp_lv_adapter_arduino.cpp` still has the `printf()`-based `[DIAG]`
instrumentation described above (three checkpoints: entry of
`esp_lv_adapter_init()`, entry of `lvgl_port_task()`'s loop, and a
100-iteration heartbeat plus new-max-only prints for the three measured
timings). Safe to leave in — cheap when nothing new happens — but worth
removing once this is fully closed out, since the `[DIAG]`-prefixed lines
add serial noise.

## Possible ESP32-S3 Octal-PSRAM soft-reset quirk after Wi-Fi save-and-restart (watch for recurrence)
While diagnosing the blank-screen issue above, one boot (following the
web UI's "Save and Restart", which calls `ESP.restart()` right after a
PSRAM-heavy canvas allocation) came up with `rst:0x10 (RTCWDT_RTC_RST)` — a
watchdog reset — and hung right after the "Script wraps..." log line before
finishing setup. This didn't reproduce on every save+restart cycle, so it
wasn't chased further, but it matches a known ESP32-S3 erratum where Octal
PSRAM doesn't always reinitialize cleanly after a *soft* reset
(`esp_restart()`), only after a real power cycle. If this recurs
consistently, the fix is likely avoiding `ESP.restart()` after heavy PSRAM
use (or forcing a harder reset), not a code bug in our sketch.
