#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "ident.h"
#include "net.h"
#include "web.h"

static const char *TAG = "f-control";

static ident_t s_id;

static void banner(void) {
    char words[80];
    char ssid[32];
    ident_words(&s_id, words, sizeof words);
    ident_ap_name(&s_id, ssid, sizeof ssid);

    printf("\n");
    printf("f-control  v0.1.0-dev\n");
    printf("fingerprint   %s\n", words);
    printf("access point  %s, open, channel %u\n", ssid, NET_CHANNEL);
    printf("dashboard     http://192.168.4.1\n");
    printf("\n");
    printf("  THIS BUILD HAS NO ENCRYPTION.\n");
    printf("  Messages travel in clear over the air and anyone with a\n");
    printf("  receiver can read them. Identities are not signed, so anyone\n");
    printf("  can claim to be anyone. This is a hardware bring-up build,\n");
    printf("  not something to trust with anything that matters.\n");
    printf("\n");
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ident_load(&s_id);

    /* The access point has to be up before ESP-NOW, because ESP-NOW rides on
     * the same radio and inherits its channel. */
    web_start(&s_id);
    net_start(&s_id, web_on_message, web_send_roster);

    banner();
    ESP_LOGI(TAG, "running");
}
