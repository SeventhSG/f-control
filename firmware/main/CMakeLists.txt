idf_component_register(SRCS "main.c" "ident.c" "net.c" "web.c" "wifi.c" "netkey.c" "provision.c" "contacts.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "index.html.gz"
                       REQUIRES fcp fmesh fcrypto nvs_flash esp_wifi esp_http_server esp_timer esp_netif esp_event json)
