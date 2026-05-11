// internal.h — shared state + helpers for libscadable.
// SCADABLE 2026 · Apache-2.0
//
// NOT part of the public API. Customer code must NEVER include this — only
// the .c files inside src/ are allowed to.

#ifndef SCADABLE_INTERNAL_H
#define SCADABLE_INTERNAL_H

#include "scadable.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Build-time injected provisioning (overridden by scadable_generated.c) ────
//
// The factory provisioner generates a `scadable_generated.c` that defines
// these as strong symbols. For standalone builds (no provisioner) the weak
// defaults below + NVS-backed values keep things working.

const char *scadable_gen_broker_url(void);
const char *scadable_gen_namespace_id(void);
const char *scadable_gen_target(void);
const char *scadable_gen_build_version(void);
const char *scadable_gen_build_sha(void);

// ─── State accessors ─────────────────────────────────────────────────────────

scadable_state_t scd_state_get(void);
void scd_state_set(scadable_state_t s);

// Returns NULL until init succeeds. Stable pointer for the process lifetime.
const char *scd_gateway_id(void);
const char *scd_namespace_id(void);
const char *scd_broker_url(void);

// MQTT handle accessor — opaque to non-mqtt callers. Returns NULL if not yet
// connected. Real type is esp_mqtt_client_handle_t on ESP-IDF builds; void *
// in the host-side test stub so unit tests can compile without esp-idf.
void *scd_mqtt_handle(void);
void scd_mqtt_handle_set(void *h);

// Counts pending QoS1 publishes — incremented before mqtt_publish, decremented
// in the on-PUBACK event handler. scadable_flush() blocks on this reaching 0.
void scd_pending_inc(void);
void scd_pending_dec(int32_t msg_id);
uint32_t scd_pending_count(void);

// Fire user callback from the internal MQTT task. Safe to call from any task —
// internally serializes via the global event-cb mutex.
void scd_emit_event(const scadable_event_t *evt);

// ─── Topic helpers ──────────────────────────────────────────────────────────
//
// Format mirrors gateway-linux + gateway-esp so the cloud-side router doesn't
// special-case libscadable: `{namespace}/{gateway}/...`.
//
// out_size MUST be at least 256. Returns negative on overflow.

int scd_topic_data(char *out, size_t out_size, uint32_t channel);
int scd_topic_metric(char *out, size_t out_size, uint32_t metric);
int scd_topic_logs_batch(char *out, size_t out_size);
int scd_topic_diag_result(char *out, size_t out_size);
int scd_topic_presence(char *out, size_t out_size);
int scd_topic_offline_announce(char *out, size_t out_size);
int scd_topic_subscribe_ota(char *out, size_t out_size);
int scd_topic_subscribe_env(char *out, size_t out_size);
int scd_topic_subscribe_cmd(char *out, size_t out_size);
int scd_topic_ota_progress(char *out, size_t out_size);

// Returns true when topic is exactly the OTA notify subscription for this gw.
bool scd_topic_is_ota_notify(const char *topic);
bool scd_topic_is_env_change(const char *topic);

// v0.3.0: cmd-topic dispatch.
//
// Topic shape for downlink commands is `{ns}/{gw}/cmd/{type}`; this helper
// returns true if the topic matches that shape and writes a heap-allocated
// type string to *cmd_type_out (caller frees). Returns false (and leaves
// *cmd_type_out untouched) for any non-cmd topic.
bool scd_topic_is_cmd(const char *topic, char **cmd_type_out);

// Dispatch a cmd payload to the right handler based on cmd_type. Currently
// recognized: "diagnostic.run", "diagnostic.run_all". Unknown types are
// logged at DEBUG and dropped (forward-compatible — newer cloud, older chip).
void scd_cmd_dispatch(const char *cmd_type, const char *body, size_t len);

// ─── Logging subsystem ──────────────────────────────────────────────────────
//
// Customer-facing API is the SCADABLE_LOG_* macros in <scadable.h>. The
// implementation feeds into a bounded ring buffer that a flusher task drains
// every `log_batch_secs` (default 5; 0 = realtime, no batching).

void scd_log_init(uint16_t batch_secs);
void scd_log_shutdown(void);
void scd_log_flush_blocking(uint32_t timeout_ms);

// ─── Env vars + secrets ─────────────────────────────────────────────────────

void scd_env_init(void);
void scd_env_shutdown(void);
// Refresh from edge (HTTPS GET). Called at startup + when EVT_ENV_CHANGED MQTT
// nudge arrives. Returns 0 on success, negative on error.
int scd_env_refresh_blocking(void);

// ─── Diagnostics ────────────────────────────────────────────────────────────

void scd_diag_init(void);
void scd_diag_shutdown(void);
// Run every registered test, publish results to MQTT, return overall status.
// Used by OTA gate (refuse to swap if any diag fails).
bool scd_diag_run_all_blocking(void);

// ─── Publish primitive ──────────────────────────────────────────────────────
//
// Wraps esp_mqtt_client_enqueue with bookkeeping: increments pending counter
// for QoS1, returns BACKPRESSURE if outbound buffer full. Topic is borrowed
// (owned by caller); payload is copied internally by esp-mqtt.

scadable_err_t
scd_mqtt_publish(const char *topic, const void *payload, size_t len, uint8_t qos, bool retain);

// ─── OTA ────────────────────────────────────────────────────────────────────

void scd_ota_init(void);
void scd_ota_handle_notify(const char *payload, size_t len);

// ─── NVS wrappers ───────────────────────────────────────────────────────────
//
// Two namespaces:
//   "scadable_cfg"   — gateway_id, namespace override, log_batch_secs override
//   "scadable_certs" — device cert PEM, private key PEM, ca cert PEM
//                     (encrypted at rest via flash encryption when enabled)

int scd_nvs_get_str(const char *ns, const char *key, char *out, size_t out_size);
int scd_nvs_get_blob(const char *ns, const char *key, void *out, size_t *out_size);
int scd_nvs_set_str(const char *ns, const char *key, const char *value);

// Loads device cert + key from "scadable_certs" NVS namespace. Returns
// SCADABLE_ERR_NO_CERT if either is missing.
//
// On ESP-IDF the buffers are heap-allocated and the caller owns them.
//
// CA verification is NOT loaded here — the broker (io.scadable.com) is
// fronted by a public-trust LE leaf as of libscadable 0.2.0, so all
// outbound TLS uses esp_crt_bundle_attach (the Mozilla root bundle baked
// into mbedTLS via CONFIG_MBEDTLS_CERTIFICATE_BUNDLE). The previous
// per-namespace `ca_cert` NVS key is no longer read; older provisioning
// bundles that wrote it remain compatible (the key is ignored, NVS space
// is freed when the bundle is overwritten).
scadable_err_t scd_load_certs(char **cert_pem_out, char **key_pem_out);

// ─── Defaults (overridden by scadable_config_t at init time) ────────────────

#define SCD_DEFAULT_KEEPALIVE_SECS 30
#define SCD_DEFAULT_LOG_BATCH_SECS 5
#define SCD_LOG_RING_CAP           128  // records; ~16 KB worst-case heap

#ifdef __cplusplus
}
#endif
#endif  // SCADABLE_INTERNAL_H
