// diagnostics.c — SCADABLE_TEST registration + invocation.
// SCADABLE 2026 · Apache-2.0
//
// Customers declare diagnostics in .scadable/diagnostics/*.yaml; the build
// pipeline emits scadable_init_diagnostics() which calls the underscore
// registration function once per test. For standalone builds (no pipeline)
// the customer registers manually:
//
//     SCADABLE_TEST(temp_sensor, ctx) {
//         TEST_LOG(ctx, "reading sensor");
//         float t = read_temp();
//         if (t < -40 || t > 125) return TEST_FAIL("out of range: %.2f", t);
//         return TEST_PASS("temp=%.2f", t);
//     }
//     ...
//     scadable_register_test_("temp_sensor", temp_sensor);
//
// Results publish on `{ns}/{gw}/diagnostics/result` QoS1 in this shape:
//   {"v":1,"ts_ms":...,"results":[
//      {"name":"temp_sensor","status":"pass","duration_ms":12,
//       "message":"temp=23.50","log":"reading sensor\n"}
//   ]}
//
// The OTA gate (in ota.c) calls scd_diag_run_all_blocking() before performing
// the swap and aborts if any test returns FAIL. PASS_WITH_WARN does NOT block
// OTA — it surfaces in the dashboard but doesn't gate.

#include "internal.h"
#include "scadable.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#    include "esp_log.h"
#    include "esp_timer.h"
#    include "freertos/FreeRTOS.h"
#    include "freertos/semphr.h"
#endif

#define MAX_TESTS    64
#define TEST_LOG_CAP 512

static const char *TAG __attribute__((unused)) = "scadable.diag";

struct scadable_test_ctx {
    const char *test_name;
    char log_buf[TEST_LOG_CAP];
    size_t log_len;
};

static struct {
    struct {
        const char *name;
        scadable_test_fn_t fn;
    } tests[MAX_TESTS];
    int count;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t mtx;
#endif
} D = {0};

static int64_t diag_now_ms(void) {
#ifdef ESP_PLATFORM
    return esp_timer_get_time() / 1000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static const char *status_str(scadable_test_status_t s) {
    switch (s) {
    case TEST_RESULT_PASS:
        return "pass";
    case TEST_RESULT_PASS_WITH_WARN:
        return "pass_with_warn";
    case TEST_RESULT_FAIL:
        return "fail";
    default:
        return "unknown";
    }
}

void scd_diag_init(void) {
#ifdef ESP_PLATFORM
    if (!D.mtx) D.mtx = xSemaphoreCreateMutex();
#endif
    // Customer-defined wiring: codegen-emitted scadable_init_diagnostics()
    // registers every YAML-declared test. Weak default below makes this safe
    // when no codegen ran.
    scadable_init_diagnostics();
}

void scd_diag_shutdown(void) {
    // Tests are statically registered in BSS — nothing to free.
    D.count = 0;
}

scadable_err_t scadable_register_test_(const char *name, scadable_test_fn_t fn) {
    if (!name || !fn) return SCADABLE_ERR_INVALID_ARG;
#ifdef ESP_PLATFORM
    if (D.mtx) xSemaphoreTake(D.mtx, portMAX_DELAY);
#endif
    if (D.count >= MAX_TESTS) {
#ifdef ESP_PLATFORM
        if (D.mtx) xSemaphoreGive(D.mtx);
#endif
        return SCADABLE_ERR_INTERNAL;
    }
    D.tests[D.count].name = name;
    D.tests[D.count].fn   = fn;
    D.count++;
#ifdef ESP_PLATFORM
    if (D.mtx) xSemaphoreGive(D.mtx);
#endif
    return SCADABLE_OK;
}

void scadable_test_log_(scadable_test_ctx_t *ctx, const char *fmt, ...) {
    if (!ctx) return;
    if (ctx->log_len + 1 >= sizeof(ctx->log_buf)) return;  // full
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ctx->log_buf + ctx->log_len, sizeof(ctx->log_buf) - ctx->log_len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    ctx->log_len += (size_t)n;
    if (ctx->log_len + 1 < sizeof(ctx->log_buf)) {
        ctx->log_buf[ctx->log_len++] = '\n';
        ctx->log_buf[ctx->log_len]   = '\0';
    }
}

scadable_test_result_t scadable_test_make_(scadable_test_status_t status, const char *fmt, ...) {
    scadable_test_result_t r = {.status = status, .duration_ms = 0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    return r;
}

// ─── JSON-escape helper (duplicated from logging.c for simplicity) ──────────
//
// Could be hoisted into a shared util but the v0.1 cut keeps each module
// self-contained. ~30 lines duplicated, no plans to grow it.

static int diag_json_escape(char *out, size_t out_sz, const char *in) {
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        const char *esc = NULL;
        char uesc[8];
        if (c == '"')
            esc = "\\\"";
        else if (c == '\\')
            esc = "\\\\";
        else if (c == '\n')
            esc = "\\n";
        else if (c == '\r')
            esc = "\\r";
        else if (c == '\t')
            esc = "\\t";
        else if (c < 0x20) {
            snprintf(uesc, sizeof(uesc), "\\u%04x", c);
            esc = uesc;
        }
        if (esc) {
            size_t l = strlen(esc);
            if (o + l + 1 >= out_sz) return -1;
            memcpy(out + o, esc, l);
            o += l;
        } else {
            if (o + 2 >= out_sz) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

bool scd_diag_run_all_blocking(void) {
    if (D.count == 0) return true;

    // Allocate envelope. Each result ~700 bytes worst-case (256 message +
    // 512 log + name + overhead).
    size_t cap = 256 + (size_t)D.count * 1024;
    char *body = malloc(cap);
    if (!body) return false;
    int o = snprintf(body, cap, "{\"v\":1,\"ts_ms\":%" PRId64 ",\"results\":[", diag_now_ms());
    if (o <= 0) {
        free(body);
        return false;
    }

    bool overall_ok = true;

    for (int i = 0; i < D.count; i++) {
        scadable_test_ctx_t ctx    = {.test_name = D.tests[i].name};
        int64_t t0                 = diag_now_ms();
        scadable_test_result_t res = D.tests[i].fn(&ctx);
        res.duration_ms            = (uint32_t)(diag_now_ms() - t0);

        if (res.status == TEST_RESULT_FAIL) overall_ok = false;

        char esc_msg[sizeof(res.message) * 2 + 8];
        char esc_log[TEST_LOG_CAP * 2 + 8];
        if (diag_json_escape(esc_msg, sizeof(esc_msg), res.message) < 0) esc_msg[0] = '\0';
        if (diag_json_escape(esc_log, sizeof(esc_log), ctx.log_buf) < 0) esc_log[0] = '\0';

        int n = snprintf(body + o, cap - (size_t)o,
                         "%s{\"name\":\"%s\",\"status\":\"%s\",\"duration_ms\":%u,"
                         "\"message\":\"%s\",\"log\":\"%s\"}",
                         i == 0 ? "" : ",", D.tests[i].name, status_str(res.status),
                         (unsigned)res.duration_ms, esc_msg, esc_log);
        if (n <= 0 || (size_t)(o + n) >= cap) break;
        o += n;
    }

    if ((size_t)o + 2 < cap) {
        body[o++] = ']';
        body[o++] = '}';
    }

    char topic[160];
    if (scd_topic_diag_result(topic, sizeof(topic)) > 0) {
        scd_mqtt_publish(topic, body, (size_t)o, 1, /*retain=*/false);
    }
    free(body);
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "diagnostics complete: %d tests, overall=%s", D.count,
             overall_ok ? "pass" : "fail");
#else
    (void)TAG;
#endif
    return overall_ok;
}

// scadable_init_diagnostics() — emitted by the build pipeline. Weak default
// so standalone builds (no pipeline) don't fail to link.
__attribute__((weak)) void scadable_init_diagnostics(void) {
    // No-op default — overridden by build-pipeline-emitted scadable_generated.c.
}

// ─── v0.3.0 typed-diagnostic surface ───────────────────────────────────────
//
// New registration table (separate from the legacy v0.1 `D.tests` table) so
// the two surfaces don't tangle. Each entry carries a type_str so the
// codegen can emit registrations for ANY YAML-declared diagnostic — even
// types this firmware doesn't know about — and we can return
// TEST_RESULT_TYPE_NOT_SUPPORTED at run time without crashing.
//
// Cloud-trigger flow:
//   1. Cloud publishes JSON `{"run_id":"...", "id":"motor_health"}` on
//      `{ns}/{gw}/cmd/diagnostic.run` (or `cmd/diagnostic.run_all`).
//   2. lifecycle.c MQTT_EVENT_DATA hands payload to scd_cmd_dispatch().
//   3. scd_cmd_dispatch() calls scadable_run_diagnostic(id, run_id).
//   4. We look up id in the v0.3.0 table; if found, invoke; if found-but-
//      type-not-supported, return TYPE_NOT_SUPPORTED; if missing, return
//      ERROR with "unknown diagnostic id".
//   5. Result is published as envelope v2 on the existing
//      `{ns}/{gw}/diagnostics/result` topic.

#define MAX_DIAGS       64
#define DIAG_LOG_CAP    512
#define DIAG_RUN_ID_CAP 64

struct scadable_diag_ctx {
    const char *id;
    char log_buf[DIAG_LOG_CAP];
    size_t log_len;
};

static struct {
    struct {
        const char *id;
        const char *type_str;
        scadable_diag_fn_t fn;  // NULL means type-not-supported on this fw
    } items[MAX_DIAGS];
    int count;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t mtx;
#endif
} D2 = {0};

static const char *v3_status_str(scadable_test_status_t s) {
    switch (s) {
    case TEST_RESULT_PASS:
        return "pass";
    case TEST_RESULT_PASS_WITH_WARN:
        return "pass_with_warn";
    case TEST_RESULT_FAIL:
        return "fail";
    case TEST_RESULT_TIMEOUT:
        return "timeout";
    case TEST_RESULT_ERROR:
        return "error";
    case TEST_RESULT_TYPE_NOT_SUPPORTED:
        return "type_not_supported";
    default:
        return "unknown";
    }
}

void scadable_diag_log_(scadable_diag_ctx_t *ctx, const char *fmt, ...) {
    if (!ctx) return;
    if (ctx->log_len + 1 >= sizeof(ctx->log_buf)) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ctx->log_buf + ctx->log_len, sizeof(ctx->log_buf) - ctx->log_len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    ctx->log_len += (size_t)n;
    if (ctx->log_len + 1 < sizeof(ctx->log_buf)) {
        ctx->log_buf[ctx->log_len++] = '\n';
        ctx->log_buf[ctx->log_len]   = '\0';
    }
}

scadable_diag_result_t scadable_diag_make_(scadable_test_status_t status, const char *fmt, ...) {
    scadable_diag_result_t r = {.status = status, .duration_ms = 0, .output_log = NULL};
    r.message[0]             = '\0';
    r.details[0]             = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r.message, sizeof(r.message), fmt, ap);
        va_end(ap);
    }
    return r;
}

scadable_err_t
scadable_register_diagnostic(const char *id, const char *type_str, scadable_diag_fn_t fn) {
    if (!id || !type_str) return SCADABLE_ERR_INVALID_ARG;
#ifdef ESP_PLATFORM
    if (!D2.mtx) D2.mtx = xSemaphoreCreateMutex();
    if (D2.mtx) xSemaphoreTake(D2.mtx, portMAX_DELAY);
#endif
    if (D2.count >= MAX_DIAGS) {
#ifdef ESP_PLATFORM
        if (D2.mtx) xSemaphoreGive(D2.mtx);
#endif
        return SCADABLE_ERR_INTERNAL;
    }
    // v1 only knows "function". Other types are accepted into the registry
    // (so the codegen can emit them and the dashboard sees the diagnostic
    // exists) but stored with fn=NULL so the runner returns
    // TYPE_NOT_SUPPORTED on invocation.
    bool supported              = (strcmp(type_str, "function") == 0);
    D2.items[D2.count].id       = id;
    D2.items[D2.count].type_str = type_str;
    D2.items[D2.count].fn       = supported ? fn : NULL;
    D2.count++;
#ifdef ESP_PLATFORM
    if (D2.mtx) xSemaphoreGive(D2.mtx);
#endif
    if (!supported) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "diag %s: type '%s' not supported on this firmware version", id, type_str);
#endif
    }
    return SCADABLE_OK;
}

static int diag_find(const char *id) {
    for (int i = 0; i < D2.count; i++) {
        if (strcmp(D2.items[i].id, id) == 0) return i;
    }
    return -1;
}

// Publish a single result row using envelope v2 (run_id + triggered_by + type).
// `id` is the diagnostic id. `type_str` is "function" / "api_call" / etc.
// `triggered_by` is "manual" / "ota_verify" / "local_pre_ota" / "scheduled".
static void diag_publish_v2(const char *id,
                            const char *type_str,
                            const char *run_id,
                            const char *triggered_by,
                            const scadable_diag_result_t *res,
                            const char *log_buf) {
    if (!id || !type_str || !run_id || !triggered_by || !res) return;

    // Worst-case: ~256 message + 1024 details + 512 log + headers.
    size_t cap = 2400;
    char *body = malloc(cap);
    if (!body) return;

    char esc_msg[sizeof(res->message) * 2 + 8];
    char esc_det[SCD_DIAG_DETAILS_CAP * 2 + 8];
    char esc_log[DIAG_LOG_CAP * 2 + 8];
    if (diag_json_escape(esc_msg, sizeof(esc_msg), res->message) < 0) esc_msg[0] = '\0';
    if (diag_json_escape(esc_det, sizeof(esc_det), res->details) < 0) esc_det[0] = '\0';
    if (diag_json_escape(esc_log, sizeof(esc_log), log_buf ? log_buf : "") < 0) esc_log[0] = '\0';

    int n = snprintf(body, cap,
                     "{\"v\":2,\"ts_ms\":%" PRId64 ",\"run_id\":\"%s\","
                     "\"triggered_by\":\"%s\",\"results\":[{"
                     "\"id\":\"%s\",\"type\":\"%s\",\"status\":\"%s\","
                     "\"duration_ms\":%u,\"message\":\"%s\","
                     "\"details\":\"%s\",\"log\":\"%s\"}]}",
                     diag_now_ms(), run_id, triggered_by, id, type_str, v3_status_str(res->status),
                     (unsigned)res->duration_ms, esc_msg, esc_det, esc_log);
    if (n > 0 && (size_t)n < cap) {
        char topic[160];
        if (scd_topic_diag_result(topic, sizeof(topic)) > 0) {
            scd_mqtt_publish(topic, body, (size_t)n, 1, /*retain=*/false);
        }
    }
    free(body);
}

scadable_err_t scadable_run_diagnostic(const char *id, const char *run_id) {
    if (!id || !run_id) return SCADABLE_ERR_INVALID_ARG;
#ifdef ESP_PLATFORM
    if (D2.mtx) xSemaphoreTake(D2.mtx, portMAX_DELAY);
#endif
    int idx = diag_find(id);
    if (idx < 0) {
#ifdef ESP_PLATFORM
        if (D2.mtx) xSemaphoreGive(D2.mtx);
#endif
        // Publish an ERROR envelope so the cloud sees the run completed
        // (rather than timing out a pending row).
        scadable_diag_result_t r =
            scadable_diag_make_(TEST_RESULT_ERROR, "unknown diagnostic id: %s", id);
        diag_publish_v2(id, "unknown", run_id, "manual", &r, NULL);
        return SCADABLE_ERR_INVALID_ARG;
    }
    const char *type_str  = D2.items[idx].type_str;
    scadable_diag_fn_t fn = D2.items[idx].fn;
#ifdef ESP_PLATFORM
    if (D2.mtx) xSemaphoreGive(D2.mtx);
#endif

    if (!fn) {
        // Type not supported on this firmware (e.g. cloud asked for an
        // api_call diagnostic on a v1 chip).
        scadable_diag_result_t r =
            scadable_diag_make_(TEST_RESULT_TYPE_NOT_SUPPORTED,
                                "diagnostic type '%s' not supported on this firmware", type_str);
        diag_publish_v2(id, type_str, run_id, "manual", &r, NULL);
        return SCADABLE_OK;
    }

    scadable_diag_ctx_t ctx    = {.id = id};
    int64_t t0                 = diag_now_ms();
    scadable_diag_result_t res = fn(&ctx);
    res.duration_ms            = (uint32_t)(diag_now_ms() - t0);

    diag_publish_v2(id, type_str, run_id, "manual", &res, ctx.log_buf);
    return SCADABLE_OK;
}

scadable_err_t scadable_run_all_diagnostics(const char *run_id) {
    if (!run_id) return SCADABLE_ERR_INVALID_ARG;
    // Snapshot count + ids under the lock; release before invoking each fn
    // (don't hold the registry mutex while running customer code).
    int n = 0;
    const char *ids[MAX_DIAGS];
#ifdef ESP_PLATFORM
    if (D2.mtx) xSemaphoreTake(D2.mtx, portMAX_DELAY);
#endif
    for (int i = 0; i < D2.count && n < MAX_DIAGS; i++) {
        ids[n++] = D2.items[i].id;
    }
#ifdef ESP_PLATFORM
    if (D2.mtx) xSemaphoreGive(D2.mtx);
#endif
    for (int i = 0; i < n; i++) {
        scadable_run_diagnostic(ids[i], run_id);
    }
    return SCADABLE_OK;
}

// ─── Cmd-topic dispatcher ──────────────────────────────────────────────────
//
// Called from lifecycle.c MQTT_EVENT_DATA when topic is `{ns}/{gw}/cmd/{type}`.
// Body shape varies per cmd type; for diagnostic commands it's:
//   {"run_id":"01HXY...","id":"motor_health"}        (diagnostic.run)
//   {"run_id":"01HXY..."}                            (diagnostic.run_all)
//
// Tiny inline JSON extractor (no cJSON dep — keeps libscadable's dep
// footprint small). Limitation: doesn't handle escapes inside the value.
// That's fine for ULIDs and id strings (alphanumeric + underscore + dot).

static bool
diag_extract_str(const char *body, size_t len, const char *key, char *out, size_t out_sz) {
    if (!body || !key || !out || out_sz == 0) return false;
    char needle[64];
    int nn = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nn <= 0 || (size_t)nn >= sizeof(needle)) return false;
    const char *p = NULL;
    for (size_t i = 0; i + (size_t)nn <= len; i++) {
        if (memcmp(body + i, needle, (size_t)nn) == 0) {
            p = body + i + nn;
            break;
        }
    }
    if (!p) return false;
    while (*p == ' ' || *p == ':')
        p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_sz)
        out[o++] = *p++;
    out[o] = '\0';
    return o > 0;
}

void scd_cmd_dispatch(const char *cmd_type, const char *body, size_t len) {
    if (!cmd_type || !body) return;
    if (strcmp(cmd_type, "diagnostic.run") == 0) {
        char run_id[DIAG_RUN_ID_CAP];
        char id[64];
        if (!diag_extract_str(body, len, "run_id", run_id, sizeof(run_id)) ||
            !diag_extract_str(body, len, "id", id, sizeof(id))) {
#ifdef ESP_PLATFORM
            ESP_LOGW(TAG, "diagnostic.run: missing run_id or id field");
#endif
            return;
        }
        scadable_run_diagnostic(id, run_id);
    } else if (strcmp(cmd_type, "diagnostic.run_all") == 0) {
        char run_id[DIAG_RUN_ID_CAP];
        if (!diag_extract_str(body, len, "run_id", run_id, sizeof(run_id))) {
#ifdef ESP_PLATFORM
            ESP_LOGW(TAG, "diagnostic.run_all: missing run_id field");
#endif
            return;
        }
        scadable_run_all_diagnostics(run_id);
    } else {
#ifdef ESP_PLATFORM
        ESP_LOGD(TAG, "unhandled cmd type: %s", cmd_type);
#endif
        (void)len;
    }
}
