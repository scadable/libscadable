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
