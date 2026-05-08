// publish.c — publish + flush + announce_offline.
// SCADABLE 2026 · Apache-2.0
//
// Wraps esp_mqtt_client_enqueue with QoS1 PUBACK accounting so scadable_flush()
// can deterministically wait for outstanding messages before deep sleep — the
// most-asked ESP32-MQTT forum question. Without it, a sequence like
//
//     scadable_publish(...);
//     esp_deep_sleep_start();
//
// loses the message half the time on Verdant-style scheduled-online devices.

#include "internal.h"
#include "scadable.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#    include "esp_log.h"
#    include "freertos/FreeRTOS.h"
#    include "freertos/task.h"
#    include "mqtt_client.h"
#endif

static const char *TAG __attribute__((unused)) = "scadable.publish";

scadable_err_t
scd_mqtt_publish(const char *topic, const void *payload, size_t len, uint8_t qos, bool retain) {
    if (!topic) return SCADABLE_ERR_INVALID_ARG;
    if (!scadable_is_connected()) return SCADABLE_ERR_NOT_CONNECTED;
#ifdef ESP_PLATFORM
    void *h = scd_mqtt_handle();
    if (!h) return SCADABLE_ERR_NOT_CONNECTED;

    // Use enqueue (non-blocking) so a slow socket can't stall the customer's
    // task. esp_mqtt_client_enqueue returns the message ID for QoS>0 or 0 for
    // QoS0; -1 means the outbound queue is full (BACKPRESSURE).
    int msg_id =
        esp_mqtt_client_enqueue(h, topic, (const char *)payload, (int)len, (int)qos, retain ? 1 : 0,
                                /*store=*/true);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "enqueue failed for %s (queue full)", topic);
        return SCADABLE_ERR_BACKPRESSURE;
    }
    if (qos >= 1) scd_pending_inc();
    return SCADABLE_OK;
#else
    (void)payload;
    (void)len;
    (void)qos;
    (void)retain;
    return SCADABLE_OK;
#endif
}

scadable_err_t
scadable_publish(scadable_channel_t channel, const void *data, size_t len, uint8_t qos) {
    if (!data && len > 0) return SCADABLE_ERR_INVALID_ARG;
    if (qos > 1) return SCADABLE_ERR_INVALID_ARG;
    if (scadable_state() == SCADABLE_STATE_UNINITIALIZED) return SCADABLE_ERR_NOT_CONNECTED;

    char topic[160];
    if (scd_topic_data(topic, sizeof(topic), (uint32_t)channel) < 0) {
        return SCADABLE_ERR_INTERNAL;
    }
    return scd_mqtt_publish(topic, data, len, qos, /*retain=*/false);
}

scadable_err_t scadable_flush(uint32_t timeout_ms) {
    // First flush queued log records — they're the most likely "I just logged
    // an important shutdown event then deep-slept" loss case.
    scd_log_flush_blocking(timeout_ms / 2);

#ifdef ESP_PLATFORM
    // Then poll the QoS1 PUBACK counter. esp-mqtt fires MQTT_EVENT_PUBLISHED
    // on every PUBACK; the lifecycle handler decrements g.pending_qos1.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (scd_pending_count() > 0) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGW("scadable.publish", "flush timed out with %u QoS1 messages still pending",
                     (unsigned)scd_pending_count());
            return SCADABLE_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return SCADABLE_OK;
#else
    (void)timeout_ms;
    return SCADABLE_OK;
#endif
}

scadable_err_t scadable_announce_offline(uint32_t expected_offline_secs) {
    if (!scadable_is_connected()) return SCADABLE_ERR_NOT_CONNECTED;

    char topic[160];
    if (scd_topic_offline_announce(topic, sizeof(topic)) < 0) return SCADABLE_ERR_INTERNAL;

    // Reserved subject (per platform/CONVENTIONS.md): the orchestrator updates
    // last_announced_offline_until on receipt and suppresses offline alerts
    // until the window expires.
    char body[128];
    int n =
        snprintf(body, sizeof(body), "{\"v\":1,\"expected_offline_secs\":%u,\"hostname\":\"%s\"}",
                 (unsigned)expected_offline_secs, scd_gateway_id() ? scd_gateway_id() : "");
    if (n <= 0 || (size_t)n >= sizeof(body)) return SCADABLE_ERR_INTERNAL;

    scadable_err_t err = scd_mqtt_publish(topic, body, (size_t)n, 1, /*retain=*/false);
    if (err != SCADABLE_OK) return err;

    // Block briefly so the announce actually leaves the device before the
    // customer's code calls esp_deep_sleep_start() in the next line.
    return scadable_flush(1000);
}
