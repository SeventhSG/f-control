#include "net.h"

#include <string.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "net";

static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static ident_t        *s_id;
static fmesh_t         s_mesh;
static SemaphoreHandle_t s_lock;
static QueueHandle_t   s_rx_q;
static net_msg_cb_t    s_on_msg;
static net_roster_cb_t s_on_roster;

typedef struct {
    uint8_t frame[FCP_MAX_FRAME];
    size_t  len;
    int8_t  rssi;
} rx_item_t;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t rng(void *ctx) {
    (void)ctx;
    return esp_random();
}

/* ---- transmit ---------------------------------------------------------- */

static void air_send(const uint8_t *frame, size_t len) {
    const esp_err_t e = esp_now_send(BROADCAST, frame, len);
    if (e != ESP_OK) ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(e));
}

/* ---- receive ----------------------------------------------------------- */

/* Runs in the wifi task. It must not block and must not do real work, so it
 * copies the frame into a queue and returns. */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len <= 0 || len > FCP_MAX_FRAME) return;

    rx_item_t item;
    memcpy(item.frame, data, (size_t)len);
    item.len = (size_t)len;
    item.rssi = (info && info->rx_ctrl) ? (int8_t)info->rx_ctrl->rssi : -100;

    if (xQueueSend(s_rx_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full, dropping a frame");
    }
}

static void deliver(const fcp_hdr_t *h, const uint8_t *payload, size_t payload_len) {
    /* Frame shape is identical to the encrypted one so nothing downstream has
     * to change when crypto lands: a 12 byte nonce, the body, a 16 byte tag.
     * In this build the body is plaintext and the tag is zero. */
    if (payload_len <= FCP_NONCE_LEN + FCP_TAG_LEN) return;

    const char *text = (const char *)(payload + FCP_NONCE_LEN);
    const size_t text_len = payload_len - FCP_NONCE_LEN - FCP_TAG_LEN;

    if (s_on_msg) s_on_msg(h->src_id, text, text_len);
}

static void rx_task(void *arg) {
    (void)arg;
    rx_item_t item;

    for (;;) {
        if (xQueueReceive(s_rx_q, &item, portMAX_DELAY) != pdTRUE) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const fmesh_action_t act = fmesh_rx(&s_mesh, item.frame, item.len, item.rssi, now_ms());
        xSemaphoreGive(s_lock);

        if (act == FMESH_DELIVER) {
            fcp_hdr_t h;
            const uint8_t *p = NULL;
            size_t plen = 0;
            if (fcp_frame_decode(item.frame, item.len, &h, &p, &plen) == FCP_OK) {
                deliver(&h, p, plen);
            }
        } else if (act == FMESH_BEACON) {
            /* Discovery is otherwise invisible without a phone attached to the
             * dashboard, which makes a two board bring-up test impossible to
             * observe. This line is the test output. */
            fcp_hdr_t h;
            const uint8_t *p = NULL;
            size_t plen = 0;
            if (fcp_frame_decode(item.frame, item.len, &h, &p, &plen) == FCP_OK) {
                const fmesh_peer_t *peer = fmesh_peer(&s_mesh, h.src_id);
                ESP_LOGI(TAG, "heard %02x%02x%02x%02x%02x%02x \"%s\"  %d dBm  %u hop%s",
                         h.src_id[0], h.src_id[1], h.src_id[2],
                         h.src_id[3], h.src_id[4], h.src_id[5],
                         peer ? peer->name : "",
                         (int)item.rssi, (unsigned)h.hop_count,
                         h.hop_count == 1 ? "" : "s");
            }
            if (s_on_roster) s_on_roster();
        }
    }
}

/* ---- relay pump -------------------------------------------------------- */

/* Repeats come due on their own schedule, so this drains them often enough
 * that a 20 ms assessment delay is still roughly a 20 ms delay. */
static void relay_task(void *arg) {
    (void)arg;
    for (;;) {
        fmesh_out_t out[FMESH_RELAY_SLOTS];
        uint8_t copies[FMESH_RELAY_SLOTS][FCP_MAX_FRAME];
        size_t lens[FMESH_RELAY_SLOTS];

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const size_t n = fmesh_due(&s_mesh, now_ms(), out, FMESH_RELAY_SLOTS);
        for (size_t i = 0; i < n; i++) {
            memcpy(copies[i], out[i].frame, out[i].len);
            lens[i] = out[i].len;
        }
        xSemaphoreGive(s_lock);

        /* Transmit outside the lock. The queue slot is already released, so
         * the copies above are what actually goes on the air. */
        for (size_t i = 0; i < n; i++) air_send(copies[i], lens[i]);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---- beacons ----------------------------------------------------------- */

static void beacon_task(void *arg) {
    (void)arg;

    for (;;) {
        fcp_hdr_t h;
        memset(&h, 0, sizeof h);
        h.version = FCP_VERSION;
        h.type = FCP_T_BEACON;
        h.flags = 0;
        h.msg_id = esp_random();
        h.hop_limit = 1;
        h.hop_count = 0;
        memcpy(h.src_id, s_id->id, FCP_ID_LEN);

        fcp_beacon_t b;
        memset(&b, 0, sizeof b);
        memcpy(b.ed25519_pub, s_id->key, FCP_PUB_LEN);
        memcpy(b.x25519_pub, s_id->key, FCP_PUB_LEN);
        const size_t nlen = strlen(s_id->name);
        b.name_len = (uint8_t)(nlen > FCP_NAME_MAX ? FCP_NAME_MAX : nlen);
        memcpy(b.name, s_id->name, b.name_len);
        b.boot_id = s_id->boot_id;
        b.seq = ++s_id->seq;
        /* signature stays zero: this build signs nothing */

        uint8_t payload[FCP_BEACON_PAYLOAD_LEN];
        size_t plen = 0;
        uint8_t frame[FCP_MAX_FRAME];
        size_t flen = 0;

        if (fcp_beacon_encode(&b, payload, sizeof payload, &plen) == FCP_OK
            && fcp_frame_encode(&h, payload, plen, frame, sizeof frame, &flen) == FCP_OK) {
            air_send(frame, flen);
        } else {
            ESP_LOGE(TAG, "could not build a beacon");
        }

        const uint32_t jitter = esp_random() % NET_BEACON_JITTER;
        vTaskDelay(pdMS_TO_TICKS(NET_BEACON_MS - (NET_BEACON_JITTER / 2) + jitter));
    }
}

/* ---- api --------------------------------------------------------------- */

size_t net_roster(fmesh_peer_t *out, size_t max) {
    size_t n = 0;
    const uint32_t t = now_ms();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < FMESH_PEER_SLOTS && n < max; i++) {
        const fmesh_peer_t *p = &s_mesh.peers[i];
        if (!p->used) continue;
        if ((uint32_t)(t - p->last_heard_ms) > FMESH_PEER_TTL_MS) continue;
        out[n++] = *p;
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t net_send_text(const uint8_t dst_id[FCP_ID_LEN], const char *text, size_t len) {
    if (text == NULL || len == 0 || len > NET_TEXT_MAX) return 0;

    fcp_hdr_t h;
    memset(&h, 0, sizeof h);
    h.version = FCP_VERSION;
    h.type = FCP_T_MSG;
    h.flags = FCP_FLAG_ADDRESSED;
    h.msg_id = esp_random();
    if (h.msg_id == 0) h.msg_id = 1;   /* 0 means failure to the caller */
    h.hop_limit = 3;
    h.hop_count = 0;
    memcpy(h.src_id, s_id->id, FCP_ID_LEN);
    memcpy(h.dst_id, dst_id, FCP_ID_LEN);

    uint8_t payload[FCP_NONCE_LEN + NET_TEXT_MAX + FCP_TAG_LEN];
    esp_fill_random(payload, FCP_NONCE_LEN);
    memcpy(payload + FCP_NONCE_LEN, text, len);
    memset(payload + FCP_NONCE_LEN + len, 0, FCP_TAG_LEN);   /* no tag, no crypto */

    uint8_t frame[FCP_MAX_FRAME];
    size_t flen = 0;
    if (fcp_frame_encode(&h, payload, FCP_NONCE_LEN + len + FCP_TAG_LEN,
                         frame, sizeof frame, &flen) != FCP_OK) {
        return 0;
    }

    air_send(frame, flen);
    return h.msg_id;
}

void net_stats(fmesh_stats_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_mesh.stats;
    xSemaphoreGive(s_lock);
}

void net_start(ident_t *id, net_msg_cb_t on_msg, net_roster_cb_t on_roster) {
    s_id = id;
    s_on_msg = on_msg;
    s_on_roster = on_roster;

    s_lock = xSemaphoreCreateMutex();
    s_rx_q = xQueueCreate(12, sizeof(rx_item_t));

    fmesh_init(&s_mesh, id->id, rng, NULL);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = NET_CHANNEL;
    peer.ifidx = WIFI_IF_AP;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    xTaskCreate(rx_task, "fc_rx", 4096, NULL, 6, NULL);
    xTaskCreate(relay_task, "fc_relay", 3072, NULL, 5, NULL);
    xTaskCreate(beacon_task, "fc_beacon", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "radio up on channel %u", NET_CHANNEL);
}
