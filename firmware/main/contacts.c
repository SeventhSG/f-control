#include "contacts.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "contacts";
static const char *NS = "fc_contacts";

/* Stored as one blob: CONTACTS_MAX fixed width entries, each FCP_ID_LEN bytes,
 * unused entries zero filled. A real database is overkill for 64 entries that
 * change rarely, and a single blob read/write keeps this file small. */
#define BLOB_LEN (CONTACTS_MAX * FCP_ID_LEN)

static bool s_loaded;
static uint8_t s_ids[CONTACTS_MAX][FCP_ID_LEN];
static size_t  s_count;

static bool is_zero(const uint8_t id[FCP_ID_LEN]) {
    for (int i = 0; i < FCP_ID_LEN; i++) if (id[i] != 0) return false;
    return true;
}

static void load(void) {
    if (s_loaded) return;
    s_loaded = true;
    s_count = 0;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t blob[BLOB_LEN];
    size_t len = sizeof blob;
    if (nvs_get_blob(h, "ids", blob, &len) == ESP_OK && len == BLOB_LEN) {
        for (size_t i = 0; i < CONTACTS_MAX; i++) {
            const uint8_t *id = blob + i * FCP_ID_LEN;
            if (is_zero(id)) continue;
            memcpy(s_ids[s_count++], id, FCP_ID_LEN);
        }
        ESP_LOGI(TAG, "loaded %u confirmed contacts", (unsigned)s_count);
    }
    nvs_close(h);
}

static bool save(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;

    uint8_t blob[BLOB_LEN];
    memset(blob, 0, sizeof blob);
    for (size_t i = 0; i < s_count; i++) memcpy(blob + i * FCP_ID_LEN, s_ids[i], FCP_ID_LEN);

    const bool ok = nvs_set_blob(h, "ids", blob, sizeof blob) == ESP_OK
                  && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool contacts_is_confirmed(const uint8_t id[FCP_ID_LEN]) {
    load();
    for (size_t i = 0; i < s_count; i++) {
        if (memcmp(s_ids[i], id, FCP_ID_LEN) == 0) return true;
    }
    return false;
}

bool contacts_confirm(const uint8_t id[FCP_ID_LEN]) {
    load();

    if (contacts_is_confirmed(id)) return true;
    if (s_count >= CONTACTS_MAX) {
        ESP_LOGW(TAG, "contact list full at %u, cannot confirm another", (unsigned)CONTACTS_MAX);
        return false;
    }

    memcpy(s_ids[s_count++], id, FCP_ID_LEN);
    return save();
}
