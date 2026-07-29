#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* An optional network baked in at build time, for boards that cannot see the
 * router in a scan or that are being set up without a phone to hand. Stored
 * credentials always win over it. */
#if defined(__has_include)
#  if __has_include("wifi_default.h")
#    include "wifi_default.h"
#  endif
#endif
#ifndef FC_DEFAULT_SSID
#  define FC_DEFAULT_SSID ""
#  define FC_DEFAULT_PASS ""
#endif

static const char *TAG = "wifi";
static const char *NS = "fcontrol";

static esp_netif_t *s_sta_netif;
static char    s_ssid[WIFI_SSID_MAX + 1];
static bool    s_configured;
static bool    s_joined;
static char    s_ip[16] = "";
static uint8_t s_ap_channel = 1;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_joined = false;
        s_ip[0] = '\0';
        /* Keep trying. A router rebooting should not mean the board needs one
         * too, and the access point stays up throughout either way. */
        if (s_configured) esp_wifi_connect();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_joined = true;

        uint8_t ch = 0;
        wifi_second_chan_t second = 0;
        esp_wifi_get_channel(&ch, &second);

        ESP_LOGI(TAG, "joined \"%s\", address %s, radio now on channel %u",
                 s_ssid, s_ip, (unsigned)ch);
        if (ch != s_ap_channel) {
            ESP_LOGW(TAG, "the mesh has moved to channel %u with it, so only "
                          "boards on this same network can hear this one",
                     (unsigned)ch);
        }
    }
}

static void load_saved(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof s_ssid;
    if (nvs_get_str(h, "sta_ssid", s_ssid, &len) == ESP_OK && s_ssid[0]) {
        s_configured = true;
    }
    nvs_close(h);
}

static bool stored_password(char *out, size_t cap) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    const bool ok = nvs_get_str(h, "sta_pass", out, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void apply_station(const char *ssid, const char *password) {
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid - 1);
    if (password && password[0]) {
        strncpy((char *)cfg.sta.password, password, sizeof cfg.sta.password - 1);
    }
    cfg.sta.threshold.authmode = (password && password[0])
        ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    esp_wifi_connect();
}

void wifi_start(const char *ap_ssid, uint8_t ap_channel) {
    s_ap_channel = ap_channel;
    s_sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, ap_ssid, sizeof ap.ap.ssid - 1);
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = ap_channel;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;   /* see the note in web.h */

    load_saved();

    /* Nothing stored, but this build carries a network. Use it, and save it,
     * so it behaves from then on exactly like one typed into the dashboard and
     * can be replaced from there. */
    if (!s_configured && FC_DEFAULT_SSID[0] != 0) {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "sta_ssid", FC_DEFAULT_SSID);
            nvs_set_str(h, "sta_pass", FC_DEFAULT_PASS);
            nvs_commit(h);
            nvs_close(h);
        }
        snprintf(s_ssid, sizeof s_ssid, "%s", FC_DEFAULT_SSID);
        s_configured = true;
        ESP_LOGI(TAG, "using the network built into this firmware");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(s_configured ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Left at its default, the radio dozes between transmissions to save
     * power, and it does this on the SAME radio that is also running ESP-NOW
     * beacons and relays. A phone connected to the board's access point reads
     * those naps as the access point going quiet, and reconnects, over and
     * over, exactly the "keeps disconnecting me" symptom this build hit.
     * There is no modem sleep tradeoff worth making here: these boards run
     * on USB power, not battery, so there is nothing to save. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (s_configured) {
        char pass[WIFI_PASS_MAX + 1] = "";
        stored_password(pass, sizeof pass);
        ESP_LOGI(TAG, "a network is stored, trying \"%s\"", s_ssid);
        apply_station(s_ssid, pass);
    } else {
        /* Only pin the channel when nothing else is deciding it for us. */
        ESP_ERROR_CHECK(esp_wifi_set_channel(ap_channel, WIFI_SECOND_CHAN_NONE));
    }

    ESP_LOGI(TAG, "access point \"%s\" up on channel %u", ap_ssid, (unsigned)ap_channel);
}

void wifi_status(wifi_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->joined = s_joined;
    out->configured = s_configured;
    snprintf(out->ssid, sizeof out->ssid, "%s", s_ssid);
    snprintf(out->ip, sizeof out->ip, "%s", s_ip);

    uint8_t ch = 0;
    wifi_second_chan_t second = 0;
    if (esp_wifi_get_channel(&ch, &second) == ESP_OK) out->channel = ch;
}

size_t wifi_scan(wifi_found_t *out, size_t max) {
    /* Scanning needs the station interface, which does not exist in access
     * point only mode. */
    wifi_mode_t mode = WIFI_MODE_AP;
    esp_wifi_get_mode(&mode);
    const bool was_ap_only = (mode == WIFI_MODE_AP);
    if (was_ap_only) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        /* The station interface is not ready the instant the mode changes, and
         * scanning too early returns an error that looks exactly like "there
         * are no networks here". */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    size_t n = 0;
    const esp_err_t started = esp_wifi_scan_start(NULL, true);
    if (started != ESP_OK) {
        ESP_LOGW(TAG, "scan could not start: %s", esp_err_to_name(started));
    }
    if (started == ESP_OK) {
        uint16_t found = WIFI_SCAN_MAX;
        wifi_ap_record_t recs[WIFI_SCAN_MAX];
        if (esp_wifi_scan_get_ap_records(&found, recs) == ESP_OK) {
            for (uint16_t i = 0; i < found && n < max; i++) {
                if (recs[i].ssid[0] == '\0') continue;   /* hidden, unusable here */
                snprintf(out[n].ssid, sizeof out[n].ssid, "%s", (const char *)recs[i].ssid);
                out[n].rssi = recs[i].rssi;
                out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                n++;
            }
        }
    }

    if (was_ap_only) {
        esp_wifi_set_mode(WIFI_MODE_AP);
        /* A scan leaves the radio wherever it finished, which would silently
         * take the mesh with it. Put it back. */
        esp_wifi_set_channel(s_ap_channel, WIFI_SECOND_CHAN_NONE);
    }

    ESP_LOGI(TAG, "scan found %u networks", (unsigned)n);
    return n;
}

bool wifi_join(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > WIFI_SSID_MAX) return false;
    if (password && strlen(password) > WIFI_PASS_MAX) return false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "sta_ssid", ssid);
    nvs_set_str(h, "sta_pass", password ? password : "");
    nvs_commit(h);
    nvs_close(h);

    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    s_configured = true;
    s_joined = false;
    s_ip[0] = '\0';

    ESP_LOGI(TAG, "joining \"%s\"", ssid);
    apply_station(ssid, password);
    return true;
}

void wifi_leave(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "sta_ssid");
        nvs_erase_key(h, "sta_pass");
        nvs_commit(h);
        nvs_close(h);
    }

    s_configured = false;
    s_joined = false;
    s_ssid[0] = '\0';
    s_ip[0] = '\0';

    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_channel(s_ap_channel, WIFI_SECOND_CHAN_NONE);

    ESP_LOGI(TAG, "left the network, back on channel %u", (unsigned)s_ap_channel);
}
