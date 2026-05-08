// telemetry.c — typed metric setters.
// SCADABLE 2026 · Apache-2.0
// STUB IMPLEMENTATION — wired up in Sprint 1 lane C.

#include "scadable.h"

scadable_err_t scadable_metric_set_u32(scadable_metric_t metric, uint32_t value) {
    (void)metric; (void)value;
    // TODO: rate-limit per [telemetry.rates] from baked-in config.
    // TODO: enqueue to telemetry channel for batched publish.
    return SCADABLE_OK;
}

scadable_err_t scadable_metric_set_f64(scadable_metric_t metric, double value) {
    (void)metric; (void)value;
    return SCADABLE_OK;
}
