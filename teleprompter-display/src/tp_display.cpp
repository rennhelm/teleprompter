// Arduino.h first, deliberately: ESP_LOGx only produces serial output in
// translation units that pull in esp32-hal-log.h through it. The old vendor
// files logged into the void for exactly this reason.
#include <Arduino.h>
#include "tp_display.h"

#include <string.h>

#include <esp_attr.h>
#include <esp_check.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tp_text.h"

static const char *TAG = "tp_display";

static esp_lcd_panel_handle_t s_panel = nullptr;

// ---------------------------------------------------------------------------
// Colour lookup. s_lut maps one mask byte (two 4-bit alpha pixels) to two
// packed RGB565 pixels, which is the whole of the per-pixel work. Both tables
// live in .bss, i.e. internal RAM, so the interrupt handler never waits on
// flash or PSRAM for them.
// ---------------------------------------------------------------------------
static uint32_t s_lut[256];
static uint16_t s_ramp[16];
static uint32_t s_bg_pair;

// ---------------------------------------------------------------------------
// Scroll state. Advanced once per frame from inside the renderer, so it is
// paced by the panel itself and is immune to anything the CPU is busy with.
// ---------------------------------------------------------------------------
static volatile int32_t  s_pos_row   = 0;
static volatile uint32_t s_pos_frac  = 0;       // 16.16 fraction of a row
static volatile int32_t  s_speed_q16 = 0;       // px/sec, 16.16 (sign tests only)
static volatile int32_t  s_step_k    = 0;       // px/sec scaled so that
                                                // (dt_us * k) >> 20 == step in q16
static volatile int32_t  s_row_base  = 0;       // latched for the frame in flight
static volatile bool     s_paused    = (TP_START_PAUSED != 0);
static volatile bool     s_hold      = false;
static volatile uint32_t s_frames    = 0;
static int64_t           s_last_us   = 0;

// Run extent. A run goes from the first line sitting on the reading row to the
// last line sitting on it, so the distance is exactly (text height - one line)
// however high up the reading row is. s_start_row is normally negative: the
// script begins below the top of the screen.
static volatile int32_t  s_start_row = 0;
static volatile int32_t  s_end_row   = 0;
static volatile bool     s_finished  = false;

// Run clock, accumulated only while actually scrolling. Kept as 32-bit ms so a
// task can read it without tearing against the interrupt that writes it.
static volatile uint32_t s_run_ms    = 0;
static uint32_t          s_run_us_rem = 0;

#if TP_RENDER_STATS
static volatile uint32_t s_max_fill_us = 0;
#endif

static inline uint16_t rgb565(uint32_t rgb888)
{
    const uint32_t r = (rgb888 >> 16) & 0xFF;
    const uint32_t g = (rgb888 >> 8) & 0xFF;
    const uint32_t b = rgb888 & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void tp_display_set_colors(uint32_t fg, uint32_t bg)
{
    for (int a = 0; a < 16; a++) {
        const uint32_t r = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * (15 - a)) / 15;
        const uint32_t g = (((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * (15 - a)) / 15;
        const uint32_t b = ((fg & 0xFF) * a + (bg & 0xFF) * (15 - a)) / 15;
        s_ramp[a] = rgb565((r << 16) | (g << 8) | b);
    }
    // High nibble is the left-hand pixel, and a little-endian 32-bit store puts
    // it in the low half. tp_text.cpp writes the mask to match.
    for (int v = 0; v < 256; v++) {
        s_lut[v] = (uint32_t)s_ramp[v >> 4] | ((uint32_t)s_ramp[v & 0x0F] << 16);
    }
    s_bg_pair = s_lut[0];
}

// ---------------------------------------------------------------------------
// Scanline generation
// ---------------------------------------------------------------------------

// One full 1024px scanline: 512 table lookups, 512 aligned 32-bit stores.
static IRAM_ATTR void fill_row_full(uint32_t *d, int32_t img_y)
{
    const uint8_t *src = s_hold ? nullptr : tp_text_row_ptr(img_y);

    if (!src) {
        const uint32_t bg = s_bg_pair;
        for (int i = 0; i < TP_MASK_STRIDE; i++) d[i] = bg;
        return;
    }

    const uint32_t *lut = s_lut;
    int i = 0;
    for (; i + 4 <= TP_MASK_STRIDE; i += 4) {
        d[i + 0] = lut[src[i + 0]];
        d[i + 1] = lut[src[i + 1]];
        d[i + 2] = lut[src[i + 2]];
        d[i + 3] = lut[src[i + 3]];
    }
    for (; i < TP_MASK_STRIDE; i++) d[i] = lut[src[i]];
}

// Partial scanline. Only reachable if the bounce buffer is not a whole number
// of rows, which the static_assert in tp_config.h rules out; kept for safety.
static IRAM_ATTR void fill_row_partial(uint16_t *dst, int32_t img_y, int x, int n)
{
    const uint8_t *src = s_hold ? nullptr : tp_text_row_ptr(img_y);
    for (int i = 0; i < n; i++) {
        const int px = x + i;
        if (!src) { dst[i] = s_ramp[0]; continue; }
        const uint8_t b = src[px >> 1];
        dst[i] = s_ramp[(px & 1) ? (b & 0x0F) : (b >> 4)];
    }
}

// Called once per frame, before the first strip of that frame is generated.
static IRAM_ATTR void frame_tick(void)
{
    const int64_t now = esp_timer_get_time();
    uint32_t dt = (uint32_t)(now - s_last_us);
    s_last_us = now;
    if (dt > 250000u) dt = 250000u;         // first frame, or after a long stall

    if (!s_paused && !s_hold) {
        // Multiply and shift rather than a 64-bit divide: this runs in the
        // interrupt that also has to generate the first scanlines of the frame.
        const int32_t step = (int32_t)(((int64_t)dt * (int64_t)s_step_k) >> 20);
        const int32_t acc = (int32_t)s_pos_frac + step;
        s_pos_row  += (acc >> 16);          // arithmetic shift floors, so reverse works
        s_pos_frac = (uint32_t)(acc & 0xFFFF);

        s_run_us_rem += dt;
        if (s_run_us_rem >= 1000u) {
            s_run_ms += s_run_us_rem / 1000u;
            s_run_us_rem %= 1000u;
        }

#if TP_STOP_AT_END
        if (s_speed_q16 > 0 && s_pos_row >= s_end_row) {
            s_pos_row = s_end_row;
            s_pos_frac = 0;
            s_paused = true;
            s_finished = true;
        } else if (s_speed_q16 < 0 && s_pos_row <= s_start_row) {
            s_pos_row = s_start_row;        // backing up past the top just stops
            s_pos_frac = 0;
            s_paused = true;
        }
#endif
    }

    s_row_base = s_pos_row;
    s_frames++;
}

static IRAM_ATTR void fill_strip(void *buf, int pos_px, int len_bytes)
{
    const int32_t base = s_row_base;
    uint16_t *dst = (uint16_t *)buf;
    int px = pos_px;
    int left = len_bytes >> 1;

    while (left > 0) {
        const int y = px / TP_LCD_H_RES;
        const int x = px - y * TP_LCD_H_RES;
        int n = TP_LCD_H_RES - x;
        if (n > left) n = left;

        if (n == TP_LCD_H_RES) fill_row_full((uint32_t *)dst, base + y);
        else                   fill_row_partial(dst, base + y, x, n);

        dst  += n;
        px   += n;
        left -= n;
    }
}

#if !TP_USE_FRAMEBUFFER
static IRAM_ATTR bool on_bounce_empty(esp_lcd_panel_handle_t panel, void *bounce_buf,
                                      int pos_px, int len_bytes, void *user_ctx)
{
    (void)panel; (void)user_ctx;
#if TP_RENDER_STATS
    const int64_t t0 = esp_timer_get_time();
#endif
    if (pos_px == 0) frame_tick();
    fill_strip(bounce_buf, pos_px, len_bytes);
#if TP_RENDER_STATS
    const uint32_t took = (uint32_t)(esp_timer_get_time() - t0);
    if (took > s_max_fill_us) s_max_fill_us = took;
#endif
    return false;
}
#else
static TaskHandle_t s_fb_waiter = nullptr;
static void *s_fb[2] = { nullptr, nullptr };
static int   s_fb_back = 0;

static IRAM_ATTR bool on_frame_done(esp_lcd_panel_handle_t panel,
                                    const esp_lcd_rgb_panel_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    BaseType_t hp = pdFALSE;
    if (s_fb_waiter) vTaskNotifyGiveFromISR(s_fb_waiter, &hp);
    return hp == pdTRUE;
}
#endif

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------

esp_err_t tp_display_init(void)
{
    tp_display_set_colors(TP_COLOR_FG_DEFAULT, TP_COLOR_BG_DEFAULT);
    tp_display_set_speed(TP_SPEED_DEFAULT);

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz           = TP_LCD_PCLK_HZ;
    cfg.timings.h_res             = TP_LCD_H_RES;
    cfg.timings.v_res             = TP_LCD_V_RES;
    cfg.timings.hsync_pulse_width = TP_LCD_HSYNC_PULSE;
    cfg.timings.hsync_back_porch  = TP_LCD_HSYNC_BACK;
    cfg.timings.hsync_front_porch = TP_LCD_HSYNC_FRONT;
    cfg.timings.vsync_pulse_width = TP_LCD_VSYNC_PULSE;
    cfg.timings.vsync_back_porch  = TP_LCD_VSYNC_BACK;
    cfg.timings.vsync_front_porch = TP_LCD_VSYNC_FRONT;
    cfg.timings.flags.pclk_active_neg = 1;

    cfg.data_width     = 16;
    cfg.bits_per_pixel = 16;

    cfg.hsync_gpio_num = TP_PIN_HSYNC;
    cfg.vsync_gpio_num = TP_PIN_VSYNC;
    cfg.de_gpio_num    = TP_PIN_DE;
    cfg.pclk_gpio_num  = TP_PIN_PCLK;
    cfg.disp_gpio_num  = TP_PIN_DISP;

    const int data_pins[16] = TP_RGB_DATA_PINS;
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_pins[i];

    cfg.bounce_buffer_size_px = TP_LCD_H_RES * TP_BOUNCE_LINES;

#if TP_USE_FRAMEBUFFER
    cfg.num_fbs = 2;
    cfg.flags.fb_in_psram = 1;
#else
    cfg.num_fbs = 0;            // must be 0 when no_fb is set
    cfg.flags.no_fb = 1;        // we fill the bounce buffers ourselves
#endif

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG, "esp_lcd_new_rgb_panel failed");

    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
#if TP_USE_FRAMEBUFFER
    cbs.on_frame_buf_complete = on_frame_done;
#else
    cbs.on_bounce_empty = on_bounce_empty;
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr),
                        TAG, "callback registration failed");

    s_last_us = esp_timer_get_time();

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");

#if TP_USE_FRAMEBUFFER
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &s_fb[0], &s_fb[1]),
                        TAG, "could not get frame buffers");
    ESP_LOGW(TAG, "running the PSRAM frame-buffer fallback path");
#else
    ESP_LOGI(TAG, "scanline renderer active: no frame buffer, %d-line bounce buffers",
             TP_BOUNCE_LINES);
#endif
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

void tp_display_set_speed(float px_per_sec)
{
    s_speed_q16 = (int32_t)(px_per_sec * 65536.0f);
    // 65536 / 1e6 * 2^20 = 68719.48
    s_step_k = (int32_t)(px_per_sec * 68719.48f);
}

#if TP_RENDER_STATS
uint32_t tp_display_take_max_fill_us(void)
{
    const uint32_t v = s_max_fill_us;
    s_max_fill_us = 0;
    return v;
}
#endif

void tp_display_set_paused(bool paused) { s_paused = paused; }
bool tp_display_paused(void)            { return s_paused; }
void tp_display_set_hold(bool hold)     { s_hold = hold; }

void tp_display_set_range(int32_t start_row, int32_t end_row)
{
    if (end_row < start_row) end_row = start_row;
    s_start_row = start_row;
    s_end_row = end_row;
}

int32_t tp_display_start_row(void) { return s_start_row; }
int32_t tp_display_end_row(void)   { return s_end_row; }
bool    tp_display_finished(void)  { return s_finished; }

void tp_display_rewind(void)
{
    s_pos_row = s_start_row;
    s_pos_frac = 0;
    s_row_base = s_start_row;
    s_run_ms = 0;
    s_run_us_rem = 0;
    s_finished = false;
}

uint32_t tp_display_run_ms(void)   { return s_run_ms; }
int32_t  tp_display_view_row(void) { return s_row_base; }
uint32_t tp_display_frames(void)   { return s_frames; }

#if TP_USE_FRAMEBUFFER
void tp_display_render_frame_blocking(void)
{
    s_fb_waiter = xTaskGetCurrentTaskHandle();

    frame_tick();

    uint32_t *fb = (uint32_t *)s_fb[s_fb_back];
    const int32_t base = s_row_base;
    for (int y = 0; y < TP_LCD_V_RES; y++) {
        fill_row_full(fb + (size_t)y * TP_MASK_STRIDE, base + y);
    }

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, TP_LCD_H_RES, TP_LCD_V_RES, fb);
    s_fb_back ^= 1;

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
}
#endif
