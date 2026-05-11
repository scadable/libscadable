// scadable.h — Customer-facing API for libscadable.
// SCADABLE 2026 · Apache-2.0
//
// Quickstart:
//   #include "scadable.h"
//   void app_main(void) {
//       wifi_up();
//       scadable_init(NULL);
//       scadable_connect();
//       scadable_publish(SCADABLE_CH_HELLO, "world", 5, 1);
//   }
//
// Full reference: https://docs.scadable.com/library/esp/api/

#ifndef SCADABLE_H
#define SCADABLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// scadable_generated.h is emitted by the SCADABLE build pipeline from your
// .scadable/config.toml + digital-twin/ + diagnostics/ folders. It defines
// scadable_channel_t, scadable_metric_t, scadable_test_t enums and bakes-in
// broker URL + namespace ID + build provenance.
//
// If you're building this library standalone (without the build pipeline),
// the SCADABLE_NO_GENERATED macro disables the include and you get raw
// uint32_t IDs instead of typed enums.
#ifndef SCADABLE_NO_GENERATED
#    include "scadable_generated.h"
#else
typedef uint32_t scadable_channel_t;
typedef uint32_t scadable_metric_t;
typedef uint32_t scadable_test_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ─── Error codes (errno-style; SCADABLE_OK == 0) ────────────────
typedef enum {
    SCADABLE_OK                  = 0,
    SCADABLE_ERR_NOT_INITIALIZED = -1,
    SCADABLE_ERR_NOT_CONNECTED   = -2,
    SCADABLE_ERR_INVALID_ARG     = -3,
    SCADABLE_ERR_BACKPRESSURE    = -4,  // outbound queue full; retry after EVT_PUBLISHED
    SCADABLE_ERR_TIMEOUT         = -5,
    SCADABLE_ERR_NO_CERT         = -6,
    SCADABLE_ERR_TLS             = -7,
    SCADABLE_ERR_NETWORK         = -8,
    SCADABLE_ERR_TEST_FAILED     = -9,  // returned from diagnostic handlers
    SCADABLE_ERR_INTERNAL        = -100,
} scadable_err_t;

/**
 * Translate an scadable_err_t into a human-readable string. The pointer is
 * static — never free. Always returns a non-NULL string, including for
 * unknown error codes (returns "unknown error").
 *
 * Use in log messages:
 *   if ((err = scadable_init(NULL)) != SCADABLE_OK)
 *       ESP_LOGE(TAG, "init failed: %s", scadable_strerror(err));
 */
const char *scadable_strerror(scadable_err_t err);

// ─── Connection state ───────────────────────────────────────────
typedef enum {
    SCADABLE_STATE_UNINITIALIZED = 0,
    SCADABLE_STATE_IDLE,
    SCADABLE_STATE_CONNECTING,
    SCADABLE_STATE_CONNECTED,
    SCADABLE_STATE_DISCONNECTING,
    SCADABLE_STATE_ERROR,
} scadable_state_t;

// ─── Events (fire on library's internal task — do NOT block in callback) ───
typedef enum {
    SCADABLE_EVT_CONNECTED,
    SCADABLE_EVT_DISCONNECTED,
    SCADABLE_EVT_PUBLISHED,  // QoS1 PUBACK landed
    SCADABLE_EVT_ERROR,
    SCADABLE_EVT_OTA_AVAILABLE,  // new firmware ready; library handles apply automatically
    SCADABLE_EVT_ENV_CHANGED,    // env var or secret value updated by cloud
} scadable_event_type_t;

typedef struct {
    scadable_event_type_t type;
    union {
        struct {
            uint32_t recovered_count;
        } connected;
        struct {
            int32_t msg_id;
        } published;
        struct {
            scadable_err_t code;
            int32_t esp_tls_err;
            int32_t mbedtls_err;
            uint32_t cert_verify_flags;
            bool retriable;
        } error;
        struct {
            const char *new_version;
        } ota_available;
        struct {
            const char *key;
            const char *new_value;
        } env_changed;
    };
} scadable_event_t;

typedef void (*scadable_event_cb_t)(scadable_event_t event, void *user);

// ─── Optional runtime overrides (NULL = use baked-in from config.toml) ───
typedef struct {
    const char *broker_url;
    const char *device_id;    // default = MAC-derived
    uint16_t keepalive_secs;  // default 30
    uint16_t log_batch_secs;  // default 5; 0 = realtime
} scadable_config_t;

// ─── Lifecycle ──────────────────────────────────────────────────

/**
 * Initialize library. Reads cert + namespace + baked-in config from NVS.
 * Pass NULL to use everything from .scadable/config.toml. Idempotent.
 *
 * The cert was provisioned at flash time by the SCADABLE web-flasher and
 * stored in NVS namespace `scadable_certs` (encrypted at rest via flash
 * encryption). This call reads it.
 */
scadable_err_t scadable_init(const scadable_config_t *cfg);

/**
 * Begin connecting to broker. Non-blocking; transitions IDLE → CONNECTING.
 * Subscribe to SCADABLE_EVT_CONNECTED via scadable_on_event() to know when
 * ready. Auto-reconnects on transient failures with exponential backoff.
 */
scadable_err_t scadable_connect(void);

/**
 * Graceful disconnect. Sends LWT, flushes pending PUBACKs (up to 2s),
 * stops auto-reconnect. Call before turning off WiFi.
 */
scadable_err_t scadable_disconnect(void);

/**
 * Current state (sync, no IO). Cheap — safe to poll from any task.
 */
scadable_state_t scadable_state(void);

/**
 * Convenience: equivalent to `scadable_state() == SCADABLE_STATE_CONNECTED`.
 * Returns true only when the broker has acknowledged our CONNECT and we're
 * not currently disconnecting.
 */
bool scadable_is_connected(void);

/**
 * Register event callback. Replaces previous callback. Pass NULL to unregister.
 * Callback fires on internal library task — keep it fast, do not block.
 */
void scadable_on_event(scadable_event_cb_t cb, void *user);

// ─── Pre-sleep coordination ─────────────────────────────────────

/**
 * Block until all pending QoS1 publishes land or timeout fires.
 *
 * MUST be called before deep sleep if you've recently published QoS1 —
 * otherwise the message can be lost when the socket goes down with the
 * broker before PUBACK arrives.
 */
scadable_err_t scadable_flush(uint32_t timeout_ms);

/**
 * Tell the cloud "I'm going dark for N seconds, don't alert."
 *
 * For scheduled-online devices (e.g. Verdant pattern: comes online once a
 * day for 5 min, sleeps the rest). Cloud's offline-detection respects the
 * announced window before firing alerts.
 */
scadable_err_t scadable_announce_offline(uint32_t expected_offline_secs);

// ─── Publish ────────────────────────────────────────────────────

/**
 * Publish to a channel. Channel ID must be from generated SCADABLE_CH_*
 * enum (declared in .scadable/config.toml). qos: 0 (fire-forget),
 * 1 (at-least-once).
 *
 * Returns SCADABLE_ERR_BACKPRESSURE if outbound queue is full — retry
 * after a publish completes (SCADABLE_EVT_PUBLISHED fires).
 */
scadable_err_t
scadable_publish(scadable_channel_t channel, const void *data, size_t len, uint8_t qos);

// ─── Telemetry (typed names from .scadable/digital-twin/) ───────

/**
 * Publish a numeric telemetry value on a typed metric channel. The metric ID
 * comes from the SCADABLE_METRIC_* enum your build pipeline emits from
 * .scadable/digital-twin/. Standalone builds use raw uint32_t IDs.
 *
 * Wire format: JSON envelope `{"v":1,"ts_ms":...,"value":<n>}` published
 * QoS0 on `{ns}/{gw}/metrics/m{N}`. Cloud's NATS bridge republishes on
 * `events.{ns}.{gw}.data.m{N}` for the historian.
 *
 * Rate limiting + batching are not yet implemented — every call publishes
 * inline. Don't fire these in a tight loop without throttling yourself.
 *
 * @return SCADABLE_OK, SCADABLE_ERR_NOT_CONNECTED, or SCADABLE_ERR_BACKPRESSURE.
 */
scadable_err_t scadable_metric_set_u32(scadable_metric_t metric, uint32_t value);

/**
 * Same as scadable_metric_set_u32 but for double-precision floats. Encoded
 * with %.17g — round-trips through IEEE 754 cleanly. Use for any value that
 * isn't an integer count: temperatures, voltages, ratios, percentages.
 */
scadable_err_t scadable_metric_set_f64(scadable_metric_t metric, double value);

// ─── Logging (leveled, batched, structured) ─────────────────────

typedef enum {
    SCADABLE_LOG_DEBUG_LEVEL,
    SCADABLE_LOG_INFO_LEVEL,
    SCADABLE_LOG_WARN_LEVEL,
    SCADABLE_LOG_ERROR_LEVEL,
} scadable_log_level_t;

/**
 * Underlying log primitive. Call via the SCADABLE_LOG_* macros below — they
 * fill in __FILE__ + __LINE__ for you. Format string is printf-style; the
 * formatted record is capped at ~224 bytes (longer messages are truncated).
 *
 * Records buffer into a 128-deep ring; a FreeRTOS flusher task drains them
 * every `log_batch_secs` (default 5; set via scadable_config_t). Records also
 * mirror to ESP-IDF's UART logger via ESP_LOG_LEVEL so `idf.py monitor` shows
 * the same lines without waiting for the next batch. Set `log_batch_secs = 0`
 * for realtime behavior (each record published immediately).
 *
 * Safe to call from any task. Pre-init calls fall back to ESP_LOGI() / printf
 * so boot-time log lines aren't lost.
 */
void scadable_log_(scadable_log_level_t lvl, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define SCADABLE_LOG_DEBUG(fmt, ...)                                                               \
    scadable_log_(SCADABLE_LOG_DEBUG_LEVEL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SCADABLE_LOG_INFO(fmt, ...)                                                                \
    scadable_log_(SCADABLE_LOG_INFO_LEVEL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SCADABLE_LOG_WARN(fmt, ...)                                                                \
    scadable_log_(SCADABLE_LOG_WARN_LEVEL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SCADABLE_LOG_ERROR(fmt, ...)                                                               \
    scadable_log_(SCADABLE_LOG_ERROR_LEVEL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// ─── Diagnostics ────────────────────────────────────────────────
//
// v0.3.0 introduces a typed diagnostic system. The legacy SCADABLE_TEST /
// TEST_PASS / TEST_FAIL surface (below) is kept as compile-compatible
// helpers for v0.1/v0.2 customers; new code SHOULD use SCD_DIAG /
// DIAG_PASS / DIAG_FAIL with the typed scadable_diag_result_t struct.
//
// The on-the-wire result envelope is also extended (v2) to carry a
// per-trigger run_id + a "type" hint so the cloud can present the right
// dashboard surface per diagnostic type. Old chips publishing the v1
// envelope continue to be parsed (cloud accepts both).

typedef enum {
    TEST_RESULT_PASS               = 0,
    TEST_RESULT_PASS_WITH_WARN     = 1,
    TEST_RESULT_FAIL               = 2,
    TEST_RESULT_TIMEOUT            = 3,  // v0.3.0: library aborted the fn after timeout_secs
    TEST_RESULT_ERROR              = 4,  // v0.3.0: library couldn't run the fn (not registered, panic, etc.)
    TEST_RESULT_TYPE_NOT_SUPPORTED = 5,  // v0.3.0: cloud asked for a type this firmware can't run
} scadable_test_status_t;

typedef struct {
    scadable_test_status_t status;
    char message[256];     // formatted message, capped to keep RAM small
    uint32_t duration_ms;  // library auto-measures
} scadable_test_result_t;

typedef struct scadable_test_ctx scadable_test_ctx_t;

/**
 * Diagnostic handler signature. Use the SCADABLE_TEST() macro instead of
 * defining this directly — the macro handles registration via codegen.
 */
typedef scadable_test_result_t (*scadable_test_fn_t)(scadable_test_ctx_t *ctx);

/**
 * Register a diagnostic test. The build pipeline emits scadable_init_diagnostics()
 * which calls this for each YAML-declared test.
 */
scadable_err_t scadable_register_test_(const char *name, scadable_test_fn_t fn);

/**
 * Mid-test progress logging — visible in dashboard "Run check" results.
 */
void scadable_test_log_(scadable_test_ctx_t *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// SCADABLE_TEST(name, ctx) {
//     TEST_LOG(ctx, "starting probe");
//     if (problem) return TEST_FAIL("sensor unreachable");
//     return TEST_PASS("temp=%.2f", t);
// }
#define SCADABLE_TEST(name, ctx_param) scadable_test_result_t name(scadable_test_ctx_t *ctx_param)

#define TEST_LOG(ctx, fmt, ...) scadable_test_log_(ctx, fmt, ##__VA_ARGS__)

scadable_test_result_t scadable_test_make_(scadable_test_status_t status, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define TEST_PASS(...)           scadable_test_make_(TEST_RESULT_PASS, __VA_ARGS__)
#define TEST_PASS_WITH_WARN(...) scadable_test_make_(TEST_RESULT_PASS_WITH_WARN, __VA_ARGS__)
#define TEST_FAIL(...)           scadable_test_make_(TEST_RESULT_FAIL, __VA_ARGS__)

// scadable_init_diagnostics() is emitted by the build pipeline from
// .scadable/diagnostics/*.yaml. Customer calls it once after scadable_init().
// If you're not using the SCADABLE build pipeline (standalone build),
// register tests manually with scadable_register_test_() or, for the v0.3.0
// typed surface, scadable_register_diagnostic().
extern void scadable_init_diagnostics(void);

// ─── Diagnostics v0.3.0 — typed diagnostic surface ──────────────
//
// Goal: extensible across diagnostic types (function today; api_call,
// sensor_read, mqtt_check etc. tomorrow) without breaking the wire format
// or forcing a customer rewrite per type.
//
// A diagnostic is identified by a stable string id (matches the YAML `id:`
// field). The chip implements only the type(s) it knows about; cloud-side
// trigger for an unknown type yields TEST_RESULT_TYPE_NOT_SUPPORTED rather
// than a crash. v1 only defines the "function" type; v1.1+ adds the rest.

#define SCD_DIAG_DETAILS_CAP 1024

/**
 * Typed diagnostic result. Replaces (additively) scadable_test_result_t for
 * v0.3.0+ diagnostics. The library auto-fills `duration_ms`; the customer
 * fills `status` and (optionally) `message`/`details` via the DIAG_PASS /
 * DIAG_FAIL / DIAG_PASS_WITH_WARN macros below or by hand.
 *
 * `details` is a free-form 1 KiB buffer. v1 customers use it however they
 * like (additional log lines, JSON fragments, etc). Future v1.1+ types
 * document a structured shape per type — e.g. an `api_call` type might
 * write `{"status_code":200,"body_len":4123}` here.
 *
 * `output_log` is a borrowed pointer to a streaming-log buffer. v1 chips
 * leave this NULL (the per-call ctx log_buf is published as the v1
 * "log" envelope field); v1.1+ uses it for streaming output.
 */
typedef struct {
    scadable_test_status_t status;
    uint32_t duration_ms;                        // library auto-measures
    char message[256];                           // short summary
    char details[SCD_DIAG_DETAILS_CAP];          // free-form, optional
    const char *output_log;                      // NULL in v1; reserved for streaming
} scadable_diag_result_t;

typedef struct scadable_diag_ctx scadable_diag_ctx_t;

/**
 * Typed diagnostic handler. Mirrors scadable_test_fn_t but takes/returns the
 * v0.3.0 typed shapes.
 */
typedef scadable_diag_result_t (*scadable_diag_fn_t)(scadable_diag_ctx_t *ctx);

/**
 * Register a diagnostic with explicit type. v1 only accepts type_str ==
 * "function"; passing any other value returns SCADABLE_ERR_INVALID_ARG.
 *
 * Codegen-emitted scadable_init_diagnostics() calls this once per
 * YAML-declared diagnostic at boot. Manual customers can call it directly.
 */
scadable_err_t
scadable_register_diagnostic(const char *id, const char *type_str, scadable_diag_fn_t fn);

/**
 * Mid-diagnostic progress logging. Same as TEST_LOG / scadable_test_log_
 * but for the typed ctx.
 */
void scadable_diag_log_(scadable_diag_ctx_t *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define SCD_DIAG(id, ctx_param) scadable_diag_result_t id(scadable_diag_ctx_t *ctx_param)
#define DIAG_LOG(ctx, fmt, ...) scadable_diag_log_(ctx, fmt, ##__VA_ARGS__)

scadable_diag_result_t scadable_diag_make_(scadable_test_status_t status, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define DIAG_PASS(...)           scadable_diag_make_(TEST_RESULT_PASS, __VA_ARGS__)
#define DIAG_PASS_WITH_WARN(...) scadable_diag_make_(TEST_RESULT_PASS_WITH_WARN, __VA_ARGS__)
#define DIAG_FAIL(...)           scadable_diag_make_(TEST_RESULT_FAIL, __VA_ARGS__)
#define DIAG_TIMEOUT(...)        scadable_diag_make_(TEST_RESULT_TIMEOUT, __VA_ARGS__)
#define DIAG_ERROR(...)          scadable_diag_make_(TEST_RESULT_ERROR, __VA_ARGS__)

/**
 * Run a single registered diagnostic by id and publish its result on
 * `{ns}/{gw}/diagnostics/result` with envelope v2 (run_id + triggered_by
 * fields). Used by the cloud-triggered manual-run path.
 *
 * `run_id` is the cloud-minted ULID/UUID string; the chip returns it
 * verbatim in the result envelope so the cloud can correlate.
 *
 * Returns SCADABLE_OK if the diagnostic was found and scheduled.
 * Returns SCADABLE_ERR_INVALID_ARG if id is unknown — the chip will also
 * publish a TEST_RESULT_ERROR envelope so the cloud sees the failure.
 */
scadable_err_t scadable_run_diagnostic(const char *id, const char *run_id);

/**
 * Run every registered diagnostic. Used for the manual "Run all" button
 * and for the OTA-verify flow (cloud fans out one cmd per `run_after_ota`
 * diagnostic, but the chip's helper here is also useful for offline
 * customer-driven sweeps).
 *
 * All resulting envelopes share the same `run_id` so the cloud aggregator
 * can match them up.
 */
scadable_err_t scadable_run_all_diagnostics(const char *run_id);

// ─── Env vars + secrets (delivered at runtime via mTLS HTTPS pull) ───

/**
 * Get the value of a runtime env var. Returns NULL if the key isn't set.
 *
 * Env vars are fetched from the cloud via mTLS HTTPS GET to
 * `https://edge.scadable.com/api/v1/gateways/{id}/env_vars` at startup
 * and refreshed whenever the cloud sends an EVT_ENV_CHANGED nudge over
 * MQTT. The cache lives in RAM only (NOT persisted to NVS).
 *
 * Returned pointer is owned by the library and remains valid until the
 * next refresh — copy it if you need to hold across an env update.
 */
const char *scadable_env_get(const char *key);

/**
 * Same as scadable_env_get but returns `fallback` instead of NULL when the
 * key is missing. Use this for ergonomic default-value patterns:
 *   const char *region = scadable_env_get_or("REGION", "us-east");
 */
const char *scadable_env_get_or(const char *key, const char *fallback);

/**
 * Get an env var as an int32. Returns `fallback` if the key is missing OR if
 * the value isn't a fully-consumable int (no trailing chars allowed).
 */
int32_t scadable_env_get_int(const char *key, int32_t fallback);

/**
 * Get an env var as a double. Returns `fallback` if missing or unparseable.
 */
double scadable_env_get_double(const char *key, double fallback);

/**
 * Get an env var as a bool. Accepts "true"/"1"/"yes" → true and
 * "false"/"0"/"no" → false; anything else returns `fallback`.
 */
bool scadable_env_get_bool(const char *key, bool fallback);

/**
 * Get a secret value. Same contract as scadable_env_get but reads from the
 * separate `secrets` table — not visible to env_get callers.
 *
 * Secrets are NOT persisted to NVS — they live in RAM only so a flash dump
 * doesn't leak them. Trade-off: secrets reset to "missing" on reboot until
 * the next refresh succeeds. Don't put critical bootstrap credentials here;
 * those belong in the device cert (which IS in NVS, encrypted at rest by
 * flash encryption when enabled).
 */
const char *scadable_secret_get(const char *key);

/**
 * Same as scadable_secret_get but returns `fallback` instead of NULL.
 */
const char *scadable_secret_get_or(const char *key, const char *fallback);

/**
 * Per-key env-change callback. Fires once per env key after every refresh
 * (whether the value actually changed or not — diff-only behavior is v0.2).
 * Does NOT fire for secret values (those use the secret_get path).
 *
 * Callback runs on the library's HTTPS-fetch task. Keep it fast and don't
 * call scadable_env_get / scadable_env_change_cb_t-related functions
 * recursively — though scadable_env_get itself is reentrant-safe.
 */
typedef void (*scadable_env_change_cb_t)(const char *key, const char *new_value, void *user);
void scadable_on_env_change(scadable_env_change_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
#endif  // SCADABLE_H
