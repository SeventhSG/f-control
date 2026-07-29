#include "ident.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ident";
static const char *NS = "fcontrol";

/* Sixty four short, unambiguous words. Four of them carry 24 bits, which is
 * enough for two people to notice a mismatch when they read them aloud and far
 * too few to resist an attacker who is grinding for a collision. That is
 * acceptable here only because this build has no cryptography to protect in
 * the first place. When real keys land, this list grows and the fingerprint
 * covers a hash of the public key rather than the key itself. */
static const char *WORDS[64] = {
    "amber", "anchor", "apple", "arrow", "autumn", "basil", "beacon", "birch",
    "bison", "bramble", "brass", "cedar", "chalk", "cinder", "clay", "clover",
    "copper", "coral", "cotton", "crane", "dawn", "delta", "ember", "fern",
    "flint", "forge", "fossil", "garnet", "granite", "harbor", "hazel", "heron",
    "horse", "indigo", "ivory", "juniper", "kelp", "lantern", "linen", "maple",
    "marble", "meadow", "mica", "nettle", "nickel", "oak", "onyx", "otter",
    "pewter", "pine", "quartz", "quiet", "raven", "river", "saffron", "slate",
    "sparrow", "still", "stone", "thistle", "timber", "umber", "walnut", "willow",
};

void ident_load(ident_t *out) {
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    }

    size_t len = IDENT_KEY_LEN;
    bool have = false;
    if (err == ESP_OK && nvs_get_blob(h, "key", out->key, &len) == ESP_OK
        && len == IDENT_KEY_LEN) {
        have = true;
    }

    if (!have) {
        /* esp_random is only a true hardware RNG once the radio is running,
         * which it is by the time this is called. */
        esp_fill_random(out->key, IDENT_KEY_LEN);
        if (err == ESP_OK) {
            nvs_set_blob(h, "key", out->key, IDENT_KEY_LEN);
            nvs_commit(h);
        }
        ESP_LOGI(TAG, "generated a new identity");
    }

    /* The key is uniformly random, so its leading bytes are as good an
     * identifier as a hash of it would be. This becomes a real hash when the
     * key becomes a real public key. */
    memcpy(out->id, out->key, FCP_ID_LEN);

    len = sizeof(out->name);
    if (err != ESP_OK || nvs_get_str(h, "name", out->name, &len) != ESP_OK) {
        out->name[0] = '\0';
    }

    if (err == ESP_OK) nvs_close(h);

    out->boot_id = (uint16_t)(esp_random() & 0xFFFFu);
    out->seq = 0;
}

bool ident_set_name(ident_t *id, const char *name) {
    if (name == NULL) return false;
    const size_t n = strlen(name);
    if (n > IDENT_NAME_MAX) return false;

    memset(id->name, 0, sizeof(id->name));
    memcpy(id->name, name, n);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, "name", id->name) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void ident_words_of(const uint8_t id[FCP_ID_LEN], char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';

    size_t used = 0;
    for (int i = 0; i < IDENT_WORDS; i++) {
        const char *w = WORDS[id[i] & 0x3Fu];
        const int n = snprintf(out + used, cap - used, "%s%s", i ? " " : "", w);
        if (n < 0 || (size_t)n >= cap - used) return;
        used += (size_t)n;
    }
}

void ident_words(const ident_t *id, char *out, size_t cap) {
    ident_words_of(id->id, out, cap);
}

void ident_ap_name(const ident_t *id, char *out, size_t cap) {
    snprintf(out, cap, "f-control-%02x%02x", id->id[0], id->id[1]);
}
