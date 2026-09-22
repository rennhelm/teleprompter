// tp_link.h - ESP-NOW control link.
//
// This header is the protocol, and it is meant to be shared verbatim with the
// controller sketch: copy this one file into the remote's project and both ends
// agree by construction.
//
// The remote broadcasts commands (no pairing step, no MAC to configure). The
// display answers each command with a status frame sent back to whoever sent
// it, so a remote with a screen or a couple of LEDs can show what is actually
// happening without polling.
//
// Both ends must be on the same Wi-Fi channel. The display pins its soft AP to
// TP_AP_CHANNEL, so the remote just sets the same channel before esp_now_init().
#pragma once

#include <stdint.h>

#define TP_LINK_MAGIC    0x54        // 'T'
#define TP_LINK_VERSION  1

enum : uint8_t {
    TP_CMD_PING        = 0,
    TP_CMD_PLAY        = 1,
    TP_CMD_PAUSE       = 2,
    TP_CMD_TOGGLE      = 3,   // the one a single play/pause button wants
    TP_CMD_REWIND      = 4,
    TP_CMD_SPEED_DELTA = 5,   // value = hundredths of a px/s, signed
    TP_CMD_SPEED_SET   = 6,   // value = hundredths of a px/s
    TP_CMD_REPACE      = 7,   // re-derive speed from what is left of the target
    TP_CMD_DURATION    = 8,   // value = seconds, 0 turns the target off
    TP_CMD_BRIGHTNESS  = 9,   // value = percent, 10-100
    TP_CMD_SPEED_TRIM  = 10,  // value = per mille, signed. See below.
    TP_CMD_SPEED_NUDGE = 11,  // value = per mille, relative. See below.
};

// Two ways to move the speed, for two shapes of control.
//
// TP_CMD_SPEED_NUDGE is the one BUTTONS want. It scales the base speed by a
// relative amount and the change sticks:
//
//     base *= (1 + value / 1000)
//
// Relative rather than absolute on purpose: a fixed "+5 px/s" step is a 22%
// change at 23 px/s and a 2% change at 200 px/s, so it would feel completely
// different depending on the script. A percentage feels the same everywhere.
// To get back onto the target time afterwards, send TP_CMD_REPACE.
//
// TP_CMD_SPEED_TRIM is the one a spring-centred control (joystick, slider)
// wants. It does not set a speed, it biases whatever the base speed already is:
//
//     effective = base * (1 + trim)
//
// so letting the stick go (trim 0) always returns exactly to the paced speed,
// and the target length can never be lost by fiddling with the control. The
// display clears the trim by itself if the remote goes quiet, so a remote that
// dies mid-deflection cannot leave the scroll stuck fast or slow.

struct __attribute__((packed)) tp_link_cmd_t {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  cmd;
    uint8_t  reserved;
    int32_t  value;
    uint16_t seq;             // echoed back in the status frame
};

#define TP_STATUS_PAUSED    0x01
#define TP_STATUS_FINISHED  0x02
#define TP_STATUS_TARGETED  0x04   // a target length is set

struct __attribute__((packed)) tp_link_status_t {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  flags;
    uint8_t  brightness;      // percent
    int16_t  speed_x10;       // px/s * 10
    uint16_t duration_s;      // 0 if no target
    uint16_t elapsed_s;
    int16_t  drift_s;         // + ahead of the target, - behind
    uint8_t  progress_pct;    // 0-100 through the run
    int8_t   trim_pct;        // trim currently applied, percent
    uint16_t ack_seq;         // seq of the command this answers
};

// Display side. Call after the soft AP is up.
void tp_link_start(void);
