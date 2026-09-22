# Teleprompter — Run model: target length and start-paused (2026-09-14)

Adds "set a target length and the script finishes in that time", plus booting
paused. Builds on the scanline renderer (see renderer-rewrite.md).

## The run model, and why the definition matters

A **run** is defined as: *first line sitting on the reading row* → *last line
sitting on the reading row*. The reading row is `TP_READ_LINE_Y` in
`tp_config.h`, default `V_RES / 3` = 200 px.

The useful consequence: the distance covered is exactly

```
travel = text_rows - line_height
```

which is **independent of where the reading row sits**. Moving the reading row
changes where the text rests on screen, not the timing. So:

```
speed_px_per_sec = travel / target_seconds
```

is the whole calculation. The start position is `-TP_READ_LINE_Y` (negative, so
there is blank above line one); `tp_text_row_ptr()` already returns null for
negative rows, so no special case was needed.

Any other definition of "finished" is materially wrong. If the run ended when
the last line scrolled off the *top*, the target would be overshot by one screen
height of travel — about 20% of a two minute run on a typical script — because
the speaker stops talking a screen-height before the text stops moving.

### Verified on host

`/tmp` harness rendered the frames at both extremes against the real font:
at the start position line one sits on the reading row with blank above; at the
end position the final line sits on that same row, earlier lines visible above
it for context, and **zero ink below it**. Numbers from the check: 25 lines,
69 words, text 2875 rows, travel 2760, so 2:00 → 23.0 px/s.

## Deliberately open loop

The target sets the **initial speed only**. The slider and the ESP-NOW hooks
still move it freely afterwards. There is no continuous auto-correction, on
purpose: a speed that creeps around while you are reading is horrible to read
from. Instead the control page shows the consequences and offers a manual fix:

- run clock `0:47 / 2:00`
- what the current speed projects to, and time left
- drift, "0:03 ahead" / "0:03 behind", once you have nudged the speed
- words per minute, flagged amber above 200 — the real sanity check, since a
  two minute target on a long script may imply an impossible reading rate
- **Re-pace**: recompute speed from distance left over time left

If continuous auto-pacing is ever wanted, it belongs behind a flag, not as the
default.

## Start paused, stop at end

- `TP_START_PAUSED` (default 1): boots parked with line one on the reading row.
  `s_paused` is initialised to this at file scope in `tp_display.cpp`, so there
  is no window between panel init and the first `set_paused()` call.
- `TP_STOP_AT_END` (default 1): the renderer pauses and latches `finished` when
  it reaches `end_row`, instead of looping. Reversing past `start_row` also
  stops. Set both to 0 for a continuously looping display.
- "Back to top" parks paused too — a rewind means "getting ready for a take".
- Play on a finished run rewinds first, and the page's button reads "Restart".

## Implementation notes

- **Run clock lives in the frame callback**, accumulated from the same `dt` the
  scroll position uses, so it measures scrolling rather than wall time and stops
  cleanly when paused. Kept as a 32-bit millisecond counter specifically so a
  task can read it without tearing against the interrupt that writes it (a
  64-bit microsecond counter could not be read atomically).
- **`apply_pacing()` is re-run on every layout change** — new script, mirror
  toggle — which is the real value of the feature: paste a longer script and the
  target holds, the scroll just slows down.
- `reload()` rewinds *before* re-laying out so the mask top-up task is not
  chasing a target derived from the old script, then rewinds again once the new
  range is known.
- Word count is a separate byte-wise pass (UTF-8 continuation bytes are all
  >= 0x80, so they can never be mistaken for whitespace). The layout loop itself
  rescans characters on line breaks, so counting inside it would double-count.
- New API: `tp_display_set_range()`, `tp_display_start_row()`,
  `tp_display_end_row()`, `tp_display_finished()`, `tp_display_run_ms()`,
  `tp_text_rows()`, `tp_text_word_count()`, `tp_app_set_duration()`,
  `tp_app_repace()`.
- NVS key `duration` (seconds, 0 = off) alongside `speed`, `mirror`, `bright`.

## Control page verified in a browser

The page was rendered headless against mock state at 400 px and 760 px wide:
no JS errors, and the arithmetic checked by hand against the mock (0:47 of 2:00,
1:16 left, 3 s behind, 35 wpm, progress bar at 37%). The firmware side of this
change has **not** been run on the board.

## Note for the ESP-NOW remote (Phase 6)

The run model gives the remote more to work with than play/pause/speed:
`tp_display_finished()` and `tp_display_run_ms()` are worth surfacing on the
handheld, and `tp_app_repace()` is a good candidate for a physical button —
"put me back on schedule" is a more useful control mid-take than nudging speed
by hand.
