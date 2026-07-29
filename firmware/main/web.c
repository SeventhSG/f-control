/* The board's own access point, its own web server, and the websocket the
 * dashboard talks over. One gzipped file comes out of flash, one socket
 * carries everything else.
 *
 * There is no clock on this board, so every timestamp is uptime, and the
 * interface is told that rather than being handed a fake wall time.
 */
#include "web.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"

static const char *TAG = "web";

extern const uint8_t dash_start[] asm("_binary_index_html_gz_start");
extern const uint8_t dash_end[]   asm("_binary_index_html_gz_end");

#define MAX_CLIENTS 4
#define MAX_MSGS    48

typedef struct {
    uint32_t id;
    uint8_t  peer[FCP_ID_LEN];
    bool     outgoing;
    char     at[8];
    char     text[NET_TEXT_MAX + 1];
    char     state[10];
    bool     used;
} msg_t;

static httpd_handle_t s_server;
static int            s_clients[MAX_CLIENTS];
static size_t         s_client_n;
static ident_t       *s_id;
static msg_t          s_msgs[MAX_MSGS];
static size_t         s_msg_next;

static void uptime_str(char *out, size_t cap) {
    const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(out, cap, "%02u:%02u", (unsigned)((s / 60) % 100), (unsigned)(s % 60));
}

static void hex_id(const uint8_t id[FCP_ID_LEN], char *out) {
    for (int i = 0; i < FCP_ID_LEN; i++) sprintf(out + i * 2, "%02x", id[i]);
    out[FCP_ID_LEN * 2] = '\0';
}

/* ---- sending ----------------------------------------------------------- */

static void ws_broadcast(cJSON *obj) {
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) return;

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };

    for (size_t i = 0; i < s_client_n; i++) {
        httpd_ws_send_frame_async(s_server, s_clients[i], &frame);
    }
    free(text);
}

static void send_me(void) {
    cJSON *o = cJSON_CreateObject();
    char words[80];
    ident_words(s_id, words, sizeof words);

    cJSON_AddStringToObject(o, "t", "me");
    cJSON_AddStringToObject(o, "name", s_id->name[0] ? s_id->name : "unnamed");
    cJSON_AddStringToObject(o, "fp", words);
    cJSON_AddNumberToObject(o, "contacts", 0);
    cJSON_AddNumberToObject(o, "max", FMESH_PEER_SLOTS);
    /* The interface renders a standing warning off this flag. It is not
     * decoration: without it somebody could read the README, flash this, and
     * believe their messages are protected. */
    cJSON_AddBoolToObject(o, "nocrypto", true);
    ws_broadcast(o);
}

static void send_net(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "net");
    cJSON_AddStringToObject(o, "mode", "ap");
    cJSON_AddNumberToObject(o, "meshChannel", NET_CHANNEL);
    cJSON_AddBoolToObject(o, "beta", false);
    ws_broadcast(o);
}

void web_send_roster(void) {
    if (s_server == NULL || s_client_n == 0) return;

    fmesh_peer_t peers[FMESH_PEER_SLOTS];
    const size_t n = net_roster(peers, FMESH_PEER_SLOTS);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "roster");
    cJSON *arr = cJSON_AddArrayToObject(o, "peers");

    for (size_t i = 0; i < n; i++) {
        char idhex[FCP_ID_LEN * 2 + 1];
        hex_id(peers[i].id, idhex);

        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", idhex);
        cJSON_AddStringToObject(p, "name", "");
        cJSON_AddBoolToObject(p, "verified", false);
        cJSON_AddStringToObject(p, "fp", "");
        cJSON_AddNumberToObject(p, "rssi", peers[i].rssi);
        cJSON_AddNumberToObject(p, "hops", peers[i].hops);
        cJSON_AddItemToArray(arr, p);
    }
    ws_broadcast(o);
}

static cJSON *msg_json(const msg_t *m) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", m->id);
    cJSON_AddStringToObject(j, "dir", m->outgoing ? "out" : "in");
    cJSON_AddStringToObject(j, "text", m->text);
    cJSON_AddStringToObject(j, "at", m->at);
    cJSON_AddStringToObject(j, "state", m->state);
    return j;
}

static msg_t *msg_add(const uint8_t peer[FCP_ID_LEN], bool outgoing,
                      const char *text, size_t len, const char *state) {
    msg_t *m = &s_msgs[s_msg_next];
    s_msg_next = (s_msg_next + 1) % MAX_MSGS;

    memset(m, 0, sizeof(*m));
    m->id = (uint32_t)(esp_timer_get_time() & 0x7FFFFFFF);
    memcpy(m->peer, peer, FCP_ID_LEN);
    m->outgoing = outgoing;
    if (len > NET_TEXT_MAX) len = NET_TEXT_MAX;
    memcpy(m->text, text, len);
    uptime_str(m->at, sizeof m->at);
    snprintf(m->state, sizeof m->state, "%s", state);
    m->used = true;
    return m;
}

void web_on_message(const uint8_t src_id[FCP_ID_LEN], const char *text, size_t len) {
    msg_t *m = msg_add(src_id, false, text, len, "heard");

    char idhex[FCP_ID_LEN * 2 + 1];
    hex_id(src_id, idhex);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "msg");
    cJSON_AddStringToObject(o, "peer", idhex);
    cJSON_AddItemToObject(o, "message", msg_json(m));
    ws_broadcast(o);
}

/* ---- receiving --------------------------------------------------------- */

static bool parse_id(const char *hex, uint8_t out[FCP_ID_LEN]) {
    if (hex == NULL || strlen(hex) != FCP_ID_LEN * 2) return false;
    for (int i = 0; i < FCP_ID_LEN; i++) {
        unsigned v = 0;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static void send_thread(const char *peer_hex) {
    uint8_t peer[FCP_ID_LEN];
    if (!parse_id(peer_hex, peer)) return;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "thread");
    cJSON_AddStringToObject(o, "peer", peer_hex);
    cJSON *arr = cJSON_AddArrayToObject(o, "messages");

    /* Oldest first, walking the ring from the next write position. */
    for (size_t k = 0; k < MAX_MSGS; k++) {
        const msg_t *m = &s_msgs[(s_msg_next + k) % MAX_MSGS];
        if (m->used && memcmp(m->peer, peer, FCP_ID_LEN) == 0) {
            cJSON_AddItemToArray(arr, msg_json(m));
        }
    }
    ws_broadcast(o);
}

static void handle_frame(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return;

    const cJSON *t = cJSON_GetObjectItem(root, "t");
    if (!cJSON_IsString(t)) { cJSON_Delete(root); return; }

    if (strcmp(t->valuestring, "open") == 0) {
        const cJSON *p = cJSON_GetObjectItem(root, "peer");
        if (cJSON_IsString(p)) send_thread(p->valuestring);

    } else if (strcmp(t->valuestring, "send") == 0) {
        const cJSON *p = cJSON_GetObjectItem(root, "peer");
        const cJSON *x = cJSON_GetObjectItem(root, "text");
        uint8_t peer[FCP_ID_LEN];
        if (cJSON_IsString(p) && cJSON_IsString(x) && parse_id(p->valuestring, peer)) {
            const size_t len = strlen(x->valuestring);
            const uint32_t mid = net_send_text(peer, x->valuestring, len);

            /* "heard" would be a lie. This build has no acknowledgements, so
             * all we know is that the frame left the radio. The interface
             * renders anything that is not heard or lost as still in flight,
             * which is exactly the truth. */
            msg_t *m = msg_add(peer, true, x->valuestring, len, mid ? "pending" : "lost");
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "t", "msg");
            cJSON_AddStringToObject(o, "peer", p->valuestring);
            cJSON_AddItemToObject(o, "message", msg_json(m));
            ws_broadcast(o);
        }

    } else if (strcmp(t->valuestring, "wipe") == 0) {
        memset(s_msgs, 0, sizeof s_msgs);
        s_msg_next = 0;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "wiped");
        ws_broadcast(o);
    }

    cJSON_Delete(root);
}

/* ---- http -------------------------------------------------------------- */

static esp_err_t root_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)dash_start, dash_end - dash_start);
}

static void remember_client(int fd) {
    for (size_t i = 0; i < s_client_n; i++) if (s_clients[i] == fd) return;
    if (s_client_n < MAX_CLIENTS) s_clients[s_client_n++] = fd;
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        remember_client(httpd_req_to_sockfd(req));
        ESP_LOGI(TAG, "dashboard connected");

        /* Nothing is locked, because there is nothing to unlock in a build
         * with no keys. Say so immediately so the interface does not sit on a
         * passphrase screen that cannot do anything. */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "state");
        cJSON_AddBoolToObject(o, "locked", false);
        ws_broadcast(o);

        send_me();
        send_net();
        web_send_roster();
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK || frame.len == 0 || frame.len > 1024) return err;

    uint8_t *buf = calloc(1, frame.len + 1);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    frame.payload = buf;

    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err == ESP_OK) handle_frame((const char *)buf);
    free(buf);
    return err;
}

/* ---- access point ------------------------------------------------------ */

static void ap_start(void) {
    char ssid[32];
    ident_ap_name(s_id, ssid, sizeof ssid);

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.ap.ssid, ssid, sizeof cfg.ap.ssid - 1);
    cfg.ap.ssid_len = strlen(ssid);
    cfg.ap.channel = NET_CHANNEL;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_OPEN;   /* see the note in web.h */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(NET_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_LOGI(TAG, "access point \"%s\" up on channel %u", ssid, NET_CHANNEL);
}

void web_start(ident_t *id) {
    s_id = id;
    ap_start();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = MAX_CLIENTS + 2;
    cfg.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    static const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get,
    };
    static const httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &ws);

    ESP_LOGI(TAG, "dashboard at http://192.168.4.1  (%d bytes gzipped)",
             (int)(dash_end - dash_start));
}
