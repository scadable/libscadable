// scadable_generated.h — PLACEHOLDER for standalone builds.
// SCADABLE 2026 · Apache-2.0
//
// In production, the SCADABLE build pipeline overwrites this file with one
// generated from the customer's .scadable/config.toml + digital-twin/ +
// diagnostics/ folders. The generated version contains typed enums for
// channels/metrics/tests + baked-in broker URL + namespace ID + build
// provenance.
//
// This placeholder lets standalone builds (examples, dev environments
// without a SCADABLE namespace) compile without errors. It defines the
// minimum identifiers needed to satisfy includes.

#ifndef SCADABLE_GENERATED_H
#define SCADABLE_GENERATED_H

#include <stdint.h>

typedef uint32_t scadable_channel_t;
typedef uint32_t scadable_metric_t;
typedef uint32_t scadable_test_t;

#define SCADABLE_CH_HELLO     ((scadable_channel_t)0)
#define SCADABLE_CH_TELEMETRY ((scadable_channel_t)1)
#define SCADABLE_CH_EVENTS    ((scadable_channel_t)2)

#define SCADABLE_BROKER_URL    "mqtts://io.scadable.com:8883"
#define SCADABLE_NAMESPACE_ID  "ns_unset"
#define SCADABLE_TARGET        "unset"
#define SCADABLE_BUILD_VERSION "v0.0.0-standalone"
#define SCADABLE_BUILD_SHA     "unknown"

#endif  // SCADABLE_GENERATED_H
