// env.c — runtime env vars + secrets, fetched via mTLS HTTPS from edge.
// SCADABLE 2026 · Apache-2.0
//
// Edge endpoint: GET https://edge.scadable.com/api/v1/gateways/{id}/env_vars
//   - Authenticated by the gateway's device certificate (same one used for
//     MQTT mTLS — already loaded into NVS by the web-flasher).
//   - Returns a JSON object: {"env": {"key": "value", ...}, "secrets": {...}}
//   - Cached in RAM; refreshed at startup AND on EVT_ENV_CHANGED MQTT nudge
//     from the cloud.
//
// Customers read via:
//   const char *region = scadable_env_get_or("REGION", "us-east");
//
// Secrets get the same API surface (separate accessor) so it's clear at the
// call site whether you're touching a secret. Secrets are NOT persisted to
// NVS (RAM-only) — flash dumps don't leak them. The trade-off: secrets reset
// to "unknown" on reboot until the next refresh succeeds.

#include "internal.h"
#include "scadable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#    include "esp_crt_bundle.h"
#    include "esp_http_client.h"
#    include "esp_log.h"
#    include "freertos/FreeRTOS.h"
#    include "freertos/semphr.h"
#    include "freertos/task.h"
#endif

static const char *TAG __attribute__((unused)) = "scadable.env";

// ─── In-RAM cache ────────────────────────────────────────────────────────────

typedef struct env_kv {
    char *key;
    char *value;
    bool is_secret;
    struct env_kv *next;
} env_kv_t;

static struct {
    env_kv_t *head;
    char *raw_buf;  // owned response body (kept so key/value pointers can alias)
#ifdef ESP_PLATFORM
    SemaphoreHandle_t mtx;
#endif
    scadable_env_change_cb_t change_cb;
    void *change_cb_user;
    char edge_host[96];
} E = {0};

#ifdef ESP_PLATFORM
#    define ENV_LOCK()   xSemaphoreTake(E.mtx, portMAX_DELAY)
#    define ENV_UNLOCK() xSemaphoreGive(E.mtx)
#else
#    define ENV_LOCK()   ((void)0)
#    define ENV_UNLOCK() ((void)0)
#endif

static void cache_clear_locked(void) {
    env_kv_t *n = E.head;
    while (n) {
        env_kv_t *next = n->next;
        free(n->key);
        free(n->value);
        free(n);
        n = next;
    }
    E.head = NULL;
    free(E.raw_buf);
    E.raw_buf = NULL;
}

static const env_kv_t *cache_lookup_locked(const char *key, bool secret) {
    for (env_kv_t *n = E.head; n; n = n->next) {
        if (n->is_secret == secret && strcmp(n->key, key) == 0) return n;
    }
    return NULL;
}

static void cache_insert(const char *key, const char *value, bool is_secret) {
    env_kv_t *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->key       = strdup(key);
    n->value     = strdup(value);
    n->is_secret = is_secret;
    if (!n->key || !n->value) {
        free(n->key);
        free(n->value);
        free(n);
        return;
    }
    n->next = E.head;
    E.head  = n;
}

// ─── Tiny JSON object parser ────────────────────────────────────────────────
//
// Parses {"env":{"k":"v",...},"secrets":{"k":"v",...}} in-place. Keeps the
// implementation small (no cJSON dep) — supports flat string values only,
// which matches the edge endpoint contract.

static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
        p++;
    return p;
}

// Parses a JSON string into out (NUL-terminated). Returns next char after the
// closing quote, or NULL on error. Mutates the buffer slightly (decodes
// escapes in place — caller's buffer must be writable).
static const char *parse_string(const char *p, char *out, size_t out_sz) {
    if (*p != '"') return NULL;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            switch (p[1]) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            default:
                c = p[1];
                break;
            }
            p += 2;
        } else {
            p++;
        }
        if (o + 1 < out_sz) out[o++] = c;
    }
    if (*p != '"') return NULL;
    if (o < out_sz) out[o] = '\0';
    return p + 1;
}

// Parses an object body { "k":"v", "k":"v" } and inserts each pair into the
// cache as (is_secret) entries. Returns next char after the closing brace.
static const char *parse_object_into_cache(const char *p, bool is_secret) {
    p = skip_ws(p);
    if (*p != '{') return NULL;
    p++;
    p = skip_ws(p);
    if (*p == '}') return p + 1;
    char key[96];
    char val[256];
    while (*p) {
        p = skip_ws(p);
        p = parse_string(p, key, sizeof(key));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = skip_ws(p);
        p = parse_string(p, val, sizeof(val));
        if (!p) return NULL;
        cache_insert(key, val, is_secret);
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') return p + 1;
        return NULL;
    }
    return NULL;
}

static int parse_env_response(const char *json) {
    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p++;
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') return 0;
        char key[32];
        p = parse_string(p, key, sizeof(key));
        if (!p) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        p++;
        p = skip_ws(p);
        if (strcmp(key, "env") == 0) {
            p = parse_object_into_cache(p, /*is_secret=*/false);
        } else if (strcmp(key, "secrets") == 0) {
            p = parse_object_into_cache(p, /*is_secret=*/true);
        } else {
            // Skip unknown key's value — only support objects.
            p = parse_object_into_cache(p, /*is_secret=*/false);
        }
        if (!p) return -1;
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') return 0;
        return -1;
    }
    return -1;
}

// ─── Change notification ─────────────────────────────────────────────────────
//
// After a successful refresh we fire the customer's per-key callback for
// every env key. The current cut doesn't diff against the previous snapshot
// (so customers see one fire per refresh, not just on actual changes); v0.2
// adds the diff. The lifecycle.c MQTT layer also emits a single
// SCADABLE_EVT_ENV_CHANGED event with key=NULL signaling "table refreshed."

static void scd_env_emit_changes(void) {
    scadable_env_change_cb_t cb;
    void *user;
    ENV_LOCK();
    cb   = E.change_cb;
    user = E.change_cb_user;
    ENV_UNLOCK();
    if (!cb) return;
    // Snapshot the head pointer under lock, then iterate without the lock so
    // the callback can call scadable_env_get without re-entering.
    ENV_LOCK();
    env_kv_t *cur = E.head;
    ENV_UNLOCK();
    while (cur) {
        if (!cur->is_secret) cb(cur->key, cur->value, user);
        cur = cur->next;
    }
}

// ─── Edge URL derivation ─────────────────────────────────────────────────────

static void derive_edge_host(char *out, size_t sz) {
    // Default to edge.scadable.com unless overridden via NVS or scadable_cfg.
    char buf[96];
    if (scd_nvs_get_str("scadable_cfg", "edge_host", buf, sizeof(buf)) == 0) {
        snprintf(out, sz, "%s", buf);
        return;
    }
    snprintf(out, sz, "edge.scadable.com");
}

// ─── HTTPS fetch (ESP only) ──────────────────────────────────────────────────

#ifdef ESP_PLATFORM

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} http_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt) {
    http_buf_t *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        size_t needed = b->len + (size_t)evt->data_len + 1;
        if (needed > b->cap) {
            size_t newcap = b->cap ? b->cap : 1024;
            while (newcap < needed)
                newcap *= 2;
            char *nb = realloc(b->buf, newcap);
            if (!nb) return ESP_FAIL;
            b->buf = nb;
            b->cap = newcap;
        }
        memcpy(b->buf + b->len, evt->data, (size_t)evt->data_len);
        b->len += (size_t)evt->data_len;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

int scd_env_refresh_blocking(void) {
    const char *gw = scd_gateway_id();
    if (!gw) return -1;

    // mTLS HTTPS GET. Same client cert as MQTT — read directly from NVS so we
    // don't have to thread it through global state.
    char *cert_pem = NULL;
    char *key_pem  = NULL;
    if (scd_load_certs(&cert_pem, &key_pem) != SCADABLE_OK) {
        ESP_LOGW(TAG, "no client cert; skipping env refresh");
        return -1;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s/api/v1/gateways/%s/env_vars",
             E.edge_host[0] ? E.edge_host : "edge.scadable.com", gw);

    http_buf_t body              = {0};
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .client_cert_pem   = cert_pem,
        .client_key_pem    = key_pem,
        .event_handler     = http_event,
        .user_data         = &body,
        .method            = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(cert_pem);
        free(key_pem);
        free(body.buf);
        return -1;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(cert_pem);
    free(key_pem);

    if (err != ESP_OK || status / 100 != 2) {
        ESP_LOGW(TAG, "env refresh failed: %s status=%d", esp_err_to_name(err), status);
        free(body.buf);
        return -1;
    }

    ENV_LOCK();
    cache_clear_locked();
    int rc = parse_env_response(body.buf ? body.buf : "{}");
    if (rc != 0) {
        ESP_LOGW(TAG, "env response parse failed");
        cache_clear_locked();
    }
    E.raw_buf = body.buf;  // keep alive for cache_insert strdup'd values
    ENV_UNLOCK();

    // Notify subscribers that the env table changed. We emit one per key (the
    // pre-refresh diff isn't tracked yet — v0.2 will diff so we only fire on
    // actual changes) — the customer can filter by key in their callback.
    if (rc == 0) scd_env_emit_changes();
    return rc;
}

#else  // host build — no network, populates from a fixed test stub if set

static const char *host_test_blob;
int scd_env_refresh_blocking(void) {
    if (!host_test_blob) return 0;
    ENV_LOCK();
    cache_clear_locked();
    int rc = parse_env_response(host_test_blob);
    ENV_UNLOCK();
    if (rc == 0) scd_env_emit_changes();
    return rc;
}
// Test hook — set env response body for host build (tests/test_libscadable.c).
void scd_env_test_set_response(const char *json) {
    host_test_blob = json;
}

#endif  // ESP_PLATFORM

void scd_env_init(void) {
#ifdef ESP_PLATFORM
    if (!E.mtx) E.mtx = xSemaphoreCreateMutex();
#endif
    derive_edge_host(E.edge_host, sizeof(E.edge_host));
    // Don't block init on the network — refresh fires async on first
    // MQTT_EVENT_CONNECTED (in lifecycle.c). The customer's app_main() returns
    // immediately.
    // TODO(v0.2): kick off a one-shot background refresh task here so
    // env vars are populated even if MQTT is slow to connect.
}

void scd_env_shutdown(void) {
    ENV_LOCK();
    cache_clear_locked();
    ENV_UNLOCK();
}

// ─── Public API ─────────────────────────────────────────────────────────────

const char *scadable_env_get(const char *key) {
    if (!key) return NULL;
    const char *out = NULL;
    ENV_LOCK();
    const env_kv_t *n = cache_lookup_locked(key, /*secret=*/false);
    if (n) out = n->value;
    ENV_UNLOCK();
    return out;
}

const char *scadable_env_get_or(const char *key, const char *fallback) {
    const char *v = scadable_env_get(key);
    return v ? v : fallback;
}

int32_t scadable_env_get_int(const char *key, int32_t fallback) {
    const char *v = scadable_env_get(key);
    if (!v) return fallback;
    char *end;
    long n = strtol(v, &end, 10);
    return (end != v && *end == '\0') ? (int32_t)n : fallback;
}

double scadable_env_get_double(const char *key, double fallback) {
    const char *v = scadable_env_get(key);
    if (!v) return fallback;
    char *end;
    double n = strtod(v, &end);
    return (end != v && *end == '\0') ? n : fallback;
}

bool scadable_env_get_bool(const char *key, bool fallback) {
    const char *v = scadable_env_get(key);
    if (!v) return fallback;
    if (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0) return true;
    if (strcmp(v, "false") == 0 || strcmp(v, "0") == 0 || strcmp(v, "no") == 0) return false;
    return fallback;
}

const char *scadable_secret_get(const char *key) {
    if (!key) return NULL;
    const char *out = NULL;
    ENV_LOCK();
    const env_kv_t *n = cache_lookup_locked(key, /*secret=*/true);
    if (n) out = n->value;
    ENV_UNLOCK();
    return out;
}

const char *scadable_secret_get_or(const char *key, const char *fallback) {
    const char *v = scadable_secret_get(key);
    return v ? v : fallback;
}

void scadable_on_env_change(scadable_env_change_cb_t cb, void *user) {
    ENV_LOCK();
    E.change_cb      = cb;
    E.change_cb_user = user;
    ENV_UNLOCK();
}
