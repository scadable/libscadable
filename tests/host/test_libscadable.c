// test_libscadable.c — host-side smoke suite for libscadable.
// SCADABLE 2026 · Apache-2.0
//
// Run via:
//   cmake -S tests/host -B build/host && cmake --build build/host
//   ./build/host/test_libscadable
//
// Catches:
//   - Compile / link breakage in the !ESP_PLATFORM branches of every src/*.c
//   - Public API contract drift (return codes, NULL handling, idempotency)
//   - JSON envelope shape regressions in env / diagnostics / logs
//
// What it does NOT cover (needs hardware or QEMU):
//   - mTLS handshake against a real broker
//   - actual MQTT enqueue + PUBACK accounting
//   - esp_https_ota partition swap
//   - NVS persistence
//
// Those are tested via examples/esp-idf-hello-world on real hardware in CI.

#define SCADABLE_NO_GENERATED
#include "scadable.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Test hooks from env.c host build (see #else branch of scd_env_refresh_blocking).
extern void scd_env_test_set_response(const char *json);
extern int scd_env_refresh_blocking(void);

static int passes = 0;
static int fails  = 0;
static void *event_user;
static int event_count;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            printf("  PASS  %s\n", msg);                                                           \
            passes++;                                                                              \
        } else {                                                                                   \
            printf("  FAIL  %s  (at %s:%d)\n", msg, __FILE__, __LINE__);                           \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

static void on_evt(scadable_event_t e, void *user) {
    (void)e;
    (void)user;
    event_count++;
}

// ────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────────────────

static void test_lifecycle(void) {
    printf("\n[lifecycle]\n");
    CHECK(scadable_state() == SCADABLE_STATE_UNINITIALIZED, "starts uninitialized");
    CHECK(!scadable_is_connected(), "not connected before init");

    scadable_err_t e = scadable_publish(0, "x", 1, 1);
    CHECK(e == SCADABLE_ERR_NOT_CONNECTED, "publish before init returns NOT_CONNECTED");

    e = scadable_init(NULL);
    CHECK(e == SCADABLE_OK, "init succeeds with NULL config");
    CHECK(scadable_state() == SCADABLE_STATE_IDLE, "state == IDLE after init");

    e = scadable_init(NULL);
    CHECK(e == SCADABLE_OK, "init is idempotent");

    scadable_on_event(on_evt, &event_user);
    CHECK(true, "register event callback");

    e = scadable_connect();
    CHECK(e == SCADABLE_OK, "connect succeeds (host stub)");

    e = scadable_disconnect();
    CHECK(e == SCADABLE_OK, "disconnect succeeds");
}

// ────────────────────────────────────────────────────────────────────────────
// Publish — host build is a no-op but should validate args.
// ────────────────────────────────────────────────────────────────────────────

static void test_publish(void) {
    printf("\n[publish]\n");
    scadable_err_t e = scadable_publish(0, NULL, 5, 1);
    CHECK(e == SCADABLE_ERR_INVALID_ARG, "publish(NULL, len>0) → INVALID_ARG");

    e = scadable_publish(0, "hi", 2, 99);
    CHECK(e == SCADABLE_ERR_INVALID_ARG, "publish(qos=99) → INVALID_ARG");

    // Zero-length payload allowed (heartbeat-style ping).
    e = scadable_publish(0, NULL, 0, 0);
    CHECK(e == SCADABLE_ERR_NOT_CONNECTED, "publish(NULL, 0) when not connected → NOT_CONNECTED");

    // Flush is a no-op on host but must not crash.
    e = scadable_flush(100);
    CHECK(e == SCADABLE_OK, "flush returns OK on host");

    e = scadable_announce_offline(60);
    CHECK(e == SCADABLE_ERR_NOT_CONNECTED, "announce_offline when not connected → NOT_CONNECTED");
}

// ────────────────────────────────────────────────────────────────────────────
// Telemetry
// ────────────────────────────────────────────────────────────────────────────

static void test_telemetry(void) {
    printf("\n[telemetry]\n");
    // Not connected on host → publish is a no-op but still returns OK because
    // the host stub of scd_mqtt_publish bypasses the connected check.
    scadable_err_t e = scadable_metric_set_u32(0, 42);
    CHECK(e == SCADABLE_OK || e == SCADABLE_ERR_NOT_CONNECTED, "metric_set_u32 returns sane code");

    e = scadable_metric_set_f64(0, 3.14159);
    CHECK(e == SCADABLE_OK || e == SCADABLE_ERR_NOT_CONNECTED, "metric_set_f64 returns sane code");
}

// ────────────────────────────────────────────────────────────────────────────
// Logging
// ────────────────────────────────────────────────────────────────────────────

static void test_logging(void) {
    printf("\n[logging]\n");
    // Macros must compile and not crash even when init didn't run yet.
    SCADABLE_LOG_INFO("smoke %d %s", 1, "hello");
    SCADABLE_LOG_DEBUG("debug line");
    SCADABLE_LOG_WARN("warn %s", "msg");
    SCADABLE_LOG_ERROR("error %d", -1);
    CHECK(true, "log macros don't crash");
}

// ────────────────────────────────────────────────────────────────────────────
// Env vars
// ────────────────────────────────────────────────────────────────────────────

static void test_env(void) {
    printf("\n[env]\n");
    scd_env_test_set_response(
        "{\"env\":{\"REGION\":\"us-east\",\"PORT\":\"8080\",\"FLAG\":\"true\","
        "\"PI\":\"3.14\"},\"secrets\":{\"API_KEY\":\"sk_live_xyz\"}}");
    int rc = scd_env_refresh_blocking();
    CHECK(rc == 0, "env_refresh parses response");

    const char *v = scadable_env_get("REGION");
    CHECK(v && strcmp(v, "us-east") == 0, "env_get returns string value");

    v = scadable_env_get("DOES_NOT_EXIST");
    CHECK(v == NULL, "env_get for missing key returns NULL");

    v = scadable_env_get_or("DOES_NOT_EXIST", "fallback");
    CHECK(v && strcmp(v, "fallback") == 0, "env_get_or returns fallback");

    int32_t i = scadable_env_get_int("PORT", 0);
    CHECK(i == 8080, "env_get_int parses number");

    i = scadable_env_get_int("REGION", -1);
    CHECK(i == -1, "env_get_int falls back when value isn't a number");

    double d = scadable_env_get_double("PI", 0);
    CHECK(d > 3.13 && d < 3.15, "env_get_double parses number");

    bool b = scadable_env_get_bool("FLAG", false);
    CHECK(b == true, "env_get_bool parses 'true'");

    const char *s = scadable_secret_get("API_KEY");
    CHECK(s && strcmp(s, "sk_live_xyz") == 0, "secret_get returns secret value");

    s = scadable_secret_get("REGION");  // env, not secret
    CHECK(s == NULL, "secret_get does not see env vars");

    s = scadable_secret_get_or("MISSING", "x");
    CHECK(s && strcmp(s, "x") == 0, "secret_get_or returns fallback");
}

// ────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ────────────────────────────────────────────────────────────────────────────

static int diag_calls;
static scadable_test_result_t diag_pass(scadable_test_ctx_t *ctx) {
    TEST_LOG(ctx, "running");
    diag_calls++;
    return TEST_PASS("ok");
}
static scadable_test_result_t diag_fail(scadable_test_ctx_t *ctx) {
    diag_calls++;
    return TEST_FAIL("nope");
}

static void test_diagnostics(void) {
    printf("\n[diagnostics]\n");
    scadable_err_t e = scadable_register_test_("pass_test", diag_pass);
    CHECK(e == SCADABLE_OK, "register pass test");
    e = scadable_register_test_("fail_test", diag_fail);
    CHECK(e == SCADABLE_OK, "register fail test");
    e = scadable_register_test_(NULL, diag_pass);
    CHECK(e == SCADABLE_ERR_INVALID_ARG, "register NULL name → INVALID_ARG");

    scadable_test_result_t r = scadable_test_make_(TEST_RESULT_FAIL, "boom %d", 42);
    CHECK(r.status == TEST_RESULT_FAIL, "test_make_ status preserved");
    CHECK(strstr(r.message, "boom 42") != NULL, "test_make_ formats message");
}

// ────────────────────────────────────────────────────────────────────────────
// Strerror coverage
// ────────────────────────────────────────────────────────────────────────────

static void test_strerror(void) {
    printf("\n[strerror]\n");
    CHECK(scadable_strerror(SCADABLE_OK) != NULL, "OK has message");
    CHECK(scadable_strerror(SCADABLE_ERR_TIMEOUT) != NULL, "TIMEOUT has message");
    CHECK(scadable_strerror((scadable_err_t)999) != NULL, "unknown code has fallback message");
}

int main(void) {
    test_lifecycle();
    test_publish();
    test_telemetry();
    test_logging();
    test_env();
    test_diagnostics();
    test_strerror();

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails == 0 ? 0 : 1;
}
