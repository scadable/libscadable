// diagnostics.c — SCADABLE_TEST registration + invocation.
// SCADABLE 2026 · Apache-2.0
// STUB IMPLEMENTATION — wired up in Sprint 1 lane C.

#include "scadable.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_TESTS 64

struct scadable_test_ctx { const char *test_name; };

static struct {
    const char *name;
    scadable_test_fn_t fn;
} s_tests[MAX_TESTS];
static int s_test_count = 0;

scadable_err_t scadable_register_test_(const char *name, scadable_test_fn_t fn) {
    if (s_test_count >= MAX_TESTS) return SCADABLE_ERR_INTERNAL;
    s_tests[s_test_count].name = name;
    s_tests[s_test_count].fn = fn;
    s_test_count++;
    return SCADABLE_OK;
}

void scadable_test_log_(scadable_test_ctx_t *ctx, const char *fmt, ...) {
    (void)ctx;
    // TODO: append to per-test log buffer that ships with the result back to dashboard.
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

scadable_test_result_t scadable_test_make_(scadable_test_status_t status,
                                           const char *fmt, ...) {
    scadable_test_result_t r = { .status = status, .duration_ms = 0 };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    return r;
}

// scadable_init_diagnostics() is emitted by the build pipeline. If the
// customer is building standalone (no build pipeline), they call
// scadable_register_test_() manually for each test.
__attribute__((weak)) void scadable_init_diagnostics(void) {
    // No-op default — overridden by build-pipeline-emitted scadable_generated.c.
}
