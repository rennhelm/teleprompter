// Arduino.h first so ESP_LOGx actually reaches the serial port.
#include <Arduino.h>
#include "tp_link.h"

#include <WiFi.h>
#include <esp_log.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "tp_app.h"
#include "tp_config.h"

static const char *TAG = "tp_link";

struct queued_cmd_t {
    tp_link_cmd_t msg;
    uint8_t       from[6];
};

static QueueHandle_t s_queue = nullptr;
static uint8_t       s_peer[6] = { 0 };
static bool          s_have_peer = false;
static uint32_t      s_last_rx_ms = 0;
static bool          s_trim_applied = false;

// The receive callback runs in the Wi-Fi task. Applying a command can block for
// a couple of hundred milliseconds (a rewind re-rasterises), so all it does is
// hand the frame to our own task.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != (int)sizeof(tp_link_cmd_t)) return;

    queued_cmd_t q;
    memcpy(&q.msg, data, sizeof(q.msg));
    if (q.msg.magic != TP_LINK_MAGIC || q.msg.version != TP_LINK_VERSION) return;

    memcpy(q.from, info->src_addr, 6);
    xQueueSend(s_queue, &q, 0);         // drop rather than stall the Wi-Fi task
}

static void remember_peer(const uint8_t *mac)
{
    if (s_have_peer && memcmp(s_peer, mac, 6) == 0) return;

    if (s_have_peer) esp_now_del_peer(s_peer);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;                   // 0 = whatever channel we are already on
    peer.ifidx = WIFI_IF_AP;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) == ESP_OK) {
        memcpy(s_peer, mac, 6);
        s_have_peer = true;
        ESP_LOGI(TAG, "remote at %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

static void send_status(uint16_t ack_seq)
{
    if (!s_have_peer) return;

    tp_status_t st;
    tp_app_status(&st);

    const int32_t travel = (st.end_row > st.start_row) ? (st.end_row - st.start_row) : 1;
    int32_t covered = st.view_row - st.start_row;
    if (covered < 0) covered = 0;
    if (covered > travel) covered = travel;

    const float elapsed = st.elapsed_ms / 1000.0f;

    tp_link_status_t out = {};
    out.magic = TP_LINK_MAGIC;
    out.version = TP_LINK_VERSION;
    out.flags = (uint8_t)((st.paused ? TP_STATUS_PAUSED : 0) |
                          (st.finished ? TP_STATUS_FINISHED : 0) |
                          (st.duration_s > 0 ? TP_STATUS_TARGETED : 0));
    out.brightness = (uint8_t)st.brightness;
    out.speed_x10 = (int16_t)(st.speed * 10.0f);
    out.duration_s = (uint16_t)st.duration_s;
    out.elapsed_s = (uint16_t)elapsed;
    out.progress_pct = (uint8_t)(covered * 100 / travel);
    out.trim_pct = (int8_t)(st.trim * 100.0f);
    out.ack_seq = ack_seq;

    const float eff = st.speed * (1.0f + st.trim);
    if (st.duration_s > 0 && eff > 0.0f) {
        const float expected = travel * (elapsed / (float)st.duration_s);
        out.drift_s = (int16_t)((covered - expected) / eff);
    }
    out.speed_x10 = (int16_t)(eff * 10.0f);   // report what is actually happening

    esp_now_send(s_peer, (const uint8_t *)&out, sizeof(out));
}

static void apply(const tp_link_cmd_t &m)
{
    tp_status_t st;

    switch (m.cmd) {
    case TP_CMD_PING:                                            break;
    case TP_CMD_PLAY:       tp_app_set_paused(false);            break;
    case TP_CMD_PAUSE:      tp_app_set_paused(true);             break;
    case TP_CMD_REWIND:     tp_app_rewind();                     break;
    case TP_CMD_REPACE:     tp_app_repace();                     break;
    case TP_CMD_DURATION:   tp_app_set_duration((int)m.value);   break;
    case TP_CMD_BRIGHTNESS: tp_app_set_brightness((int)m.value); break;

    case TP_CMD_TOGGLE:
        tp_app_status(&st);
        tp_app_set_paused(!st.paused);
        break;

    case TP_CMD_SPEED_DELTA:
        tp_app_status(&st);
        tp_app_set_speed(st.speed + m.value / 100.0f);
        break;

    case TP_CMD_SPEED_SET:
        tp_app_set_speed(m.value / 100.0f);
        break;

    case TP_CMD_SPEED_NUDGE:
        tp_app_nudge_speed(1.0f + m.value / 1000.0f);
        break;

    case TP_CMD_SPEED_TRIM:
        tp_app_set_trim(m.value / 1000.0f);
        s_trim_applied = (m.value != 0);
        break;

    default:
        ESP_LOGW(TAG, "unknown command %u", (unsigned)m.cmd);
        break;
    }
}

static void link_task(void *)
{
    queued_cmd_t q;
    for (;;) {
        // Wake on a command, or every 250 ms to push a fresh status frame so a
        // remote with a screen keeps up without having to poll.
        if (xQueueReceive(s_queue, &q, pdMS_TO_TICKS(250)) == pdTRUE) {
            s_last_rx_ms = millis();
            remember_peer(q.from);
            apply(q.msg);
            send_status(q.msg.seq);
            continue;
        }

        // Failsafe: a remote that dies while the stick is deflected must not
        // leave the scroll permanently biased. Trim is momentary by definition,
        // so if the remote goes quiet it goes back to neutral.
        if (s_trim_applied && (millis() - s_last_rx_ms) > TP_LINK_TIMEOUT_MS) {
            s_trim_applied = false;
            tp_app_set_trim(0.0f);
            ESP_LOGW(TAG, "remote went quiet, trim released");
        }
        send_status(0);
    }
}

void tp_link_start(void)
{
    s_queue = xQueueCreate(8, sizeof(queued_cmd_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "could not create the command queue");
        return;
    }

    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return;
    }
    esp_now_register_recv_cb(on_recv);

    uint8_t ch = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&ch, &sec);
    ESP_LOGI(TAG, "ESP-NOW listening on channel %u, AP mac %s",
             (unsigned)ch, WiFi.softAPmacAddress().c_str());

    xTaskCreatePinnedToCore(link_task, "tp_link", 4096, nullptr, 3, nullptr, 0);
}
