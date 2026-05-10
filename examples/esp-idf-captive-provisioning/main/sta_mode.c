// STA-mode bringup for the SCADABLE provisioning firmware.
// SCADABLE 2026 · Apache-2.0
//
// After `app_main` finds WiFi creds in NVS, it calls into here. We:
//   1. Configure WiFi as STA with the supplied SSID + password.
//   2. Wait for IP_EVENT_STA_GOT_IP (via FreeRTOS event group).
//   3. Hand off to libscadable: scadable_init + scadable_connect.
//   4. Idle forever — the customer firmware will OTA-replace us via
//      the ota_0/ota_1 partitions defined in partitions.csv.
//
// On 5 consecutive disconnects without ever getting an IP, we erase
// the NVS creds and reboot — that recovers from typo'd passwords by
// dropping back into AP mode.

#include "sta_mode.h"

#define SCADABLE_NO_GENERATED  // standalone build — no codegen header
#include "scadable.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "sta-mode";

#define BIT_GOT_IP        (1 << 0)
#define BIT_GAVE_UP       (1 << 1)
#define MAX_RETRIES_BEFORE_FALLBACK 5

static EventGroupHandle_t s_wifi_events;
static int s_retry_count = 0;
static bool s_ever_got_ip = false;

// Erase NVS namespace `wifi` and reboot. Called when we conclude the
// stored creds are bad (5 disconnects without ever reaching GOT_IP).
// Reboot lands back in app_main → no creds → AP mode.
static void wipe_creds_and_reboot(void) {
    ESP_LOGW(TAG, "wiping bad WiFi creds and rebooting into AP mode");
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_ever_got_ip) {
            s_retry_count++;
            ESP_LOGW(TAG, "wifi disconnected (retry %d/%d)",
                     s_retry_count, MAX_RETRIES_BEFORE_FALLBACK);
            if (s_retry_count >= MAX_RETRIES_BEFORE_FALLBACK) {
                xEventGroupSetBits(s_wifi_events, BIT_GAVE_UP);
                return;
            }
        } else {
            // Already had an IP once — this is a transient disconnect.
            // Don't reset s_retry_count or the typo-recovery logic, just
            // rejoin and let libscadable's MQTT reconnect handle it.
            ESP_LOGW(TAG, "wifi disconnected after IP — reconnecting");
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_ever_got_ip = true;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, BIT_GOT_IP);
    }
}

void sta_mode_start(const char *ssid, const char *password) {
    s_wifi_events = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    // WPA2_PSK by default; ESP-IDF auto-falls-back to OPEN if the AP is
    // unsecured, so this works for both home + office networks.
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for either GOT_IP (success) or GAVE_UP (5 retries → wipe + reboot).
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, BIT_GOT_IP | BIT_GAVE_UP, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & BIT_GAVE_UP) {
        wipe_creds_and_reboot();
        return;
    }

    // We have an IP. Hand off to libscadable. The cert is expected to
    // already be in NVS namespace `scadable_certs` (keys `device_cert`
    // and `device_key`), written by the dashboard's WebSerial flasher
    // during the Provisioning-mode flow.
    scadable_err_t e = scadable_init(NULL);
    if (e != SCADABLE_OK) {
        ESP_LOGE(TAG, "scadable_init failed: %s — sleeping 60s before reboot",
                 scadable_strerror(e));
        vTaskDelay(pdMS_TO_TICKS(60000));
        esp_restart();
    }
    e = scadable_connect();
    if (e != SCADABLE_OK) {
        ESP_LOGE(TAG, "scadable_connect failed: %s — sleeping 60s before reboot",
                 scadable_strerror(e));
        vTaskDelay(pdMS_TO_TICKS(60000));
        esp_restart();
    }

    ESP_LOGI(TAG, "scadable connected — waiting for customer firmware OTA");

    // Block forever. libscadable's MQTT task runs independently and will
    // accept the OTA notify message that the cloud sends once the
    // customer firmware release is published. On OTA apply, the chip
    // reboots into the new firmware.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
