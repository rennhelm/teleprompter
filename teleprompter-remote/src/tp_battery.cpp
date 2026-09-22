// tp_battery.cpp - see tp_battery.h for wiring and tunables.
//
// Arduino.h first, deliberately (see the header for why).
#include <Arduino.h>

#include "tp_battery.h"

static const char *TAG = "tp_battery";

// Discharge curve for a single-cell LiPo under a light load (~100 mA).
// Voltage alone is a poor charge gauge because the curve is flat through the
// middle, so interpolate a real curve rather than mapping 3.3-4.2 V linearly.
static const uint16_t kCurveMv[] = {
    3300, 3500, 3600, 3680, 3740, 3770, 3790, 3820, 3870, 3930, 4000, 4100, 4200};
static const uint8_t kCurvePct[] = {
    0,    5,    10,   15,   20,   30,   40,   50,   60,   70,   80,   90,   100};
static const size_t kCurveLen = sizeof(kCurvePct) / sizeof(kCurvePct[0]);

static float s_ema_mv = 0.0f;
static uint16_t s_mv = 0;
static uint8_t s_pct = 100;
static bool s_valid = false;
static bool s_low = false;
static bool s_crit = false;
static uint32_t s_next_sample_ms = 0;

static uint8_t curve_pct(uint16_t mv) {
  if (mv <= kCurveMv[0]) return 0;
  if (mv >= kCurveMv[kCurveLen - 1]) return 100;

  for (size_t i = 1; i < kCurveLen; i++) {
    if (mv < kCurveMv[i]) {
      const uint16_t lo_mv = kCurveMv[i - 1];
      const uint16_t hi_mv = kCurveMv[i];
      const uint8_t lo_pct = kCurvePct[i - 1];
      const uint8_t hi_pct = kCurvePct[i];
      const float t = (float)(mv - lo_mv) / (float)(hi_mv - lo_mv);
      return (uint8_t)(lo_pct + t * (hi_pct - lo_pct) + 0.5f);
    }
  }
  return 100;
}

// One averaged reading of the pack, in millivolts at the battery.
static uint16_t read_pack_mv() {
  uint32_t acc = 0;
  for (int i = 0; i < TP_BAT_OVERSAMPLE; i++) {
    // analogReadMilliVolts applies the chip's factory eFuse ADC calibration.
    // Raw analogRead plus a hand-rolled scale factor is off by 5-10% on most
    // ESP32-S3 parts, which is the whole usable span of a LiPo curve.
    acc += analogReadMilliVolts(TP_BAT_ADC_PIN);
  }
  const float pin_mv = (float)acc / (float)TP_BAT_OVERSAMPLE;
  return (uint16_t)(pin_mv * TP_BAT_DIVIDER * TP_BAT_CAL + 0.5f);
}

void tp_battery_init() {
  analogReadResolution(12);
  // 11 dB attenuation gives a usable range to roughly 3.1 V at the pin. With a
  // 2:1 divider a full 4.2 V cell presents 2.1 V, comfortably inside it.
  // If your core version rejects ADC_11db, it wants ADC_12db instead.
  analogSetPinAttenuation(TP_BAT_ADC_PIN, ADC_11db);

  // Prime the filter so the first real update does not ramp up from zero.
  const uint16_t mv = read_pack_mv();
  s_ema_mv = (float)mv;
  s_mv = mv;
  s_pct = curve_pct(mv);
  s_valid = true;
  s_low = (s_pct <= TP_BAT_LOW_PCT);
  s_crit = (s_pct <= TP_BAT_CRIT_PCT);
  s_next_sample_ms = millis() + TP_BAT_SAMPLE_MS;

  ESP_LOGI(TAG, "battery monitor on GPIO%d: %u mV, %u%%", TP_BAT_ADC_PIN, mv, s_pct);
}

void tp_battery_update() {
  const uint32_t now = millis();
  if ((int32_t)(now - s_next_sample_ms) < 0) return;
  s_next_sample_ms = now + TP_BAT_SAMPLE_MS;

  const uint16_t mv = read_pack_mv();
  s_ema_mv += TP_BAT_EMA_ALPHA * ((float)mv - s_ema_mv);
  s_mv = (uint16_t)(s_ema_mv + 0.5f);
  s_pct = curve_pct(s_mv);

  // Hysteresis: trip at the threshold, clear only once well above it.
  if (s_low) {
    if (s_pct > TP_BAT_LOW_PCT + TP_BAT_HYST_PCT) s_low = false;
  } else if (s_pct <= TP_BAT_LOW_PCT) {
    s_low = true;
    ESP_LOGW(TAG, "battery low: %u mV, %u%%", s_mv, s_pct);
  }

  if (s_crit) {
    if (s_pct > TP_BAT_CRIT_PCT + TP_BAT_HYST_PCT) s_crit = false;
  } else if (s_pct <= TP_BAT_CRIT_PCT) {
    s_crit = true;
    ESP_LOGW(TAG, "battery critical: %u mV, %u%%", s_mv, s_pct);
  }
}

uint16_t tp_battery_mv() { return s_mv; }
uint8_t tp_battery_pct() { return s_valid ? s_pct : 100; }
bool tp_battery_valid() { return s_valid; }
bool tp_battery_low() { return s_valid && s_low; }
bool tp_battery_critical() { return s_valid && s_crit; }

bool tp_battery_led_override(uint32_t *rgb) {
  if (!s_valid) return false;

  if (s_crit) {
    // 2 Hz: on for the first half of each 500 ms window.
    if ((millis() % 500) < 250) {
      *rgb = 0xFF0000;
      return true;
    }
    // Off phase still counts as an override, otherwise the state color shows
    // through and it reads as a flicker rather than a blink.
    *rgb = 0x000000;
    return true;
  }

  if (s_low) {
    // Two short blinks at the top of every 4 s window, then hand the pixel
    // back so the normal play/pause color is still readable.
    const uint32_t phase = millis() % 4000;
    if (phase < 120 || (phase >= 240 && phase < 360)) {
      *rgb = 0xFF0000;
      return true;
    }
    if (phase < 360) {
      *rgb = 0x000000;
      return true;
    }
  }

  return false;
}
