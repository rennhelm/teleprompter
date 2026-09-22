// Arduino.h first, deliberately: ESP_LOGx only produces serial output in
// translation units that pull in esp32-hal-log.h through it. The old vendor
// files logged into the void for exactly this reason.
#include <Arduino.h>
#include "tp_text.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lvgl.h"

LV_FONT_DECLARE(Roboto_96);

static const char *TAG = "tp_text";

tp_text_state_t g_tp_text = { nullptr, 0 };

struct tp_line_t {
    uint32_t start;     // byte offset into s_script
    uint32_t len;
};

static const lv_font_t *s_font = &Roboto_96;

static char      *s_script      = nullptr;
static size_t     s_script_len  = 0;
static tp_line_t *s_lines       = nullptr;
static int        s_line_count  = 0;
static bool       s_mirror      = false;

static int        s_cursor      = 0;    // next line to rasterise
static int        s_gap_left    = 0;    // blank rows still owed before restart
static int        s_missing     = 0;
static int        s_words       = 0;
static bool       s_counting    = false;   // only tally substitutions during layout

static SemaphoreHandle_t s_lock = nullptr;

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

static uint32_t utf8_next(const char *s, size_t len, size_t *i)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t k = *i;
    if (k >= len) return 0;

    uint8_t c = p[k];
    if (c < 0x80) { *i = k + 1; return c; }

    int extra;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0)      { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                         { *i = k + 1; return '?'; }   // stray continuation

    if (k + extra >= len) { *i = len; return '?'; }
    for (int n = 1; n <= extra; n++) {
        uint8_t cc = p[k + n];
        if ((cc & 0xC0) != 0x80) { *i = k + 1; return '?'; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i = k + extra + 1;
    return cp;
}

static uint32_t utf8_peek(const char *s, size_t len, size_t i)
{
    size_t tmp = i;
    return utf8_next(s, len, &tmp);
}

// ---------------------------------------------------------------------------
// Font helpers
// ---------------------------------------------------------------------------

// LVGL 8's own range test in get_glyph_dsc_id() is `rcp > range_length`, which
// lets the single codepoint immediately after each cmap through and resolves it
// to the first glyph of the next range. With Roboto_96's two ranges (0x20-0x2F
// and 0x31-0x7E) that means '0' is reported as present and drawn as '1' - dates
// and figures come out silently wrong. Check the ranges ourselves first.
static bool font_has(uint32_t cp)
{
    const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *)s_font->dsc;
    if (fdsc) {
        bool in_range = false;
        for (uint32_t i = 0; i < fdsc->cmap_num; i++) {
            if (cp - fdsc->cmaps[i].range_start < fdsc->cmaps[i].range_length) {
                in_range = true;
                break;
            }
        }
        if (!in_range) return false;
    }

    lv_font_glyph_dsc_t d;
    return lv_font_get_glyph_dsc(s_font, &d, cp, 0);
}

// Map a codepoint onto something this font can actually draw.
static uint32_t resolve_cp(uint32_t cp)
{
    if (cp == 0) return 0;
    if (font_has(cp)) return cp;

    if (s_counting) s_missing++;

    // The original Roboto_96.c was generated without '0' in its symbol list.
    // The font shipped here has it, but keep the fallback so an older font
    // asset degrades to something readable instead of a wrong digit.
    if (cp == '0' && font_has('O')) return 'O';

    // Common typographic characters people paste in from word processors.
    switch (cp) {
    case 0x2018: case 0x2019: cp = '\''; break;   // curly single quotes
    case 0x201C: case 0x201D: cp = '"';  break;   // curly double quotes
    case 0x2013: case 0x2014: cp = '-';  break;   // en/em dash
    case 0x2026: cp = '.'; break;                 // ellipsis
    case 0x00A0: cp = ' '; break;                 // non-breaking space
    default: cp = '?'; break;
    }
    return font_has(cp) ? cp : ' ';
}

// Control characters are never drawn, so they must not be resolved (which
// would count them as missing glyphs) or fed to the kerning lookup.
static inline uint32_t kern_neighbour(uint32_t nx)
{
    return (nx < 0x20) ? 0 : resolve_cp(nx);
}

static int glyph_advance(uint32_t cp, uint32_t next_cp)
{
    lv_font_glyph_dsc_t d;
    if (!lv_font_get_glyph_dsc(s_font, &d, cp, next_cp)) return 0;
    return (int)d.adv_w + TP_LETTER_SPACE;
}

int tp_text_line_height(void)
{
    return (int)s_font->line_height + TP_LINE_SPACE;
}

int tp_text_line_count(void) { return s_line_count; }
int tp_text_missing_glyphs(void) { return s_missing; }
int tp_text_word_count(void) { return s_words; }

int32_t tp_text_rows(void)
{
    return (int32_t)s_line_count * tp_text_line_height();
}

int32_t tp_text_loop_rows(void)
{
    return tp_text_rows() + TP_GAP_ROWS;
}

// ---------------------------------------------------------------------------
// Ring access
// ---------------------------------------------------------------------------

static inline uint8_t *ring_row(int32_t y)
{
    return g_tp_text.ring + (uint32_t)(y & (TP_RING_ROWS - 1)) * TP_MASK_STRIDE;
}

static inline void publish_fill(int32_t hi)
{
    __sync_synchronize();           // rows are written before they are announced
    g_tp_text.fill_hi = hi;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void push_line(size_t start, size_t len)
{
    if (s_line_count >= TP_MAX_LINES) return;
    s_lines[s_line_count].start = (uint32_t)start;
    s_lines[s_line_count].len   = (uint32_t)len;
    s_line_count++;
}

// Byte-wise is safe here: UTF-8 continuation bytes are all >= 0x80, so they
// can never be mistaken for whitespace.
static int count_words(void)
{
    int words = 0;
    bool in_word = false;
    for (size_t i = 0; i < s_script_len; i++) {
        const unsigned char c = (unsigned char)s_script[i];
        const bool space = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (space) in_word = false;
        else if (!in_word) { words++; in_word = true; }
    }
    return words;
}

static void layout(void)
{
    const int max_w = TP_LCD_H_RES - 2 * TP_MARGIN_X;

    s_line_count = 0;
    s_missing = 0;
    s_counting = true;
    s_words = count_words();

    size_t i = 0;
    size_t line_start = 0;
    size_t break_at = SIZE_MAX;     // byte index of the last space seen
    int w = 0;

    while (i < s_script_len) {
        const size_t cur = i;
        uint32_t cp = utf8_next(s_script, s_script_len, &i);

        if (cp == '\n') {
            push_line(line_start, cur - line_start);
            line_start = i;
            w = 0;
            break_at = SIZE_MAX;
            continue;
        }
        if (cp < 0x20 && cp != '\t') continue;      // CR and other controls

        if (cp == ' ' || cp == '\t') break_at = cur;

        const uint32_t nx = utf8_peek(s_script, s_script_len, i);
        const int adv = glyph_advance(resolve_cp(cp), kern_neighbour(nx));

        if (w + adv > max_w && cur > line_start) {
            size_t next_start;
            if (break_at != SIZE_MAX && break_at > line_start) {
                push_line(line_start, break_at - line_start);
                next_start = break_at;
                // Swallow the run of spaces that caused the break.
                while (next_start < s_script_len &&
                       (s_script[next_start] == ' ' || s_script[next_start] == '\t')) {
                    next_start++;
                }
            } else {
                // A single word longer than the line: break mid-word.
                push_line(line_start, cur - line_start);
                next_start = cur;
            }
            line_start = next_start;
            i = next_start;
            w = 0;
            break_at = SIZE_MAX;
            continue;
        }

        w += adv;
    }

    if (line_start < s_script_len || s_line_count == 0) {
        push_line(line_start, s_script_len - line_start);
    }

    s_counting = false;

    if (s_missing) {
        ESP_LOGW(TAG, "%d character(s) are not in Roboto_96 and were substituted", s_missing);
    }
    ESP_LOGI(TAG, "laid out %d lines / %d words, %d px per line, %d rows of text",
             s_line_count, s_words, tp_text_line_height(), (int)tp_text_rows());
}

// ---------------------------------------------------------------------------
// Rasterisation
// ---------------------------------------------------------------------------

// One pixel of glyph coverage, as 4 bits, from LVGL's continuous bitstream.
static inline uint8_t sample_alpha(const uint8_t *m, uint32_t bit, uint8_t bpp)
{
    switch (bpp) {
    case 4:  { uint8_t b = m[bit >> 3]; return (bit & 4) ? (b & 0x0F) : (uint8_t)(b >> 4); }
    case 8:  return (uint8_t)(m[bit >> 3] >> 4);
    case 2:  { uint8_t b = m[bit >> 3]; return (uint8_t)(((b >> (6 - (bit & 7))) & 0x03) * 5); }
    case 1:  { uint8_t b = m[bit >> 3]; return (uint8_t)(((b >> (7 - (bit & 7))) & 0x01) ? 15 : 0); }
    default: return 0;
    }
}

static inline void put_alpha_max(uint8_t *row, int x, uint8_t a)
{
    uint8_t *p = row + (x >> 1);
    const uint8_t b = *p;
    if (x & 1) { if ((b & 0x0F) < a) *p = (uint8_t)((b & 0xF0) | a); }
    else       { if ((b >> 4)   < a) *p = (uint8_t)((b & 0x0F) | (uint8_t)(a << 4)); }
}

static void blit_glyph(const uint8_t *bmp, const lv_font_glyph_dsc_t *d,
                       int pen_x, int32_t line_top, int line_h)
{
    const int gx = pen_x + d->ofs_x;
    // Same placement rule LVGL uses in lv_draw_letter().
    const int gy = ((int)s_font->line_height - (int)s_font->base_line) - (int)d->box_h - d->ofs_y;
    const uint8_t bpp = d->bpp ? d->bpp : 4;

    uint32_t bit = 0;
    for (int row = 0; row < (int)d->box_h; row++) {
        const int ry = gy + row;
        uint8_t *dst = (ry >= 0 && ry < line_h) ? ring_row(line_top + ry) : nullptr;

        for (int col = 0; col < (int)d->box_w; col++) {
            const uint8_t a = sample_alpha(bmp, bit, bpp);
            bit += bpp;
            if (!a || !dst) continue;

            int x = gx + col;
            if (s_mirror) x = TP_LCD_H_RES - 1 - x;
            if ((unsigned)x >= (unsigned)TP_LCD_H_RES) continue;
            put_alpha_max(dst, x, a);
        }
    }
}

static int raster_blank(int rows)
{
    const int32_t top = g_tp_text.fill_hi;
    for (int i = 0; i < rows; i++) memset(ring_row(top + i), 0, TP_MASK_STRIDE);
    publish_fill(top + rows);
    return rows;
}

static int raster_line(int index)
{
    const int line_h = tp_text_line_height();
    const int32_t top = g_tp_text.fill_hi;

    for (int i = 0; i < line_h; i++) memset(ring_row(top + i), 0, TP_MASK_STRIDE);

    const tp_line_t ln = s_lines[index];
    const size_t end = ln.start + ln.len;
    size_t i = ln.start;
    int pen = TP_MARGIN_X;

    while (i < end) {
        uint32_t cp = utf8_next(s_script, end, &i);
        if (cp == 0) break;
        // Line ranges can still carry a trailing CR from a CRLF script.
        if (cp < 0x20 && cp != '\t') continue;

        const uint32_t nx = (i < end) ? kern_neighbour(utf8_peek(s_script, end, i)) : 0;
        cp = resolve_cp(cp);

        lv_font_glyph_dsc_t d;
        if (!lv_font_get_glyph_dsc(s_font, &d, cp, nx)) continue;

        if (d.box_w && d.box_h) {
            const uint8_t *bmp = lv_font_get_glyph_bitmap(s_font, cp);
            if (bmp) blit_glyph(bmp, &d, pen, top, line_h);
        }
        pen += (int)d.adv_w + TP_LETTER_SPACE;
    }

    publish_fill(top + line_h);
    return line_h;
}

// Produce the next unit of the endless stream: one text line, a chunk of the
// end-of-script gap, or a restart.
static int raster_next_unit(void)
{
    if (s_line_count == 0) return raster_blank(150);

    if (s_gap_left <= 0 && s_cursor >= s_line_count) {
        s_cursor = 0;                   // wrap: blank gap, then the script again
        s_gap_left = TP_GAP_ROWS;
    }
    if (s_gap_left > 0) {
        const int n = s_gap_left > 150 ? 150 : s_gap_left;
        s_gap_left -= n;
        return raster_blank(n);
    }
    return raster_line(s_cursor++);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool tp_text_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    g_tp_text.ring = (uint8_t *)heap_caps_malloc((size_t)TP_RING_ROWS * TP_MASK_STRIDE,
                                                 MALLOC_CAP_SPIRAM);
    s_lines  = (tp_line_t *)heap_caps_malloc(sizeof(tp_line_t) * TP_MAX_LINES, MALLOC_CAP_SPIRAM);
    s_script = (char *)heap_caps_malloc(TP_MAX_SCRIPT_BYTES + 1, MALLOC_CAP_SPIRAM);

    if (!s_lock || !g_tp_text.ring || !s_lines || !s_script) {
        ESP_LOGE(TAG, "out of memory: ring/lines/script allocation failed");
        return false;
    }

    memset(g_tp_text.ring, 0, (size_t)TP_RING_ROWS * TP_MASK_STRIDE);
    s_script[0] = '\0';
    ESP_LOGI(TAG, "mask ring: %d rows x %d bytes = %u KB in PSRAM",
             TP_RING_ROWS, TP_MASK_STRIDE,
             (unsigned)((size_t)TP_RING_ROWS * TP_MASK_STRIDE / 1024));
    return true;
}

void tp_text_rewind(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cursor = 0;
    s_gap_left = 0;
    publish_fill(0);
    xSemaphoreGive(s_lock);
}

void tp_text_set_script(const char *utf8, size_t len, bool mirrored)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (len > TP_MAX_SCRIPT_BYTES) len = TP_MAX_SCRIPT_BYTES;
    memcpy(s_script, utf8, len);
    s_script[len] = '\0';
    s_script_len = len;
    s_mirror = mirrored;

    layout();

    s_cursor = 0;
    s_gap_left = 0;
    publish_fill(0);

    xSemaphoreGive(s_lock);
}

void tp_text_ensure(int32_t target_row, int budget_rows)
{
    if (g_tp_text.fill_hi >= target_row) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int spent = 0;
    while (g_tp_text.fill_hi < target_row) {
        spent += raster_next_unit();
        if (budget_rows > 0 && spent >= budget_rows) break;
    }
    xSemaphoreGive(s_lock);
}
