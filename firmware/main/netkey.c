#include "netkey.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "netkey";
static const char *NS = "fcontrol";

static uint8_t s_key[FCRYPTO_KEY_LEN];
static bool    s_loaded;

static void store_and_log(const char *how) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "netkey", s_key, FCRYPTO_KEY_LEN);
        nvs_commit(h);
        nvs_close(h);
    }
    char fp[16];
    fcrypto_key_fingerprint(s_key, fp, sizeof fp);
    ESP_LOGI(TAG, "network key set %s, fingerprint %s", how, fp);
}

void netkey_load(uint8_t key[FCRYPTO_KEY_LEN]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);

    size_t len = FCRYPTO_KEY_LEN;
    bool have = false;
    if (err == ESP_OK && nvs_get_blob(h, "netkey", s_key, &len) == ESP_OK
        && len == FCRYPTO_KEY_LEN) {
        have = true;
    }

    if (!have) {
        esp_fill_random(s_key, FCRYPTO_KEY_LEN);
        if (err == ESP_OK) {
            nvs_set_blob(h, "netkey", s_key, FCRYPTO_KEY_LEN);
            nvs_commit(h);
        }
        ESP_LOGI(TAG, "generated a random network key, unprovisioned boards "
                      "cannot talk to anyone until they share one");
    }

    if (err == ESP_OK) nvs_close(h);

    s_loaded = true;
    memcpy(key, s_key, FCRYPTO_KEY_LEN);
}

void netkey_set_passphrase(const char *passphrase, size_t len) {
    fcrypto_derive_key(passphrase, len, s_key);
    s_loaded = true;
    store_and_log("from a passphrase");
}

void netkey_set_random(void) {
    esp_fill_random(s_key, FCRYPTO_KEY_LEN);
    s_loaded = true;
    store_and_log("to a fresh random value");
}

void netkey_set_raw(const uint8_t key[FCRYPTO_KEY_LEN]) {
    memcpy(s_key, key, FCRYPTO_KEY_LEN);
    s_loaded = true;
    store_and_log("received from a confirmed contact");
}

void netkey_fingerprint(char *out, size_t cap) {
    uint8_t key[FCRYPTO_KEY_LEN];
    if (!s_loaded) netkey_load(key);
    else memcpy(key, s_key, FCRYPTO_KEY_LEN);
    fcrypto_key_fingerprint(key, out, cap);
}
