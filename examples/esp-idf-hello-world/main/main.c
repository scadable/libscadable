// Hello world for libscadable on ESP32.
// SCADABLE 2026 · Apache-2.0

#define SCADABLE_NO_GENERATED  // standalone build — no codegen header
#include "scadable.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "hello-world";

// Bring up WiFi station mode using credentials stored in NVS namespace
// `wifi` (set by your provisioning flow).
static void wifi_up(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi started; expects creds in NVS namespace 'wifi'");
}

void app_main(void) {
    wifi_up();
    scadable_init(NULL);
    scadable_connect();
    scadable_publish(0, "world", 5, 1);  // SCADABLE_CH_HELLO == 0 in standalone mode
    ESP_LOGI(TAG, "scadable hello-world running");
}
