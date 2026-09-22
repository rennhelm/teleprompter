// Teleprompter remote - ESP32-S3, three buttons.
//
// Faster / slower step the display's base speed by a percentage and the change
// sticks, which is what a button wants (unlike a spring-centred control, there
// is nothing to return to neutral on its own). Holding a speed button repeats.
//
// Faster       : +5% per press, repeats when held
// Slower       : -5% per press, repeats when held
// Play         : play / pause
// Play, long   : back to top
// Both speed   : re-pace, recompute the speed to still land on the target time
//
// The LED shows what the display is actually doing, from the status frames it
// sends back: red = no link, amber = paused, green = playing, blue = finished,
// and it blinks when you have drifted more than a few seconds off the target.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define TP_REMOTE_BUILD "2026-09-18a"

#include "ctl_config.h"
#include "tp_link.h"

static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint16_t s_seq = 0;
static uint32_t s_last_tx_ms = 0;
static uint32_t s_flash_until_ms = 0;     // brief LED confirmation of a command

// Link state, updated from the status frames the display sends back
static volatile tp_link_status_t s_status = {};
static volatile uint32_t s_last_status_ms = 0;

// ---------------------------------------------------------------------------
// Link
// ---------------------------------------------------------------------------

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if (len != (int)sizeof(tp_link_status_t)) return;

    tp_link_status_t st;
    memcpy(&st, data, sizeof(st));
    if (st.magic != TP_LINK_MAGIC || st.version != TP_LINK_VERSION) return;

    memcpy((void *)&s_status, &st, sizeof(st));
    s_last_status_ms = millis();
}

static void send_cmd(uint8_t cmd, int32_t value)
{
    tp_link_cmd_t m = {};
    m.magic = TP_LINK_MAGIC;
    m.version = TP_LINK_VERSION;
    m.cmd = cmd;
    m.value = value;
    m.seq = ++s_seq;
    const esp_err_t err = esp_now_send(BROADCAST, (const uint8_t *)&m, sizeof(m));

    // Idle pings are not worth printing; everything else is, so "the buttons do
    // nothing" is answerable from the serial monitor alone.
    if (cmd != TP_CMD_PING) {
        static const char *NAMES[] = {
            "ping", "play", "pause", "toggle", "rewind", "speed_delta",
            "speed_set", "repace", "duration", "brightness", "trim", "nudge"
        };
        Serial.printf("tx %-10s value=%ld seq=%u%s\n",
                      cmd < (sizeof(NAMES) / sizeof(NAMES[0])) ? NAMES[cmd] : "?",
                      (long)value, m.seq, err == ESP_OK ? "" : "  SEND FAILED");
    }

    s_last_tx_ms = millis();
    s_flash_until_ms = s_last_tx_ms + 60;
}

static void link_begin(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Force the channel while unassociated. The promiscuous toggle is the
    // belt-and-braces version of this and is harmless.
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(CTL_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() != ESP_OK) {
        Serial.println("esp_now_init failed");
        return;
    }
    esp_now_register_recv_cb(on_recv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = CTL_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.printf("remote up on channel %d, mac %s\n",
                  CTL_CHANNEL, WiFi.macAddress().c_str());
}

// ---------------------------------------------------------------------------
// Buttons. Active low with the internal pull-up.
// ---------------------------------------------------------------------------

enum : uint8_t { EV_NONE = 0, EV_PRESS, EV_REPEAT, EV_LONG, EV_SHORT_RELEASE };

struct button_t {
    uint8_t  pin;
    bool     stable;            // true = released
    bool     last_raw;
    uint32_t changed_ms;
    uint32_t pressed_ms;
    uint32_t next_repeat_ms;
    bool     long_fired;
    bool     suppressed;        // consumed by a chord, ignore until released
};

static button_t s_faster = { CTL_PIN_FASTER, true, true, 0, 0, 0, false, false };
static button_t s_slower = { CTL_PIN_SLOWER, true, true, 0, 0, 0, false, false };
static button_t s_play   = { CTL_PIN_PLAY,   true, true, 0, 0, 0, false, false };

static bool held(const button_t &b) { return !b.stable; }

static uint8_t service_button(button_t &b, bool repeats, bool wants_long)
{
    const bool raw = digitalRead(b.pin);
    const uint32_t now = millis();

    if (raw != b.last_raw) {
        b.last_raw = raw;
        b.changed_ms = now;
    }

    uint8_t event = EV_NONE;

    if ((now - b.changed_ms) >= CTL_DEBOUNCE_MS && raw != b.stable) {
        b.stable = raw;
        Serial.printf("btn GPIO%u %s\n", (unsigned)b.pin, raw ? "up" : "DOWN");
        if (!raw) {                         // pressed
            b.pressed_ms = now;
            b.next_repeat_ms = now + CTL_REPEAT_DELAY_MS;
            b.long_fired = false;
            b.suppressed = false;
            event = EV_PRESS;
        } else {                            // released
            if (b.suppressed) { b.suppressed = false; }
            else if (!b.long_fired) event = EV_SHORT_RELEASE;
        }
    }

    if (b.suppressed) return EV_NONE;

    if (repeats && held(b) && now >= b.next_repeat_ms) {
        b.next_repeat_ms = now + CTL_REPEAT_MS;
        event = EV_REPEAT;
    }

    if (wants_long && held(b) && !b.long_fired &&
        (now - b.pressed_ms) >= CTL_LONG_PRESS_MS) {
        b.long_fired = true;
        event = EV_LONG;
    }

    return event;
}

// ---------------------------------------------------------------------------
// LED
// ---------------------------------------------------------------------------

static void led_show(uint8_t r, uint8_t g, uint8_t b)
{
#if CTL_PIN_LED >= 0
    // Only write on an actual change. Re-sending the same colour 25 times a
    // second is pointless, and with CORE_DEBUG_LEVEL=3 every call also printed
    // a deprecation warning, which buried the serial log.
    static int last_r = -1, last_g = -1, last_b = -1;
    if (r == last_r && g == last_g && b == last_b) return;
    last_r = r; last_g = g; last_b = b;
    rgbLedWrite(CTL_PIN_LED, r, g, b);
#else
    (void)r; (void)g; (void)b;
#endif
}

static void service_led(void)
{
#if CTL_PIN_LED >= 0
    static uint32_t last = 0;
    const uint32_t now = millis();
    if (now - last < 40) return;
    last = now;

    if (now < s_flash_until_ms) { led_show(40, 40, 40); return; }   // command sent

    if ((now - s_last_status_ms) >= CTL_LINK_LOST_MS) {
        led_show(24, 0, 0);                                         // red, no link
        return;
    }

    tp_link_status_t st;
    memcpy(&st, (const void *)&s_status, sizeof(st));

    uint8_t r = 0, g = 0, b = 0;
    if (st.flags & TP_STATUS_FINISHED)    { r = 0;  g = 0;  b = 24; }
    else if (st.flags & TP_STATUS_PAUSED) { r = 24; g = 12; b = 0;  }
    else                                  { r = 0;  g = 24; b = 0;  }

    // Blink once you are more than 3 s off the target, so you get the nudge
    // without having to look anywhere but the script.
    if ((st.flags & TP_STATUS_TARGETED) && !(st.flags & TP_STATUS_PAUSED) &&
        abs((int)st.drift_s) > 3 && ((now / 250) & 1)) {
        r = g = b = 0;
    }
    led_show(r, g, b);
#endif
}

// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    // Long enough for USB CDC to enumerate on the host. Anything printed before
    // that is simply lost, which is how the boot banner went missing last time.
    delay(600);

    pinMode(CTL_PIN_FASTER, INPUT_PULLUP);
    pinMode(CTL_PIN_SLOWER, INPUT_PULLUP);
    pinMode(CTL_PIN_PLAY, INPUT_PULLUP);

    link_begin();

    Serial.println("=== teleprompter remote, build " TP_REMOTE_BUILD " ===");
    // With nothing pressed all three should read 1. A 0 here means that button
    // is wired to 3V3 instead of GND, or is on a different pin than configured.
    Serial.printf("buttons at rest: faster(GPIO%d)=%d slower(GPIO%d)=%d play(GPIO%d)=%d"
                  "   (all should be 1)\n",
                  CTL_PIN_FASTER, digitalRead(CTL_PIN_FASTER),
                  CTL_PIN_SLOWER, digitalRead(CTL_PIN_SLOWER),
                  CTL_PIN_PLAY, digitalRead(CTL_PIN_PLAY));

    send_cmd(TP_CMD_PING, 0);
}

void loop()
{
    const uint8_t ef = service_button(s_faster, true, false);
    const uint8_t es = service_button(s_slower, true, false);

    // Both speed buttons together means "put me back on schedule". Checked
    // before acting on either, and both are then suppressed until released so
    // the chord does not also ripple the speed up and down.
    if (held(s_faster) && held(s_slower) &&
        !s_faster.suppressed && !s_slower.suppressed) {
        s_faster.suppressed = true;
        s_slower.suppressed = true;
        send_cmd(TP_CMD_REPACE, 0);
    } else {
        if (ef == EV_PRESS || ef == EV_REPEAT) {
            send_cmd(TP_CMD_SPEED_NUDGE, (int32_t)(CTL_STEP * 1000.0f));
        }
        if (es == EV_PRESS || es == EV_REPEAT) {
            send_cmd(TP_CMD_SPEED_NUDGE, -(int32_t)(CTL_STEP * 1000.0f));
        }
    }

    switch (service_button(s_play, false, true)) {
    case EV_SHORT_RELEASE: send_cmd(TP_CMD_TOGGLE, 0); break;
    case EV_LONG:          send_cmd(TP_CMD_REWIND, 0); break;
    default: break;
    }

    // Say out loud when the display starts or stops answering, so the serial
    // monitor tells you which half of the link is the problem.
    static bool linked_prev = false;
    const bool linked = (millis() - s_last_status_ms) < CTL_LINK_LOST_MS;
    if (linked != linked_prev) {
        linked_prev = linked;
        Serial.println(linked ? "link UP: display is answering"
                              : "link DOWN: no status from the display");
    }

    // Keeps the display's reply address fresh, so the LED stays meaningful even
    // if the display reboots while the remote is idle.
    if ((millis() - s_last_tx_ms) > CTL_HEARTBEAT_MS) send_cmd(TP_CMD_PING, 0);

    service_led();
    delay(CTL_POLL_MS);
}
