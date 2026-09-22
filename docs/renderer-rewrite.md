# Teleprompter — Renderer rewrite (2026-09-14)

Closes the open decision in architecture-plan.md ("pursue an application-level
mitigation, or accept the ~300ms LVGL stall as a platform quirk"). **Neither.**
The third option was taking LVGL out of the render path entirely.

## Why the old approach could not be tuned into working

The earlier investigation was correct that `lv_timer_handler()` cost climbed to a
~300ms plateau, and correct that it wasn't vsync, Wi-Fi or scheduling. The
missing piece is *why* that ceiling exists, and it isn't a cache-coherency
erratum — it's arithmetic:

- The panel's own refresh DMA reads the 1.2 MB PSRAM frame buffer continuously,
  ~40 MB/s, whether or not anything changes. (With `bounce_buffer_size_px` set,
  that read is done by a CPU `memcpy` inside the DMA ISR, ~1960 interrupts/sec.)
- Scrolling a canvas means most of the screen is dirty every frame, so LVGL must
  re-blit ~614k pixels through its draw pipeline into PSRAM.
- ESP32-S3 PSRAM writes go through a **write-allocate** cache, so writing a frame
  costs a read *and* a write — roughly 2.4 MB of bus traffic per frame on top of
  the panel's own 40 MB/s.

Any design that composes a full-screen frame in PSRAM is bandwidth-bound on this
board. That is why mode 1 (full-refresh) was "too slow", why `lv_canvas_fill_bg`
was 768 ms, and why the raw `memset` benchmark only reached ~12 MB/s.

## What replaced it: generate scanlines, keep no frame buffer

The RGB panel now runs in ESP-IDF's no-frame-buffer mode
(`esp_lcd_rgb_panel_config_t.flags.no_fb = 1`, which **requires `num_fbs == 0`**).
In that mode the driver calls `on_bounce_empty(panel, buf, pos_px, len_bytes, ctx)`
to have the application fill each bounce buffer just before it is shifted out.

1. The script is rasterised **once** into a rolling 4-bits-per-pixel alpha mask in
   PSRAM (`TP_RING_ROWS` = 4096 rows x 512 bytes = 2 MB). Mirroring is baked in at
   rasterisation, never per frame.
2. The callback expands the 12 scanlines it is about to send straight from that
   mask through a 256-entry lookup table: one lookup and one aligned 32-bit store
   per two pixels.
3. Scroll position advances inside that callback from `esp_timer_get_time()`, in
   integer fixed point (no FPU in an ISR), latched once per frame at `pos_px == 0`.

Results by construction, not by tuning:
- PSRAM traffic per frame: ~2.4 MB → ~300 KB (mask reads only). Pixels are built
  in internal SRAM.
- Per-frame cost is **constant** — there is no periodic refill left to hitch on.
- Scrolling cannot be stalled by software. Wi-Fi, HTTP, flash writes and
  rasterising the next line all sit outside the render path.
- No tearing: pixels are generated as they are scanned out.
- Budget: ~1.8M cycles/frame of scanline generation ≈ 25% of one core, on core 1.
  Wi-Fi and the web server on core 0.

Fallback if `no_fb` ever misbehaves: `TP_USE_FRAMEBUFFER 1` in `tp_config.h`
switches to two PSRAM frame buffers filled by the same row-expansion code. Much
slower (~10-15 fps), but conventional, so it is a useful A/B.

## Silent bug found: LVGL resolves the codepoint just past a cmap range

`lv_font_get_glyph_dsc_id()` in LVGL 8 tests `rcp > range_length` rather than
`>=`. The single codepoint immediately after each cmap is therefore reported as
present and resolved to the **first glyph of the next range**.

`Roboto_96.c` was generated with `--symbols ...123456789...` — no `'0'` — and its
two ranges are 0x20-0x2F and 0x31-0x7E. So `'0'` (0x30) resolved to `'1'`:
**"2026" was rendering on screen as "2126"**, and every price and time was wrong,
with no error anywhere. Verified by host-rendering the real font.

Two fixes, both applied:
- `font_has()` in `tp_text.cpp` walks the cmaps itself, so a character the font
  lacks is always treated as missing rather than drawn as its neighbour.
- `src/Roboto_96.c` regenerated over 0x20-0x7E plus en/em dash, curly quotes and
  ellipsis, from the same Roboto variable font. Metrics are identical
  (`line_height` 103, `base_line` 23), and the `__has_include` header block is
  reproduced. To regenerate (e.g. a different size or typeface), with
  [lv_font_conv](https://github.com/lvgl/lv_font_conv) and a Roboto `.ttf`:

  ```
  npx lv_font_conv --bpp 4 --size 96 --no-compress --font roboto.ttf \
    -r 0x20-0x7E -r 0x2013-0x2014 -r 0x2018-0x2019 -r 0x201C-0x201D -r 0x2026 \
    --format lvgl -o src/Roboto_96.c
  ```

  Keep `0x20-0x7E` as one unbroken range. A gap is what caused the `'0'` bug.

## Gotchas from hardware-gotchas.md that are honoured in the new code

- **`ESP_LOGx` needs `Arduino.h` in the translation unit.** Every new source file
  includes it first, deliberately, with a comment saying why. Logging is visible
  this time; no `printf` workaround needed.
- **`touch_gt911_init()` was load-bearing for the display, not for touch.**
  `touch_gt911_init()` is gone, but `tp_board_init()` runs the same sequence in
  the same order before the panel: I2C up, expander mode register to all-outputs,
  then the GPIO4 / expander-IO1 reset dance with the same delays. LCD reset (IO3)
  high; backlight (IO2) explicitly low until 120 ms after the panel is scanning,
  so there is no garbage flash at boot. `tp_board_backlight()` really writes to
  the expander — it is not the vendor's commented-out stub.
- **`lib/lvgl/` + `lib/lv_conf.h` sibling layout** is unchanged. LVGL is now only
  a font container (`lv_font_get_glyph_dsc` / `lv_font_get_glyph_bitmap`).
  Nothing calls `lv_init()`, so its 48 KB pool is never allocated.
- **The Octal-PSRAM soft-reset quirk** can no longer be hit in normal use: saving
  a script re-lays out in place instead of calling `ESP.restart()`.
- Waveshare driver files deleted: `esp_lv_adapter_arduino.*`, `rgb_lcd_port.*`,
  `gt911.*`, `touch.*`, `i2c.*`, `io_extension.*`, along with all `[DIAG]`
  instrumentation.

## ESP-NOW hooks (Phase 6) are intact

`tp_display_set_speed(float)` and `tp_display_set_paused(bool)` are what the
remote's receive callback should call; `tp_app_set_speed()` / `tp_app_set_paused()`
/ `tp_app_rewind()` additionally persist and reflect on the web page. Speed is
signed, so reverse is a negative value with no other change. Wi-Fi stays in AP
mode, so the ESP-NOW channel is still pinned.

## Verification status

Host-compiled and rendered against real LVGL 8.4 font code and the actual font:
layout, word wrap, mid-word breaking, blank lines, glyph placement,
anti-aliasing, 4bpp mask packing, mirroring and the lookup-table expansion were
all checked as images. The ESP-IDF panel configuration was checked against
`esp_lcd_panel_rgb.h` / `.c` for IDF 5.5 (what pioarduino pulls in), in
particular that `no_fb` requires `num_fbs == 0`, that the frame-buffer size must
be a whole multiple of the bounce-buffer size (so `TP_BOUNCE_LINES` must divide
600), and the `on_bounce_empty` signature.

**Not yet flashed or run on the board.** First log lines to check:

```
I tp_display: scanline renderer active: no frame buffer, 12-line bounce buffers
I tp_text: laid out N lines, 115 px per line, ... rows per pass
I teleprompter: 32 fps | row ... | heap ... | psram ...
```

A steady ~32 fps with the row number climbing evenly is the fix working.
If bands of the screen tear or repeat under heavy Wi-Fi load, raise
`TP_BOUNCE_LINES` to 20 (costs 80 KB of internal RAM instead of 48 KB).

> Note (repo docs pass): "not yet flashed" above is as of this doc's original
> date (2026-09-14); architecture-plan.md's later changelog says the stutter
> fix was confirmed on hardware. Treat the verification note as historical.
