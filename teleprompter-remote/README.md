# Teleprompter remote

ESP32-S3-N16R8 DevKitC, three buttons. Separate PlatformIO project from the
display; flash it to the second board.

## Wiring

Every button is two wires: pin to button, button to GND. Internal pull-ups are
enabled in firmware, so no resistors. Pin numbers live in `src/ctl_config.h`;
if you rewire, change them there and update this table.

| Signal | Pin |
| --- | --- |
| Faster | GPIO7 to GND |
| Slower | GPIO5 to GND |
| Play / pause | GPIO4 to GND |
| LED | GPIO48 (onboard addressable LED on most S3 DevKitC boards) |

Pins avoided on purpose: 0, 45, 46 (strapping), 19, 20 (USB), 26-32 (flash) and
33-37 (the octal PSRAM on an N16R8 part). Nothing here uses the ADC, so the
ADC1-only restriction that an analog control would have imposed does not apply.

## Controls

| Control | Action |
| --- | --- |
| Faster | +5% speed, repeats while held |
| Slower | -5% speed, repeats while held |
| Play | play / pause |
| Play, long (0.8 s) | back to top |
| Faster + Slower together | re-pace: recompute speed to land on the target time |

**Steps are relative, not a fixed px/s.** A "+5 px/s" step would be a 22% change
on a script paced at 23 px/s and a 2% change at 200 px/s, so it would feel
completely different depending on the script. A percentage feels the same
everywhere.

**The change sticks.** Buttons have no spring return, so a speed you press in
stays until you press again. That means the paced speed does drift away from the
target as you use it, which is what the display's drift readout is for, and what
the re-pace chord is for: it recomputes the speed from the distance left over
the time left, putting you back on schedule from wherever you are.

Holding a speed button repeats about six times a second after a half second
delay, so roughly a second of holding is a 1.34x change. Fine adjustment by
tapping, big changes by holding.

## LED

| Colour | Meaning |
| --- | --- |
| Red | no link to the display |
| Amber | paused |
| Green | playing |
| Blue | run finished |
| White flash | command sent |
| Blinking | more than 3 s off the target time |

The blink is the useful one: it tells you to lean on the slower or faster button
without having to look away from the script.

## Setup notes

- `CTL_CHANNEL` must match `TP_AP_CHANNEL` in the display's `tp_config.h`. Both
  are 1. ESP-NOW needs both ends on the same channel and there is no
  negotiation.
- `src/tp_link.h` is a **copy of the display's file**. If you change the
  protocol, copy it across again rather than editing one side.
- Re-pace only does anything when a target length is set on the display. Without
  one there is no schedule to get back onto.

## Tuning the feel

All in `ctl_config.h`:

- `CTL_STEP` (0.05): how much one press moves the speed.
- `CTL_REPEAT_DELAY_MS` (500) and `CTL_REPEAT_MS` (150): how quickly holding a
  button starts and then continues repeating.
- `CTL_LONG_PRESS_MS` (800): how long a hold on Play means back-to-top.

## No link?

The remote prints its MAC and channel at boot, and the display logs
`ESP-NOW listening on channel 1` plus the remote's MAC the first time it hears
from it. If the display never logs the remote, the channel is the first thing to
check. The remote pings once a second when idle, so the display always knows
where to send its status replies even if it reboots mid-session.
