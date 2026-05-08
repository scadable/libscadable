// telemetry.c — typed metric setters.
// SCADABLE 2026 · Apache-2.0
//
// Each set publishes a small JSON envelope on `{ns}/{gw}/metrics/m{id}`. The
// cloud's NATS bridge strips the slash topic and republishes on
// `events.{ns}.{gw}.data.m{id}` for the historian.
//
// Wire format (stable v1):
//   {"v":1,"ts_ms":1714867200123,"value":<number>}
//
// Rate limiting + batching are deferred — the v0.1 cut publishes inline. A
// future v0.2 will read [telemetry.rates] from baked-in config and coalesce.

#include "internal.h"
#include "scadable.h"

#include <inttypes.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#    include "esp_timer.h"
static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}
#else
#    include <time.h>
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

static scadable_err_t publish_metric(scadable_metric_t metric, const char *value_json) {
    char topic[160];
    if (scd_topic_metric(topic, sizeof(topic), (uint32_t)metric) < 0) {
        return SCADABLE_ERR_INTERNAL;
    }
    char body[128];
    int n = snprintf(body, sizeof(body), "{\"v\":1,\"ts_ms\":%" PRId64 ",\"value\":%s}", now_ms(),
                     value_json);
    if (n <= 0 || (size_t)n >= sizeof(body)) return SCADABLE_ERR_INTERNAL;
    return scd_mqtt_publish(topic, body, (size_t)n, 0, /*retain=*/false);
}

scadable_err_t scadable_metric_set_u32(scadable_metric_t metric, uint32_t value) {
    char numbuf[16];
    snprintf(numbuf, sizeof(numbuf), "%u", (unsigned)value);
    return publish_metric(metric, numbuf);
}

scadable_err_t scadable_metric_set_f64(scadable_metric_t metric, double value) {
    char numbuf[32];
    // %g chops trailing zeros and switches to scientific when it would help —
    // ideal for telemetry where the value's significance varies wildly.
    snprintf(numbuf, sizeof(numbuf), "%.17g", value);
    return publish_metric(metric, numbuf);
}
