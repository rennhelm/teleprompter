// tp_display.h - RGB panel bring-up and the scanline renderer.
//
// Nothing composes a frame ahead of time. The RGB peripheral asks for the next
// strip of scanlines and we expand them straight out of the 4bpp text mask, so
// the cost per frame is fixed, there is no frame buffer traffic in PSRAM, and
// scroll position advances from a hardware-paced timestamp rather than from
// whatever the application happened to get around to.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "tp_config.h"

esp_err_t tp_display_init(void);

void tp_display_set_speed(float px_per_sec);    // negative scrolls backwards
void tp_display_set_paused(bool paused);
bool tp_display_paused(void);

// Blank the output without touching PSRAM. Hold the display across anything
// that rewrites the text mask or writes to flash, then release it.
void tp_display_set_hold(bool hold);

void tp_display_rewind(void);                   // back to the start, clock reset
void tp_display_set_colors(uint32_t fg_rgb888, uint32_t bg_rgb888);

// Extent of a run, in image rows. The renderer stops (and reports finished)
// on reaching end_row, and will not reverse past start_row.
void    tp_display_set_range(int32_t start_row, int32_t end_row);
int32_t tp_display_start_row(void);
int32_t tp_display_end_row(void);
bool    tp_display_finished(void);

#if TP_RENDER_STATS
// Worst scanline-generation time since the last call, in microseconds. Compare
// against TP_BOUNCE_PERIOD_US: if it gets close, the panel is being fed late
// and will show a mistimed band.
uint32_t tp_display_take_max_fill_us(void);
#endif

uint32_t tp_display_run_ms(void);               // time spent actually scrolling
int32_t  tp_display_view_row(void);             // top image row currently shown
uint32_t tp_display_frames(void);               // frames generated since boot

#if TP_USE_FRAMEBUFFER
// Fallback path only: renders one frame into the back buffer and swaps.
void tp_display_render_frame_blocking(void);
#endif
