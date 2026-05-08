# libscadable as an ESP-IDF component.
# SCADABLE 2026 · Apache-2.0

idf_component_register(
    SRCS
        "src/lifecycle.c"
        "src/publish.c"
        "src/telemetry.c"
        "src/logging.c"
        "src/env.c"
        "src/diagnostics.c"
        "src/ota.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        mqtt
        nvs_flash
        esp_http_client
        esp_https_ota
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        esp_partition
        esp-tls
        app_update
        mbedtls
)
