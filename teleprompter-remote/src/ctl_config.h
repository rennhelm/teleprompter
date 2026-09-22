// ctl_config.h - everything you might rewire or retune on the remote.
#pragma once

// ---------------------------------------------------------------------------
// Pins (ESP32-S3-N16R8 DevKitC)
//
// All three buttons wire to GND with the internal pull-up enabled, so each one
// is two wires and nothing else. No ADC involved, so none of the ADC1-only
// restriction that a joystick would have imposed.
//
// Avoided: 0/45/46 (strapping), 19/20 (USB), 26-32 (flash) and 33-37 (the
// octal PSRAM on an N16R8 part).
// ---------------------------------------------------------------------------
#define CTL_PIN_FASTER          7
#define CTL_PIN_SLOWER          5
#define CTL_PIN_PLAY            4 

// Onboard addressable LED on most S3 DevKitC boards. Set to -1 if yours has
// none, or change the pin if it differs.
#define CTL_PIN_LED             48

// ---------------------------------------------------------------------------
// Link. Must match the display's tp_config.h.
// ---------------------------------------------------------------------------
#define CTL_CHANNEL             1

// ---------------------------------------------------------------------------
// Speed steps
// ---------------------------------------------------------------------------
// Per press, as a fraction. Relative rather than a fixed px/s, because a
// "+5 px/s" step would be a 22% change at 23 px/s and a 2% change at 200 px/s.
// A percentage feels the same whatever the script length.
#define CTL_STEP                0.05f

// Hold a speed button to repeat. Roughly 6 steps a second once it gets going,
// so about a second of holding is a 1.34x change.
#define CTL_REPEAT_DELAY_MS     500
#define CTL_REPEAT_MS           150

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define CTL_POLL_MS             15
#define CTL_DEBOUNCE_MS         25
#define CTL_LONG_PRESS_MS       800     // play button long press = back to top
#define CTL_HEARTBEAT_MS        1000    // so the display knows where to reply
#define CTL_LINK_LOST_MS        2500    // no status for this long = show red
