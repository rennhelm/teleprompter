// tp_battery.h - single-cell LiPo monitoring for the teleprompter remote.
//
// Arduino.h first, deliberately: ESP_LOGx only produces serial output in
// translation units that pull in esp32-hal-log.h via Arduino.h.
// See hardware-gotchas.md, "ESP_LOGI/ESP_LOGW output is silently suppressed".
#pragma once

#include <Arduino.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Tunables. Override any of these in tp_config.h / build_flags if you prefer.
// ---------------------------------------------------------------------------

// ADC1 only. ADC2 shares hardware with the Wi-Fi radio and reads fail whenever
// the radio is active, which on this remote is always. ADC1 is GPIO1-10.
// GPIO4/5/6 are the buttons, so 7 is the first free one.
#ifndef TP_BAT_ADC_PIN
#define TP_BAT_ADC_PIN 7
#endif

// Divider ratio: 100k top + 100k bottom = 2.0. Change if you use other values,
// e.g. 100k/220k would be (100+220)/220 = 1.4545.
#ifndef TP_BAT_DIVIDER
#define TP_BAT_DIVIDER 2.0f
#endif

// Trim factor. Leave at 1.0, then correct once against a multimeter:
// TP_BAT_CAL = actual_battery_mv / reported_mv.
#ifndef TP_BAT_CAL
#define TP_BAT_CAL 1.0f
#endif

#ifndef TP_BAT_SAMPLE_MS
#define TP_BAT_SAMPLE_MS 1000
#endif

// Samples averaged per pass. Wi-Fi transmit bursts pull the rail around, so a
// single read is noisy by several tens of mV.
#ifndef TP_BAT_OVERSAMPLE
#define TP_BAT_OVERSAMPLE 16
#endif

// Exponential smoothing, 0..1. Lower is smoother and slower to react.
#ifndef TP_BAT_EMA_ALPHA
#define TP_BAT_EMA_ALPHA 0.25f
#endif

// Warning thresholds, in percent.
#ifndef TP_BAT_LOW_PCT
#define TP_BAT_LOW_PCT 15
#endif

#ifndef TP_BAT_CRIT_PCT
#define TP_BAT_CRIT_PCT 5
#endif

// Hysteresis so the flags do not chatter at the boundary.
#ifndef TP_BAT_HYST_PCT
#define TP_BAT_HYST_PCT 3
#endif

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Configure the ADC and prime the filter. Call once from setup().
void tp_battery_init();

// Cheap and self rate-limiting. Call every pass of loop().
void tp_battery_update();

// Smoothed pack voltage, millivolts. 0 until the first sample lands.
uint16_t tp_battery_mv();

// 0-100, from a LiPo discharge curve rather than a straight voltage ramp.
uint8_t tp_battery_pct();

// True once the first averaged sample is in. Everything else reads as full
// until then, so nothing flashes a false warning during boot.
bool tp_battery_valid();

bool tp_battery_low();
bool tp_battery_critical();

// LED overlay. Call this first in whatever paints the pixel: if it returns
// true, show *rgb this instant and skip the normal state color.
//
//   critical: red, 2 Hz blink, unmistakable
//   low:      two short red blinks every 4 s, then back to the state color
//
// Deliberately distinct from the existing patterns. Solid red already means
// "no link" and a steady blink already means "more than 3 s off target".
bool tp_battery_led_override(uint32_t *rgb);
