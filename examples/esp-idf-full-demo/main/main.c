// Full-surface demo for libscadable on ESP32.
// SCADABLE 2026 · Apache-2.0
//
// Exercises every public API in include/scadable.h. Use this as a reference
// when integrating libscadable into your own firmware.
//
// Flash + run:
//   idf.py set-target esp32-s3
//   idf.py build flash monitor
//
// Expected behavior on real hardware:
//   - wifi connects (uses NVS-stored creds in namespace `wifi`)
//   - scadable_init() reads device cert from NVS namespace `scadable_certs`
//   - scadable_connect() establishes mTLS MQTT to io.scadable.com:8883
//   - main loop publishes telemetry every 5 s + a heartbeat log every 30 s
//   - cloud-side OTA notify triggers download + verify + reboot automatically
//   - cloud-side env_vars change triggers refresh + per-key callback fires

#define SCADABLE_NO_GENERATED  // standalone build — no codegen header
#include "scadable.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "full-demo";

// ────────────────────────────────────────────────────────────────────────────
// Diagnostics — gated by OTA: if any FAIL, the next OTA notify will be
// rejected until the diagnostic clears.
// ────────────────────────────────────────────────────────────────────────────

static SCADABLE_TEST(check_wifi_connected, ctx) {
    TEST_LOG(ctx, "checking wifi link");
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return TEST_FAIL("wifi not associated");
    }
    return TEST_PASS("rssi=%d", ap.rssi);
}

static SCADABLE_TEST(check_heap_healthy, ctx) {
    size_t free_heap = esp_get_free_heap_size();
    TEST_LOG(ctx, "free_heap=%u", (unsigned)free_heap);
    if (free_heap < 16 * 1024) {
        return TEST_FAIL("low heap: %u bytes free", (unsigned)free_heap);
    }
    if (free_heap < 64 * 1024) {
        return TEST_PASS_WITH_WARN("heap getting tight: %u bytes", (unsigned)free_heap);
    }
    return TEST_PASS("heap=%u bytes", (unsigned)free_heap);
}

// ────────────────────────────────────────────────────────────────────────────
// Event callback — fires on the library's internal MQTT task. Keep fast.
// ────────────────────────────────────────────────────────────────────────────

static void on_scd_event(scadable_event_t e, void *user) {
    (void)user;
    switch (e.type) {
    case SCADABLE_EVT_CONNECTED:
        SCADABLE_LOG_INFO("MQTT connected (recovered_count=%u)", e.connected.recovered_count);
        break;
    case SCADABLE_EVT_DISCONNECTED:
        SCADABLE_LOG_WARN("MQTT disconnected");
        break;
    case SCADABLE_EVT_PUBLISHED:
        // Every QoS1 PUBACK lands here. Useful as a backpressure signal.
        break;
    case SCADABLE_EVT_ERROR:
        SCADABLE_LOG_ERROR("MQTT error code=%d esp_tls_err=%d retriable=%d", e.error.code,
                           (int)e.error.esp_tls_err, e.error.retriable);
        break;
    case SCADABLE_EVT_OTA_AVAILABLE:
        SCADABLE_LOG_INFO("OTA in progress: version=%s", e.ota_available.new_version);
        break;
    case SCADABLE_EVT_ENV_CHANGED:
        SCADABLE_LOG_INFO("env table refreshed");
        break;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Per-key env-change callback (fires once per key after every refresh).
// ────────────────────────────────────────────────────────────────────────────

static void on_env_change(const char *key, const char *value, void *user) {
    (void)user;
    SCADABLE_LOG_INFO("env: %s=%s", key, value);
}

// ────────────────────────────────────────────────────────────────────────────
// WiFi bringup — stays out of libscadable's way.
// ────────────────────────────────────────────────────────────────────────────

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

// ────────────────────────────────────────────────────────────────────────────
// app_main — exercises every public API from include/scadable.h.
// ────────────────────────────────────────────────────────────────────────────

void app_main(void) {
    wifi_up();

    // Lifecycle.
    scadable_config_t cfg = {
        .keepalive_secs = 30,
        .log_batch_secs = 5,
    };
    scadable_err_t e = scadable_init(&cfg);
    if (e != SCADABLE_OK) {
        ESP_LOGE(TAG, "scadable_init failed: %s", scadable_strerror(e));
        return;
    }
    scadable_on_event(on_scd_event, NULL);
    scadable_on_env_change(on_env_change, NULL);

    // Diagnostics — register before connect so the OTA gate has them ready.
    scadable_register_test_("wifi_connected", check_wifi_connected);
    scadable_register_test_("heap_healthy", check_heap_healthy);

    e = scadable_connect();
    if (e != SCADABLE_OK) {
        ESP_LOGE(TAG, "scadable_connect failed: %s", scadable_strerror(e));
        return;
    }

    // State + connected query.
    SCADABLE_LOG_INFO("scadable_state=%d, is_connected=%d", (int)scadable_state(),
                      (int)scadable_is_connected());

    // Read env / secrets (returns fallbacks until the cloud responds).
    const char *region = scadable_env_get_or("REGION", "us-east");
    int32_t port       = scadable_env_get_int("PORT", 8080);
    bool flag          = scadable_env_get_bool("FEATURE_X", false);
    double threshold   = scadable_env_get_double("TEMP_THRESHOLD", 25.0);
    const char *secret = scadable_secret_get_or("API_KEY", "<unset>");
    SCADABLE_LOG_INFO("env: region=%s port=%ld flag=%d thresh=%.2f api_key_len=%u", region,
                      (long)port, (int)flag, threshold, (unsigned)(secret ? strlen(secret) : 0));

    // Main loop: publish telemetry + heartbeat. Any blocking operation in
    // libscadable runs on its own task — this loop never stalls.
    uint32_t tick = 0;
    while (true) {
        // Discrete-channel publish (raw bytes, customer-controlled framing).
        const char *payload = "ping";
        scadable_publish(0 /* SCADABLE_CH_HELLO */, payload, strlen(payload), /*qos=*/1);

        // Typed metrics (build pipeline emits enum names; standalone uses ints).
        scadable_metric_set_u32(0 /* SCADABLE_METRIC_TICK */, tick);
        scadable_metric_set_f64(1 /* SCADABLE_METRIC_FREE_HEAP */,
                                (double)esp_get_free_heap_size());

        if ((tick % 6) == 0) {
            SCADABLE_LOG_INFO("heartbeat tick=%u", (unsigned)tick);
        }

        // Demo: every 60 ticks (~5 min) announce a 30 s offline window so the
        // dashboard doesn't fire an alert when we briefly drop. Real Verdant-
        // style devices would call this right before deep_sleep.
        if (tick > 0 && (tick % 60) == 0) {
            scadable_announce_offline(30);
            scadable_flush(2000);  // wait for QoS1 PUBACKs before going dark
            // Real devices: esp_deep_sleep_start() here.
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // Unreachable in this demo; shown for completeness.
    scadable_disconnect();
}
