// logging.c — leveled batched log macros.
// SCADABLE 2026 · Apache-2.0
// STUB IMPLEMENTATION — wired up in Sprint 1 lane C.

#include "scadable.h"
#include <stdarg.h>
#include <stdio.h>

void scadable_log_(scadable_log_level_t lvl, const char *file, int line,
                   const char *fmt, ...) {
    (void)lvl; (void)file; (void)line;
    // TODO: secret redaction via Zap-style wrapper.
    // TODO: write to ring buffer; flush every log_batch_secs or on scadable_flush().
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);   // dev-mode passthrough until ring buffer ships
    va_end(ap);
    printf("\n");
}
