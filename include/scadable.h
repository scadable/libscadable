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
#  include "scadable_generated.h"
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
    SCADABLE_OK                  =  0,
    SCADABLE_ERR_NOT_INITIALIZED = -1,
    SCADABLE_ERR_NOT_CONNECTED   = -2,
    SCADABLE_ERR_INVALID_ARG     = -3,
    SCADABLE_ERR_BACKPRESSURE    = -4,   // outbound queue full; retry after EVT_PUBLISHED
    SCADABLE_ERR_TIMEOUT         = -5,
    SCADABLE_ERR_NO_CERT         = -6,
    SCADABLE_ERR_TLS             = -7,
    SCADABLE_ERR_NETWORK         = -8,
    SCADABLE_ERR_TEST_FAILED     = -9,   // returned from diagnostic handlers
    SCADABLE_ERR_INTERNAL        = -100,
} scadable_err_t;

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
    SCADABLE_EVT_PUBLISHED,         // QoS1 PUBACK landed
    SCADABLE_EVT_ERROR,
    SCADABLE_EVT_OTA_AVAILABLE,     // new firmware ready; library handles apply automatically
    SCADABLE_EVT_ENV_CHANGED,       // env var or secret value updated by cloud
} scadable_event_type_t;

typedef struct {
    scadable_event_type_t type;
    union {
        struct { uint32_t recovered_count; }                              connected;
        struct { int32_t  msg_id; }                                       published;
        struct {
            scadable_err_t code;
            int32_t  esp_tls_err;
            int32_t  mbedtls_err;
            uint32_t cert_verify_flags;
            bool     retriable;
        }                                                                 error;
        struct { const char *new_version; }                               ota_available;
        struct { const char *key; const char *new_value; }                env_changed;
    };
} scadable_event_t;

typedef void (*scadable_event_cb_t)(scadable_event_t event, void *user);

// ─── Optional runtime overrides (NULL = use baked-in from config.toml) ───
typedef struct {
    const char *broker_url;
    const char *device_id;        // default = MAC-derived
    uint16_t    keepalive_secs;   // default 30
    uint16_t    log_batch_secs;   // default 5; 0 = realtime
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
 * Current state (sync, no IO).
 */
scadable_state_t scadable_state(void);
bool             scadable_is_connected(void);

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
scadable_err_t scadable_publish(scadable_channel_t channel,
                                const void *data, size_t len, uint8_t qos);

// ─── Telemetry (typed names from .scadable/digital-twin/) ───────

scadable_err_t scadable_metric_set_u32(scadable_metric_t metric, uint32_t value);
scadable_err_t scadable_metric_set_f64(scadable_metric_t metric, double   value);

// ─── Logging (leveled, batched, structured) ─────────────────────

typedef enum {
    SCADABLE_LOG_DEBUG_LEVEL,
    SCADABLE_LOG_INFO_LEVEL,
    SCADABLE_LOG_WARN_LEVEL,
    SCADABLE_LOG_ERROR_LEVEL,
} scadable_log_level_t;

void scadable_log_(scadable_log_level_t lvl, const char *file, int line,
                   const char *fmt, ...) __attribute__((format(printf, 4, 5)));

#define SCADABLE_LOG_DEBUG(fmt, ...) scadable_log_(SCADABLE_LOG_DEBUG_LEVEL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SCADABLE_LOG_INFO(fmt, ...)  scadable_log_(SCADABLE_LOG_INFO_LEVEL,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SCADABLE_LOG_WARN(fmt, ...)  scadable_log_(SCADABLE_LOG_WARN_LEVEL,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SCADABLE_LOG_ERROR(fmt, ...) scadable_log_(SCADABLE_LOG_ERROR_LEVEL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// ─── Diagnostics ────────────────────────────────────────────────

typedef enum {
    TEST_RESULT_PASS = 0,
    TEST_RESULT_PASS_WITH_WARN = 1,
    TEST_RESULT_FAIL = 2,
} scadable_test_status_t;

typedef struct {
    scadable_test_status_t status;
    char     message[256];          // formatted message, capped to keep RAM small
    uint32_t duration_ms;           // library auto-measures
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
#define SCADABLE_TEST(name, ctx_param) \
    scadable_test_result_t name(scadable_test_ctx_t *ctx_param)

#define TEST_LOG(ctx, fmt, ...) scadable_test_log_(ctx, fmt, ##__VA_ARGS__)

scadable_test_result_t scadable_test_make_(scadable_test_status_t status,
                                           const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define TEST_PASS(...)            scadable_test_make_(TEST_RESULT_PASS,           __VA_ARGS__)
#define TEST_PASS_WITH_WARN(...)  scadable_test_make_(TEST_RESULT_PASS_WITH_WARN, __VA_ARGS__)
#define TEST_FAIL(...)            scadable_test_make_(TEST_RESULT_FAIL,           __VA_ARGS__)

// scadable_init_diagnostics() is emitted by the build pipeline from
// .scadable/diagnostics/*.yaml. Customer calls it once after scadable_init().
// If you're not using the SCADABLE build pipeline (standalone build),
// register tests manually with scadable_register_test_().
extern void scadable_init_diagnostics(void);

// ─── Env vars + secrets (delivered at runtime via mTLS HTTPS pull) ───

const char *scadable_env_get(const char *key);
const char *scadable_env_get_or(const char *key, const char *fallback);
int32_t     scadable_env_get_int(const char *key, int32_t fallback);
double      scadable_env_get_double(const char *key, double fallback);
bool        scadable_env_get_bool(const char *key, bool fallback);

const char *scadable_secret_get(const char *key);
const char *scadable_secret_get_or(const char *key, const char *fallback);

typedef void (*scadable_env_change_cb_t)(const char *key, const char *new_value, void *user);
void scadable_on_env_change(scadable_env_change_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
#endif  // SCADABLE_H
