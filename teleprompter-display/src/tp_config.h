// tp_config.h - every tunable for the teleprompter lives here.
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Panel: Waveshare ESP32-S3 Touch LCD 7 (1024x600 RGB565)
// ---------------------------------------------------------------------------
#define TP_LCD_H_RES            1024
#define TP_LCD_V_RES            600
#define TP_LCD_PCLK_HZ          (30 * 1000 * 1000)

#define TP_LCD_HSYNC_PULSE      162
#define TP_LCD_HSYNC_BACK       152
#define TP_LCD_HSYNC_FRONT      48
#define TP_LCD_VSYNC_PULSE      45
#define TP_LCD_VSYNC_BACK       13
#define TP_LCD_VSYNC_FRONT      3

#define TP_PIN_VSYNC            3
#define TP_PIN_HSYNC            46
#define TP_PIN_DE               5
#define TP_PIN_PCLK             7
#define TP_PIN_DISP             (-1)

// B3..B7, G2..G7, R3..R7
#define TP_RGB_DATA_PINS        { 14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40 }

// I2C to the CH422G-style IO expander (backlight, resets)
#define TP_PIN_I2C_SDA          8
#define TP_PIN_I2C_SCL          9
#define TP_I2C_FREQ_HZ          (400 * 1000)
#define TP_IO_EXPANDER_ADDR     0x24
// GPIO4 is the touch controller's INT line. We do not use touch, but the line
// has to be driven low through the vendor's power-up sequence, which is also
// what brings the expander (and the panel rails behind it) into a known state.
#define TP_PIN_TOUCH_INT        4

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
// The scanline renderer generates pixels on the fly into the RGB peripheral's
// bounce buffers, so there is no frame buffer in PSRAM at all. Set this to 1
// only if the no-frame-buffer path misbehaves on your board; it falls back to
// two PSRAM frame buffers, which works but is far slower (see docs/renderer-rewrite.md).
#define TP_USE_FRAMEBUFFER      0

// Lines of pixels generated per bounce-buffer refill. MUST divide TP_LCD_V_RES.
// Legal values for 600: 1,2,3,4,5,6,8,10,12,15,20,24,25,...
// Larger  = more slack against interrupt jitter, more internal RAM.
// Smaller = less internal RAM, tighter real-time deadline.
// 12 lines => 2 x 24 KB of internal RAM, ~550 us of slack per refill.
#define TP_BOUNCE_LINES         12

// How long the panel takes to shift out one bounce buffer, i.e. the deadline
// for generating the next one. Reported alongside the measured fill time in
// the status log when TP_RENDER_STATS is on.
#define TP_LCD_H_TOTAL          (TP_LCD_H_RES + TP_LCD_HSYNC_PULSE + \
                                 TP_LCD_HSYNC_BACK + TP_LCD_HSYNC_FRONT)
#define TP_BOUNCE_PERIOD_US     ((TP_BOUNCE_LINES * TP_LCD_H_TOTAL) / \
                                 (TP_LCD_PCLK_HZ / 1000000))

// Measure scanline generation time and report the worst case every 5s. Cheap,
// and it is the number that matters if the panel ever shows a mistimed band.
#define TP_RENDER_STATS         1

// Foreground / background as 0xRRGGBB.
#define TP_COLOR_FG_DEFAULT     0xFFFFFF
#define TP_COLOR_BG_DEFAULT     0x000000

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
#define TP_MARGIN_X             48      // left/right padding, px
#define TP_LINE_SPACE           12      // extra px between baselines
#define TP_LETTER_SPACE         0

#define TP_MAX_SCRIPT_BYTES     (64 * 1024)
#define TP_MAX_LINES            4000

// Rolling window of rasterised text held in PSRAM, 4 bits of alpha per pixel.
// MUST be a power of two. 4096 rows x 512 bytes = 2 MB.
#define TP_RING_ROWS            4096
#define TP_MASK_STRIDE          (TP_LCD_H_RES / 2)

// How far ahead of the visible window the rasteriser keeps the ring filled.
#define TP_LEAD_ROWS            1200
// Upper bound on rows rasterised per top-up call, so one call can never
// monopolise the render task.
#define TP_RASTER_BUDGET_ROWS   400
// Blank rows inserted between the end of the script and its restart.
#define TP_GAP_ROWS             TP_LCD_V_RES

// ---------------------------------------------------------------------------
// Scrolling defaults (overridden by whatever is stored in NVS)
// ---------------------------------------------------------------------------
#define TP_SPEED_DEFAULT        74.0f   // px/sec, used when no target length is set
#define TP_SPEED_MIN            2.0f
#define TP_SPEED_MAX            600.0f
#define TP_BRIGHTNESS_DEFAULT   100     // percent, 100 = brightest

// ---------------------------------------------------------------------------
// Run behaviour
// ---------------------------------------------------------------------------
// Screen row the speaker actually reads from. A run is defined as "first line
// at this row" to "last line at this row", which makes the travel distance
// exactly (text height - one line) and, usefully, independent of where this
// row sits. Moving it changes where the text rests, not the timing.
#define TP_READ_LINE_Y          (TP_LCD_V_RES / 3)

// Park at the top on boot and after a rewind instead of scrolling straight
// away, so you can start recording and then start the script.
#define TP_START_PAUSED         1

// Pause when the last line reaches the reading row, instead of looping back
// round to the start. Set to 0 for a continuously looping display.
#define TP_STOP_AT_END          1

// Target run length in seconds; 0 means run at a fixed px/sec instead.
// Overridden by whatever is stored in NVS.
#define TP_DURATION_DEFAULT_S   0
// Largest bias a remote's trim control can apply, as a fraction of the base
// speed. 0.5 means the stick can range from half to one-and-a-half times.
#define TP_TRIM_MAX             0.5f
// Drop the trim back to zero if the remote has not been heard from for this
// long, so a flat battery mid-deflection cannot leave the scroll running fast.
#define TP_LINK_TIMEOUT_MS      1500

// Clamp for a speed derived from a target length. Deliberately wider than the
// slider: a very short script over a long target is legitimately slow.
#define TP_PACE_SPEED_MIN       0.5f

// ---------------------------------------------------------------------------
// Access point
// ---------------------------------------------------------------------------
// Pinned so the ESP-NOW remote has a fixed channel to join. Changing this
// means changing it on the remote too.
#define TP_AP_CHANNEL           1
#define TP_AP_SSID              "Teleprompter"
#define TP_AP_PASSWORD          "teleprompter123"   // 8+ chars, or AP fails

static_assert(TP_LCD_V_RES % TP_BOUNCE_LINES == 0,
              "TP_BOUNCE_LINES must divide TP_LCD_V_RES");
static_assert((TP_RING_ROWS & (TP_RING_ROWS - 1)) == 0,
              "TP_RING_ROWS must be a power of two");
static_assert(TP_RING_ROWS > TP_LCD_V_RES + TP_LEAD_ROWS + 512,
              "ring must be comfortably larger than the visible window + lead");
