idf_component_register(
    SRCS
        "main.c"
        "ap_mode.c"
        "sta_mode.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "captive_portal.html"
    REQUIRES
        libscadable
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        lwip
        app_update
)
