# Teleprompter — Remote controller (Phase 6, 2026-09-15)

Second board: ESP32-S3-N16R8 DevKitC. **Three buttons**: faster, slower,
play/pause. Separate PlatformIO project in `teleprompter-remote/` (earlier planning
notes called it `controller/`).

A joystick version was built first and then dropped in favour of buttons. The
reasoning behind both is kept below, because the shape of the control determines
the command it should send and that is not obvious.

## The control shape decides the command

The protocol carries two ways to move the speed. Which one is correct depends
entirely on whether the control springs back by itself.

**Buttons → `TP_CMD_SPEED_NUDGE`** (what is built). Scales the base speed by a
relative amount, and the change **sticks**:

```
base *= (1 + value / 1000)
```

Relative, not absolute, on purpose: a fixed "+5 px/s" step is a 22% change at
23 px/s and a 2% change at 200 px/s, so it would feel completely different
depending on the script length. A percentage feels the same everywhere.

Because the change sticks, the paced speed does drift away from the target as
you use it. That is what the drift readout is for, and what re-pace is for.

**Spring-centred controls (joystick, slider) → `TP_CMD_SPEED_TRIM`** (in the
protocol, unused by this remote). Biases the base speed rather than replacing
it, `effective = base * (1 + trim)`, so letting go returns the scroll to exactly
the paced speed and the target can never be lost. A momentary trim also needs
the display-side failsafe that clears it if the remote goes quiet, so a flat
battery mid-deflection cannot leave the scroll stuck fast. That failsafe only
arms when a non-zero trim arrives, so the button remote never triggers it.

Base speed and trim stay separate variables on the display (`s_speed`, `s_trim`)
composed in one place (`apply_speed()` in `main.cpp`), so the web slider, the
duration target and the remote never fight over one value. `tp_app_repace()`
zeroes the trim, because "put me back on schedule" means from neutral.

## Control layout

| Control | Action |
| --- | --- |
| Faster (GPIO7) | +5%, repeats while held |
| Slower (GPIO5) | -5%, repeats while held |
| Play (GPIO4) | play / pause |
| Play, long 0.8 s | back to top |
| Faster + Slower together | re-pace |

The chord saves a fourth button for what is genuinely the most useful mid-take
control. One stray nudge fires before the chord is detected; harmless, because
re-pace recomputes the speed outright. Both buttons are then suppressed until
released so the chord does not ripple the speed.

All three buttons are pin-to-GND with internal pull-ups. **Nothing uses the ADC,
so the ADC1-only constraint below does not apply to the built remote.**

LED (onboard addressable, GPIO48): red = no link, amber = paused, green =
playing, blue = finished, white flash = command sent, blinking = more than 3 s
off the target. The blink is the useful one; it prompts a correction without
looking away from the script.

## Kept for reference: the ADC gotcha, if an analog control is ever added

An analog joystick or slider **must** be on ADC1, GPIO1-10. ADC2 shares hardware
with the Wi-Fi radio and reads on it fail whenever the radio is active, which on
this remote is always. It presents as a broken control, not as an error. Also:
VCC to 3V3, not 5V, or the ADC clips; and calibrate centre at boot by averaging,
because no real thumbstick sits at 2048.

Pins avoided throughout: 0/45/46 (strapping), 19/20 (USB), 26-32 (flash), 33-37
(octal PSRAM on an N16R8 part).

## Link details

- `CTL_CHANNEL` must equal `TP_AP_CHANNEL` (both 1). ESP-NOW has no channel
  negotiation, which is why the display's soft AP is pinned.
- The remote broadcasts; the display replies to whoever sent the command. The
  remote pings once a second when idle so the display always knows where to
  send status, even if the display reboots mid-session.
- `teleprompter-remote/src/tp_link.h` is a **copy** of the display's. Change one, copy it
  across; never edit one side only.
- Received commands are queued and applied on the display's own task, not in the
  Wi-Fi callback, because a rewind re-rasterises and would stall the Wi-Fi stack
  for a couple of hundred milliseconds.

## Also changed on the display side

- `tp_app_nudge_speed(factor)` added, floored at `TP_PACE_SPEED_MIN` rather than
  the slider's minimum, so a legitimately slow paced speed does not jump when
  nudged.
- `tp_app_set_speed()` and `tp_app_set_brightness()` no longer write NVS
  immediately; a repeating button would hammer flash. Flushed from `loop()` once
  settled.
- Status frames report the **effective** speed and compute drift from it.
- Web page shows "remote trim +25%" when a trim is applied, and uses the
  effective speed for its projections.

## Verification status

The button state machine was simulated on the host across five scenarios: a tap
gives exactly one step; a 1.2 s hold gives six steps (1.34x, matching the
documented figure); the chord gives one stray nudge then re-pace and nothing
further until release; a play tap gives only a toggle; a long play hold gives
only a rewind and no toggle on release. ESP-NOW callback signatures were checked
against `esp_now.h` for IDF 5.5 (note `esp_now_send_cb_t` changed shape in 5.4,
so no send callback is registered).

**Neither the remote firmware nor the display's nudge path has run on
hardware.** First check: the remote prints its MAC and channel at boot, and the
display logs the remote's MAC the first time it hears from it. If that line
never appears, check the channel.

> Note: this file is the design record. For current wiring and day-to-day use,
> `teleprompter-remote/README.md` is the operational reference, and
> `teleprompter-remote/src/ctl_config.h` is the source of truth for pins.
