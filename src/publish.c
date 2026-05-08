// publish.c — publish + flush + announce_offline.
// SCADABLE 2026 · Apache-2.0
// STUB IMPLEMENTATION — wired up in Sprint 1 lane C.

#include "scadable.h"

scadable_err_t scadable_publish(scadable_channel_t channel,
                                const void *data, size_t len, uint8_t qos) {
    (void)channel; (void)data; (void)len; (void)qos;
    if (!scadable_is_connected()) return SCADABLE_ERR_NOT_CONNECTED;
    // TODO: build MQTT topic from baked-in topic_prefix + channel name lookup.
    // TODO: enqueue to outbound queue; return BACKPRESSURE if full.
    return SCADABLE_OK;
}

scadable_err_t scadable_flush(uint32_t timeout_ms) {
    (void)timeout_ms;
    // TODO: block until pending QoS1 PUBACKs land or timeout fires.
    // The most-asked ESP32 forum question — must work right before we ship.
    return SCADABLE_OK;
}

scadable_err_t scadable_announce_offline(uint32_t expected_offline_secs) {
    (void)expected_offline_secs;
    // TODO: publish to a reserved subject `sys.{ns}.{gw}.offline.scheduled`
    // with payload {expected_offline_secs} so service-orchestrator updates
    // last_announced_offline_until and suppresses offline alerts.
    return SCADABLE_OK;
}
