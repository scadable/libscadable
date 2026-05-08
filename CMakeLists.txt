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
    INCLUDE_DIRS
        "include"
    REQUIRES
        mqtt
        nvs_flash
        esp_http_client
        esp_https_ota
        esp_wifi
        mbedtls
)
