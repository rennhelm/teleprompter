// tp_text.h - script layout and rasterisation into a rolling 4bpp alpha mask.
//
// The script is laid out into wrapped lines once, then rasterised lazily into a
// ring of PSRAM rows. Row numbers ("image rows") increase forever: when the
// rasteriser reaches the end of the script it emits a blank gap and starts the
// script again, so the renderer just walks an endless stream and never has to
// deal with looping.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_config.h"

struct tp_text_state_t {
    uint8_t *ring;              // TP_RING_ROWS * TP_MASK_STRIDE bytes in PSRAM
    volatile int32_t fill_hi;   // rows [fill_hi - TP_RING_ROWS, fill_hi) are valid
};

extern tp_text_state_t g_tp_text;

// Row lookup used by the scanline renderer. Kept in the header so it inlines
// into the interrupt handler: no flash call, no locking.
static inline const uint8_t *tp_text_row_ptr(int32_t y)
{
    const int32_t hi = g_tp_text.fill_hi;
    if (y < 0 || y >= hi || y < hi - TP_RING_ROWS) return nullptr;
    return g_tp_text.ring + (uint32_t)(y & (TP_RING_ROWS - 1)) * TP_MASK_STRIDE;
}

bool tp_text_init(void);

// Replace the script and rewind the rasteriser. The caller must hold the
// display blank across this call (see tp_display_set_hold).
void tp_text_set_script(const char *utf8, size_t len, bool mirrored);

// Rewind to the top of the current script without re-laying it out.
void tp_text_rewind(void);

// Rasterise forward until the ring covers `target_row`, spending at most
// `budget_rows` rows of work (0 = unlimited).
void tp_text_ensure(int32_t target_row, int budget_rows);

int  tp_text_line_height(void);
int  tp_text_line_count(void);
int32_t tp_text_rows(void);             // height of the laid-out script itself
int32_t tp_text_loop_rows(void);        // rows in one pass, including the gap
int  tp_text_word_count(void);
int  tp_text_missing_glyphs(void);      // characters the font could not supply
