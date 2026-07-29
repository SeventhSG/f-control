idf_component_register(SRCS "main.c" "ident.c" "net.c" "web.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "index.html.gz"
                       REQUIRES fcp fmesh nvs_flash esp_wifi esp_http_server esp_timer json)
