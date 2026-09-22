// Teleprompter for the Waveshare ESP32-S3 7" RGB panel.
//  Original teleprompter design and circuit made by Renn Helm. Code written by Claude Code.
//
// How it stays smooth
// -------------------
// There is no frame buffer and no graphics library in the hot path. The script
// is rasterised once into a rolling 4-bits-per-pixel alpha mask in PSRAM, and
// the RGB peripheral's bounce-buffer callback expands the scanlines it is about
// to shift out straight from that mask through a 256-entry lookup table. The
// work per frame is therefore constant, PSRAM sees almost no traffic, and the
// scroll position advances from the panel's own frame timing, so nothing the
// CPU is busy with - Wi-Fi, HTTP, flash, rasterising the next line - can make
// the text hitch.

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "tp_app.h"
#include "tp_board.h"
#include "tp_config.h"
#include "tp_display.h"
#include "tp_text.h"
#include "tp_web.h"

static const char *TAG = "teleprompter";

static const char *DEFAULT_SCRIPT =
    "Connect to the Teleprompter Wi-Fi network and open 192.168.4.1 to load "
    "your own script, set the scroll speed, and mirror the text for a "
    "beam-splitter.";

static Preferences s_prefs;

static String s_script;
static float  s_speed      = TP_SPEED_DEFAULT;
static bool   s_mirror     = false;
static int    s_brightness = TP_BRIGHTNESS_DEFAULT;
static int    s_duration_s = TP_DURATION_DEFAULT_S;   // 0 = fixed speed, no target
static float  s_trim       = 0.0f;                   // momentary bias from the remote

static uint32_t s_fps = 0;
static bool     s_prefs_dirty = false;   // speed/brightness settle before hitting flash

// ---------------------------------------------------------------------------
// Pacing
//
// A run is "first line on the reading row" to "last line on the reading row",
// so the distance covered is the height of the script less one line - and it
// does not depend on where the reading row sits. Setting a target length is
// therefore just a division, and it is re-derived whenever the script changes.
// ---------------------------------------------------------------------------

static int32_t run_travel_rows(void)
{
    const int32_t travel = tp_text_rows() - tp_text_line_height();
    return travel > 1 ? travel : 1;
}

// Everything that changes speed goes through here, so the base speed and the
// remote's momentary trim compose instead of fighting over one variable.
static void apply_speed(void)
{
    float v = s_speed * (1.0f + s_trim);
    if (v < TP_PACE_SPEED_MIN) v = TP_PACE_SPEED_MIN;
    if (v > TP_SPEED_MAX) v = TP_SPEED_MAX;
    tp_display_set_speed(v);
}

static void apply_pacing(void)
{
    const int32_t start = -(int32_t)TP_READ_LINE_Y;
    tp_display_set_range(start, start + run_travel_rows());

    if (s_duration_s > 0) {
        float v = (float)run_travel_rows() / (float)s_duration_s;
        if (v < TP_PACE_SPEED_MIN) v = TP_PACE_SPEED_MIN;
        if (v > TP_SPEED_MAX) v = TP_SPEED_MAX;
        s_speed = v;
        ESP_LOGI(TAG, "target %ds over %d rows -> %.1f px/s (%d words, %.0f wpm)",
                 s_duration_s, (int)run_travel_rows(), s_speed,
                 tp_text_word_count(),
                 tp_text_word_count() * 60.0f / (float)s_duration_s);
    }
    apply_speed();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static String load_script_file(void)
{
    if (LittleFS.exists("/script.txt")) {
        File f = LittleFS.open("/script.txt", "r");
        if (f) {
            String text = f.readString();
            f.close();
            if (text.length()) {
                ESP_LOGI(TAG, "loaded script.txt (%u bytes)", (unsigned)text.length());
                return text;
            }
        }
    }
    ESP_LOGI(TAG, "no script.txt, using the placeholder text");
    return String(DEFAULT_SCRIPT);
}

static bool save_script_file(const String &text)
{
    File f = LittleFS.open("/script.txt", "w");
    if (!f) {
        ESP_LOGE(TAG, "could not open script.txt for writing");
        return false;
    }
    const size_t written = f.print(text);
    f.close();
    if (written != text.length()) {
        ESP_LOGE(TAG, "short write to script.txt (%u of %u)",
                 (unsigned)written, (unsigned)text.length());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Re-layout. The display is held blank across this because the mask is being
// rewritten underneath the renderer, and because it covers the flash write.
// ---------------------------------------------------------------------------

static void reload(bool save_to_flash)
{
    tp_display_set_hold(true);
    vTaskDelay(pdMS_TO_TICKS(60));          // let any in-flight scanline retire

    if (save_to_flash) save_script_file(s_script);

    // Rewind first: the render task is still topping the mask up against
    // whatever row the display last reported, and we do not want it chasing a
    // target tens of thousands of rows ahead of a mask we just emptied.
    tp_display_rewind();
    tp_text_set_script(s_script.c_str(), s_script.length(), s_mirror);

    apply_pacing();              // new script length, so re-derive the speed
    tp_display_rewind();         // park on the reading row, run clock at zero
    tp_text_ensure(tp_display_start_row() + TP_LCD_V_RES + TP_LEAD_ROWS, 0);

    tp_display_set_paused(TP_START_PAUSED != 0);
    tp_display_set_hold(false);
}

// ---------------------------------------------------------------------------
// Control surface used by the web UI
// ---------------------------------------------------------------------------

void tp_app_set_script(const String &text)
{
    s_script = text.length() ? text : String(DEFAULT_SCRIPT);
    reload(true);
}

void tp_app_set_speed(float px_per_sec)
{
    if (px_per_sec < TP_SPEED_MIN) px_per_sec = TP_SPEED_MIN;
    if (px_per_sec > TP_SPEED_MAX) px_per_sec = TP_SPEED_MAX;
    s_speed = px_per_sec;
    apply_speed();
    // Deliberately not written straight to NVS: a rotary encoder can produce
    // dozens of these a second, and flash does not enjoy that. loop() flushes.
    s_prefs_dirty = true;
}

// Momentary, and never persisted: a spring-centred control returns to zero and
// the scroll returns to exactly the paced speed.
void tp_app_set_trim(float trim)
{
    if (trim < -TP_TRIM_MAX) trim = -TP_TRIM_MAX;
    if (trim > TP_TRIM_MAX) trim = TP_TRIM_MAX;
    s_trim = trim;
    apply_speed();
}

// Relative, because a fixed px/s step feels completely different at 23 px/s
// than at 200. Floored at the pacing minimum rather than the slider's minimum,
// so a legitimately slow paced speed does not jump when you nudge it.
void tp_app_nudge_speed(float factor)
{
    float v = s_speed * factor;
    if (v < TP_PACE_SPEED_MIN) v = TP_PACE_SPEED_MIN;
    if (v > TP_SPEED_MAX) v = TP_SPEED_MAX;
    s_speed = v;
    apply_speed();
    s_prefs_dirty = true;
}

void tp_app_set_paused(bool paused)
{
    // Pressing play on a finished run means "go again", not "sit at the end".
    if (!paused && tp_display_finished()) tp_app_rewind();
    tp_display_set_paused(paused);
}

void tp_app_set_duration(int seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds > 4 * 60 * 60) seconds = 4 * 60 * 60;
    s_duration_s = seconds;
    s_prefs.putInt("duration", s_duration_s);
    apply_pacing();
}

// Re-derive the speed from what is actually left, against what is actually
// left of the target. Use mid-run after nudging the speed around.
void tp_app_repace(void)
{
    if (s_duration_s <= 0) return;

    const int32_t remaining_rows = tp_display_end_row() - tp_display_view_row();
    const float remaining_s = (float)s_duration_s - tp_display_run_ms() / 1000.0f;
    if (remaining_rows <= 0 || remaining_s < 1.0f) return;

    float v = (float)remaining_rows / remaining_s;
    if (v < TP_PACE_SPEED_MIN) v = TP_PACE_SPEED_MIN;
    if (v > TP_SPEED_MAX) v = TP_SPEED_MAX;
    s_speed = v;
    s_trim = 0.0f;              // back on schedule means back to neutral
    apply_speed();
    ESP_LOGI(TAG, "re-paced: %d rows in %.1fs -> %.1f px/s",
             (int)remaining_rows, remaining_s, s_speed);
}

void tp_app_set_mirror(bool mirror)
{
    if (mirror == s_mirror) return;
    s_mirror = mirror;
    s_prefs.putBool("mirror", s_mirror);
    reload(false);
}

void tp_app_set_brightness(int percent)
{
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    tp_board_set_brightness((uint8_t)percent);
    s_prefs_dirty = true;
}

void tp_app_rewind(void)
{
    tp_display_set_hold(true);
    vTaskDelay(pdMS_TO_TICKS(40));
    tp_display_rewind();
    tp_text_rewind();
    tp_text_ensure(tp_display_start_row() + TP_LCD_V_RES + TP_LEAD_ROWS, 0);
    tp_display_set_paused(true);      // a rewind means "getting ready for a take"
    tp_display_set_hold(false);
}

const String &tp_app_script(void) { return s_script; }

void tp_app_status(tp_status_t *out)
{
    out->speed          = s_speed;
    out->trim           = s_trim;
    out->paused         = tp_display_paused();
    out->mirror         = s_mirror;
    out->brightness     = s_brightness;
    out->lines          = tp_text_line_count();
    out->words          = tp_text_word_count();
    out->missing_glyphs = tp_text_missing_glyphs();
    out->duration_s     = s_duration_s;
    out->elapsed_ms     = tp_display_run_ms();
    out->finished       = tp_display_finished();
    out->start_row      = tp_display_start_row();
    out->end_row        = tp_display_end_row();
    out->loop_rows      = tp_text_loop_rows();
    out->view_row       = tp_display_view_row();
    out->fps            = s_fps;
    out->free_heap      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->free_psram     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

// ---------------------------------------------------------------------------
// Render task: keeps the mask filled ahead of the renderer. In the default
// scanline mode this is all it does - the picture is produced by the panel
// interrupt - so it can stall for a whole second without the scroll noticing.
// ---------------------------------------------------------------------------

static void render_task(void *)
{
    for (;;) {
#if TP_USE_FRAMEBUFFER
        tp_display_render_frame_blocking();
        tp_text_ensure(tp_display_view_row() + TP_LCD_V_RES + TP_LEAD_ROWS,
                       TP_RASTER_BUDGET_ROWS);
#else
        tp_text_ensure(tp_display_view_row() + TP_LCD_V_RES + TP_LEAD_ROWS,
                       TP_RASTER_BUDGET_ROWS);
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
    }
}

// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    esp_log_level_set("*", ESP_LOG_INFO);

    if (!tp_board_init()) {
        ESP_LOGE(TAG, "board init failed - no backlight control, continuing anyway");
    }

    if (!LittleFS.begin(true)) {
        ESP_LOGW(TAG, "LittleFS mount failed even after formatting");
    }
    s_prefs.begin("teleprompt", false);
    s_speed      = s_prefs.getFloat("speed", TP_SPEED_DEFAULT);
    s_mirror     = s_prefs.getBool("mirror", false);
    s_brightness = s_prefs.getInt("bright", TP_BRIGHTNESS_DEFAULT);
    s_duration_s = s_prefs.getInt("duration", TP_DURATION_DEFAULT_S);

    if (!tp_text_init()) {
        ESP_LOGE(TAG, "text engine init failed - halting");
        for (;;) delay(1000);
    }

    s_script = load_script_file();
    tp_text_set_script(s_script.c_str(), s_script.length(), s_mirror);

    // Fill the mask before the panel starts scanning, so the very first frame
    // out of the peripheral already has text in it.
    tp_text_ensure(TP_LCD_V_RES + TP_LEAD_ROWS, 0);

    if (tp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed - halting");
        for (;;) delay(1000);
    }

    apply_pacing();                         // run extent, and speed if targeted
    tp_display_rewind();                    // park with line one on the reading row
    tp_display_set_paused(TP_START_PAUSED != 0);

    delay(120);                             // a few good frames before the light
    tp_board_set_brightness((uint8_t)s_brightness);
    tp_board_backlight(true);

    xTaskCreatePinnedToCore(render_task, "tp_render", 4096, nullptr, 4, nullptr, 1);
    tp_web_start();

    if (s_duration_s > 0) {
        ESP_LOGI(TAG, "ready (paused): %d lines, %d words, target %d:%02d at %.1f px/s",
                 tp_text_line_count(), tp_text_word_count(),
                 s_duration_s / 60, s_duration_s % 60, s_speed);
    } else {
        ESP_LOGI(TAG, "ready (paused): %d lines, %d words, %.0f px/s (no target length)",
                 tp_text_line_count(), tp_text_word_count(), s_speed);
    }
}

void loop()
{
    static uint32_t last_frames = 0;
    delay(5000);

    const uint32_t frames = tp_display_frames();
    s_fps = (frames - last_frames) / 5;
    last_frames = frames;

    // Flush settings that the encoder or a slider may have been changing
    // rapidly. NVS only sees a write once the value has settled.
    if (s_prefs_dirty) {
        s_prefs_dirty = false;
        s_prefs.putFloat("speed", s_speed);
        s_prefs.putInt("bright", s_brightness);
    }

#if TP_RENDER_STATS
    ESP_LOGI(TAG, "%u fps | row %d | fill %u/%u us | heap %u | psram %u",
             (unsigned)s_fps, (int)tp_display_view_row(),
             (unsigned)tp_display_take_max_fill_us(), (unsigned)TP_BOUNCE_PERIOD_US,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    ESP_LOGI(TAG, "%u fps | row %d | heap %u | psram %u",
             (unsigned)s_fps, (int)tp_display_view_row(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
}
