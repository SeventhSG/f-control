#include "provision.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "netkey.h"
#include "wifi.h"

static const char *TAG = "provision";

#define LINE_MAX  512
#define MARKER    "FCPROV "

static ident_t *s_id;

static const char *str_field(const cJSON *root, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void apply(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        printf("FCPROV-ERR bad json\n");
        return;
    }

    bool touched = false;

    const char *name = str_field(root, "name");
    if (name && name[0] && ident_set_name(s_id, name)) {
        ESP_LOGI(TAG, "name set to \"%s\"", name);
        touched = true;
    }

    const char *ap = str_field(root, "ap");
    if (ap && ident_set_ap_name(ap)) {
        ESP_LOGI(TAG, "access point name %s", ap[0] ? "overridden" : "reset to default");
        touched = true;
    }

    const char *ssid = str_field(root, "ssid");
    if (ssid && ssid[0]) {
        const char *pass = str_field(root, "pass");
        wifi_join(ssid, pass ? pass : "");
        ESP_LOGI(TAG, "network \"%s\" stored", ssid);
        touched = true;
    }

    const char *netkey = str_field(root, "netkey");
    if (netkey && netkey[0]) {
        netkey_set_passphrase(netkey, strlen(netkey));
        touched = true;
    }

    cJSON_Delete(root);

    if (!touched) {
        printf("FCPROV-ERR nothing to apply\n");
        return;
    }

    printf("FCPROV-OK\n");
    fflush(stdout);
    ESP_LOGI(TAG, "provisioned, restarting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void provision_task(void *arg) {
    (void)arg;

    char line[LINE_MAX];
    size_t n = 0;

    for (;;) {
        const int c = getchar();
        if (c == EOF) {
            /* Nothing waiting right now. This does not busy loop: the default
             * console driver's getchar() already blocks briefly, but a short
             * yield here keeps this task from ever being the one that starves
             * lower priority work if that assumption is ever wrong. */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (n == 0) continue;
            line[n] = '\0';

            if (n > sizeof(MARKER) - 1 && strncmp(line, MARKER, sizeof(MARKER) - 1) == 0) {
                apply(line + sizeof(MARKER) - 1);
            }
            n = 0;
            continue;
        }

        /* A line that will never match the marker, most of them, since this
         * UART also carries every ESP_LOG line the board prints on its own.
         * Just keep discarding until the next newline rather than growing
         * past the buffer. */
        if (n < sizeof(line) - 1) line[n++] = (char)c;
    }
}

void provision_start(ident_t *id) {
    s_id = id;
    xTaskCreate(provision_task, "fc_prov", 4096, NULL, 3, NULL);
}
