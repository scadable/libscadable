// SCADABLE captive-portal provisioning firmware — entry point.
// SCADABLE 2026 · Apache-2.0
//
// Boot decision tree:
//   1. Init NVS.
//   2. Read NVS namespace `wifi`. If `ssid` + `password` are present
//      and non-empty, hand off to STA mode (sta_mode_start). The
//      device joins the user's WiFi, calls scadable_init/connect with
//      the cert already in NVS namespace `scadable_certs`, then idles
//      waiting for the customer firmware to OTA in.
//   3. Otherwise, hand off to AP mode (ap_mode_start). The device
//      broadcasts an open `scadable-XXXX` access point + serves a
//      captive-portal form. When the user submits creds, that handler
//      writes them to NVS and reboots — landing back in step 2.

#include "ap_mode.h"
#include "sta_mode.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "captive-provisioning";

// Read a string from NVS namespace `wifi` into `out`. Returns true if
// the key exists AND the resulting string is non-empty. Treats blank
// strings as "not provisioned" so a wiped NVS that still has a stale
// zero-length key falls through to AP mode rather than looping.
static bool nvs_get_wifi_str(const char *key, char *out, size_t out_len) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) return false;
    return out[0] != '\0';
}

void app_main(void) {
    // NVS init — recover from a partition format mismatch by erasing
    // and re-initing once. This survives the case where a previous
    // firmware version laid out NVS differently.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    // Required before either WiFi mode brings up its netif.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid[33] = {0};
    char password[65] = {0};
    bool have_ssid = nvs_get_wifi_str("ssid", ssid, sizeof(ssid));
    bool have_password = nvs_get_wifi_str("password", password, sizeof(password));

    if (have_ssid && have_password) {
        ESP_LOGI(TAG, "WiFi creds present in NVS (ssid=%s) — starting STA mode", ssid);
        sta_mode_start(ssid, password);
    } else {
        ESP_LOGI(TAG, "No WiFi creds in NVS — starting AP captive portal");
        ap_mode_start();
    }
}
