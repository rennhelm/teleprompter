// tp_app.h - the control surface the web UI talks to. Implemented in main.cpp.
#pragma once

#include <Arduino.h>

struct tp_status_t {
    float    speed;             // base speed, before the remote's trim
    float    trim;              // -1..+1 bias applied on top
    bool     paused;
    bool     mirror;
    bool     finished;
    int      brightness;
    int      lines;
    int      words;
    int      missing_glyphs;
    int      duration_s;        // target run length; 0 = fixed speed
    uint32_t elapsed_ms;        // time spent actually scrolling this run
    int32_t  start_row;
    int32_t  end_row;
    int32_t  loop_rows;
    int32_t  view_row;
    uint32_t fps;
    uint32_t free_heap;
    uint32_t free_psram;
};

void tp_app_set_script(const String &text);
void tp_app_set_speed(float px_per_sec);
void tp_app_set_trim(float trim);        // -1..+1, momentary bias from a remote
void tp_app_nudge_speed(float factor);   // scale the base speed, e.g. 1.05
void tp_app_set_paused(bool paused);
void tp_app_set_mirror(bool mirror);
void tp_app_set_brightness(int percent);
void tp_app_set_duration(int seconds);   // 0 turns the target off
void tp_app_repace(void);                // re-derive speed from what is left
void tp_app_rewind(void);

const String &tp_app_script(void);
void tp_app_status(tp_status_t *out);
