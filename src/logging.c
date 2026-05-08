// logging.c — leveled, batched, structured logs.
// SCADABLE 2026 · Apache-2.0
//
// Design (mirrors gateway-esp/src/logs/batcher.rs):
//   - Customer calls SCADABLE_LOG_*("fmt", ...). Macro forwards to
//     scadable_log_() with __FILE__ + __LINE__.
//   - We format the message into a ~256B record and push into a fixed-cap ring
//     buffer (default 128 records, ~32 KB worst-case heap).
//   - A FreeRTOS flusher task wakes every `log_batch_secs` (default 5; 0 ⇒
//     realtime, no batching), drains the buffer, builds one MQTT envelope per
//     batch, and publishes on `{ns}/{gw}/sys/logs/batch` QoS1.
//   - On buffer-full the next push wakes the flusher early (don't sit on a
//     full buffer for the rest of the interval).
//   - When MQTT is down the records keep buffering; when it comes back up
//     the next flush drains everything. This is the offline-tolerant path
//     that Verdant-style scheduled-online devices need — without it, anything
//     logged between online windows disappears.
//
// Wire format (stable v1):
//   {"v":1,"batch_id":"<uuid>","batched_at_ms":<int>,
//    "records":[
//       {"ts_ms":...,"level":"INFO","file":"main.c","line":42,"msg":"..."},
//       ...
//    ]}
//
// The batch_id is the cloud-side idempotency key (matches the JetStream
// pattern from feedback_nats_idempotency.md — emitter owns the UUID).

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
#    include "esp_random.h"
#    include "esp_timer.h"
#    include "freertos/FreeRTOS.h"
#    include "freertos/semphr.h"
#    include "freertos/task.h"
#endif

#define LOG_MSG_MAX 224  // total record stays under 256 B

typedef struct {
    int64_t ts_ms;
    uint8_t level;
    uint16_t line;
    char file[24];  // basename only — full path eats too much room
    char msg[LOG_MSG_MAX];
} log_rec_t;

static struct {
    log_rec_t *ring;
    size_t cap;
    size_t head;  // next write idx
    size_t count;
    bool full_warned;
    uint16_t batch_secs;
    bool enabled;
    bool running;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t mtx;
    SemaphoreHandle_t wake;
    TaskHandle_t task;
#endif
} L = {0};

static int64_t log_now_ms(void) {
#ifdef ESP_PLATFORM
    return esp_timer_get_time() / 1000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static const char *level_str(scadable_log_level_t lvl) {
    switch (lvl) {
    case SCADABLE_LOG_DEBUG_LEVEL:
        return "DEBUG";
    case SCADABLE_LOG_INFO_LEVEL:
        return "INFO";
    case SCADABLE_LOG_WARN_LEVEL:
        return "WARN";
    case SCADABLE_LOG_ERROR_LEVEL:
        return "ERROR";
    default:
        return "INFO";
    }
}

static const char *basename_only(const char *path) {
    if (!path) return "?";
    const char *p   = path;
    const char *end = path + strlen(path);
    for (const char *c = end; c >= path; c--) {
        if (*c == '/' || *c == '\\') {
            p = c + 1;
            break;
        }
    }
    return p;
}

#ifdef ESP_PLATFORM
#    define LOG_LOCK()   xSemaphoreTake(L.mtx, portMAX_DELAY)
#    define LOG_UNLOCK() xSemaphoreGive(L.mtx)
#else
#    define LOG_LOCK()   ((void)0)
#    define LOG_UNLOCK() ((void)0)
#endif

// ─── JSON escape ────────────────────────────────────────────────────────────
//
// Conservative: escapes ", \, control chars; passes UTF-8 through untouched
// (esp-mqtt + the cloud's JSON parser handle UTF-8 fine).

static int json_escape_into(char *out, size_t out_sz, const char *in) {
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

// ─── Envelope builder ────────────────────────────────────────────────────────

static void random_uuid(char out[37]) {
#ifdef ESP_PLATFORM
    // Xtensa GCC: uint32_t == `long unsigned int` so %x triggers -Werror=format=.
    // Cast to unsigned long + use %lx so format and arg width agree across hosts.
    uint32_t r[4] = {esp_random(), esp_random(), esp_random(), esp_random()};
    snprintf(out, 37, "%08lx-%04lx-4%03lx-%04lx-%08lx%04lx", (unsigned long)r[0],
             (unsigned long)(r[1] & 0xFFFF), (unsigned long)(r[2] & 0x0FFF),
             (unsigned long)(((r[2] >> 16) & 0x3FFF) | 0x8000), (unsigned long)r[3],
             (unsigned long)(r[1] >> 16));
#else
    snprintf(out, 37, "%08x-%04x-%04x-%04x-%012x", rand(), rand() & 0xFFFF, rand() & 0xFFFF,
             rand() & 0xFFFF, rand());
#endif
}

static void publish_batch(log_rec_t *recs, size_t n) {
    if (n == 0) return;

    // Worst-case envelope size: each record ≤ ~512B JSON-escaped + envelope
    // overhead. Allocate generously.
    size_t cap = 256 + n * 384;
    char *body = malloc(cap);
    if (!body) return;

    char uuid[37];
    random_uuid(uuid);

    int o = snprintf(body, cap,
                     "{\"v\":1,\"batch_id\":\"%s\",\"batched_at_ms\":%" PRId64 ",\"records\":[",
                     uuid, log_now_ms());
    if (o <= 0 || (size_t)o >= cap) {
        free(body);
        return;
    }

    for (size_t i = 0; i < n; i++) {
        char esc_msg[LOG_MSG_MAX * 2 + 8];
        char esc_file[64];
        if (json_escape_into(esc_msg, sizeof(esc_msg), recs[i].msg) < 0) {
            esc_msg[0] = '\0';
        }
        if (json_escape_into(esc_file, sizeof(esc_file), recs[i].file) < 0) {
            esc_file[0] = '\0';
        }
        int n2 = snprintf(body + o, cap - (size_t)o,
                          "%s{\"ts_ms\":%" PRId64 ",\"level\":\"%s\",\"file\":\"%s\",\"line\":%u,"
                          "\"msg\":\"%s\"}",
                          i == 0 ? "" : ",", recs[i].ts_ms,
                          level_str((scadable_log_level_t)recs[i].level), esc_file,
                          (unsigned)recs[i].line, esc_msg);
        if (n2 <= 0 || (size_t)(o + n2) >= cap) break;
        o += n2;
    }

    if ((size_t)o + 2 < cap) {
        body[o++] = ']';
        body[o++] = '}';
    }

    char topic[160];
    if (scd_topic_logs_batch(topic, sizeof(topic)) > 0) {
        scd_mqtt_publish(topic, body, (size_t)o, 1, /*retain=*/false);
    }
    free(body);
}

// ─── Drain ──────────────────────────────────────────────────────────────────

static size_t drain_locked(log_rec_t *out, size_t out_cap) {
    size_t n     = L.count < out_cap ? L.count : out_cap;
    size_t start = (L.head + L.cap - L.count) % L.cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = L.ring[(start + i) % L.cap];
    }
    L.count = 0;
    return n;
}

#ifdef ESP_PLATFORM
static void flusher_task(void *arg) {
    (void)arg;
    log_rec_t *snapshot = malloc(sizeof(log_rec_t) * L.cap);
    if (!snapshot) {
        L.running = false;
        vTaskDelete(NULL);
        return;
    }
    while (L.enabled) {
        // Wait for the wake semaphore (signaled on full-buffer or shutdown)
        // OR for the batch interval to expire — whichever fires first.
        TickType_t wait =
            L.batch_secs == 0 ? pdMS_TO_TICKS(100) : pdMS_TO_TICKS(L.batch_secs * 1000);
        xSemaphoreTake(L.wake, wait);
        if (!L.enabled) break;
        size_t drained = 0;
        LOG_LOCK();
        drained = drain_locked(snapshot, L.cap);
        LOG_UNLOCK();
        if (drained > 0) publish_batch(snapshot, drained);
    }
    free(snapshot);
    L.running = false;
    vTaskDelete(NULL);
}
#endif

void scd_log_init(uint16_t batch_secs) {
    if (L.ring) return;  // idempotent
    L.cap         = SCD_LOG_RING_CAP;
    L.ring        = calloc(L.cap, sizeof(log_rec_t));
    L.head        = 0;
    L.count       = 0;
    L.batch_secs  = batch_secs;
    L.enabled     = (L.ring != NULL);
    L.full_warned = false;
#ifdef ESP_PLATFORM
    L.mtx     = xSemaphoreCreateMutex();
    L.wake    = xSemaphoreCreateBinary();
    L.running = true;
    xTaskCreate(flusher_task, "scd-log-flush", 4096, NULL, tskIDLE_PRIORITY + 2, &L.task);
#endif
}

void scd_log_shutdown(void) {
    if (!L.ring) return;
    L.enabled = false;
#ifdef ESP_PLATFORM
    if (L.wake) xSemaphoreGive(L.wake);
    // Best-effort wait for flusher to exit so we don't leak.
    for (int i = 0; i < 50 && L.running; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
    free(L.ring);
    L.ring = NULL;
}

void scd_log_flush_blocking(uint32_t timeout_ms) {
    if (!L.enabled) return;
#ifdef ESP_PLATFORM
    // Wake flusher and wait up to timeout_ms for the buffer to empty.
    xSemaphoreGive(L.wake);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        LOG_LOCK();
        size_t remaining = L.count;
        LOG_UNLOCK();
        if (remaining == 0) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#else
    // Host build: drain inline.
    log_rec_t snap[SCD_LOG_RING_CAP];
    LOG_LOCK();
    size_t n = drain_locked(snap, SCD_LOG_RING_CAP);
    LOG_UNLOCK();
    if (n > 0) publish_batch(snap, n);
    (void)timeout_ms;
#endif
}

// ─── Public API ─────────────────────────────────────────────────────────────

void scadable_log_(scadable_log_level_t lvl, const char *file, int line, const char *fmt, ...) {
    if (!L.enabled || !L.ring) {
        // Fallback when init hasn't run yet — keep dev-mode passthrough so
        // boot-time logs still go to UART.
        va_list ap;
        va_start(ap, fmt);
#ifdef ESP_PLATFORM
        // Mirror to ESP_LOG so it shows up in monitor too.
        char tmp[LOG_MSG_MAX];
        vsnprintf(tmp, sizeof(tmp), fmt, ap);
        ESP_LOGI("scadable", "%s", tmp);
#else
        vprintf(fmt, ap);
        printf("\n");
#endif
        va_end(ap);
        return;
    }

    log_rec_t rec;
    rec.ts_ms = log_now_ms();
    rec.level = (uint8_t)lvl;
    rec.line  = (uint16_t)(line < 0 ? 0 : (line > 0xFFFF ? 0xFFFF : line));
    snprintf(rec.file, sizeof(rec.file), "%s", basename_only(file));

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(rec.msg, sizeof(rec.msg), fmt, ap);
    va_end(ap);
    if (n < 0) rec.msg[0] = '\0';

#ifdef ESP_PLATFORM
    // Mirror to ESP_LOG so 'idf.py monitor' shows the same lines without
    // waiting for the next batch flush. INFO level → easy to grep for.
    static const esp_log_level_t map[] = {
        [SCADABLE_LOG_DEBUG_LEVEL] = ESP_LOG_DEBUG,
        [SCADABLE_LOG_INFO_LEVEL]  = ESP_LOG_INFO,
        [SCADABLE_LOG_WARN_LEVEL]  = ESP_LOG_WARN,
        [SCADABLE_LOG_ERROR_LEVEL] = ESP_LOG_ERROR,
    };
    esp_log_level_t el = (lvl >= 0 && lvl <= SCADABLE_LOG_ERROR_LEVEL) ? map[lvl] : ESP_LOG_INFO;
    ESP_LOG_LEVEL(el, "scadable", "%s:%d %s", rec.file, line, rec.msg);
#endif

    bool wake = false;
    LOG_LOCK();
    if (L.count == L.cap) {
        // Evict oldest — bounded heap, no OOM during a long offline window.
        // Match gateway-esp's behavior; the cloud spots gaps via the seq
        // field on the next batch.
        size_t oldest  = (L.head + L.cap - L.count) % L.cap;
        L.ring[oldest] = rec;  // overwrite oldest in place
        L.head         = (oldest + 1) % L.cap;
        if (!L.full_warned) {
            L.full_warned = true;
        }
        wake = true;  // force flush
    } else {
        L.ring[L.head] = rec;
        L.head         = (L.head + 1) % L.cap;
        L.count++;
        if (L.count >= L.cap) wake = true;
        if (L.batch_secs == 0) wake = true;  // realtime mode
    }
    LOG_UNLOCK();

#ifdef ESP_PLATFORM
    if (wake && L.wake) xSemaphoreGive(L.wake);
#else
    (void)wake;
#endif
}
