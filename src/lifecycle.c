// lifecycle.c — init / connect / disconnect / state / event callback.
// SCADABLE 2026 · Apache-2.0
//
// Owns the MQTT client lifetime. Init order:
//   1. NVS open
//   2. Load broker URL + namespace + gateway_id (config overrides → NVS → baked
//      provisioning defaults from scadable_generated.c)
//   3. Load device cert + key from `scadable_certs` NVS namespace
//   4. Build LWT presence payload
//   5. Spin up esp-mqtt client (non-blocking; auto-reconnect built in)
//   6. Subscribe to OTA notify, env_vars change, command topics
//   7. Start log batcher, env cache, diagnostics subsystems
//
// The customer's task NEVER blocks here past `scadable_init()` — every
// long-running operation (MQTT loop, OTA download, log flush, env refresh)
// runs on its own FreeRTOS task.

#include "internal.h"
#include "scadable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#    include "esp_event.h"
#    include "esp_log.h"
#    include "esp_mac.h"
#    include "esp_netif.h"
#    include "esp_system.h"
#    include "esp_tls.h"
#    include "freertos/FreeRTOS.h"
#    include "freertos/semphr.h"
#    include "freertos/task.h"
#    include "mqtt_client.h"
#    include "nvs.h"
#    include "nvs_flash.h"
#endif

static const char *TAG __attribute__((unused)) = "scadable.lifecycle";

// ─── Module state ────────────────────────────────────────────────────────────

static struct {
    scadable_state_t state;
    scadable_event_cb_t event_cb;
    void *event_cb_user;
    char gateway_id[64];
    char namespace_id[64];
    char broker_url[160];
    uint16_t keepalive_secs;
    uint16_t log_batch_secs;
    char *cert_pem;         // owned, freed at shutdown
    char *key_pem;          // owned, freed at shutdown
    void *mqtt;             // esp_mqtt_client_handle_t on ESP, NULL on host
    uint32_t pending_qos1;  // outstanding PUBACKs
    uint32_t recovered_count;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t state_mtx;
    SemaphoreHandle_t event_mtx;
    SemaphoreHandle_t pending_mtx;
    SemaphoreHandle_t pending_drained_sem;
#endif
} g = {
    .state           = SCADABLE_STATE_UNINITIALIZED,
    .keepalive_secs  = SCD_DEFAULT_KEEPALIVE_SECS,
    .log_batch_secs  = SCD_DEFAULT_LOG_BATCH_SECS,
    .recovered_count = 0,
};

// ─── Mutex helpers (cheap no-ops on host) ────────────────────────────────────

#ifdef ESP_PLATFORM
#    define LOCK(m)   xSemaphoreTake((m), portMAX_DELAY)
#    define UNLOCK(m) xSemaphoreGive((m))
#else
#    define LOCK(m)   ((void)(m))
#    define UNLOCK(m) ((void)(m))
#endif

// ─── Weak provisioning defaults — overridden by scadable_generated.c ─────────

__attribute__((weak)) const char *scadable_gen_broker_url(void) {
#ifdef SCADABLE_BROKER_URL
    return SCADABLE_BROKER_URL;
#else
    return "mqtts://io.scadable.com:8883";
#endif
}
__attribute__((weak)) const char *scadable_gen_namespace_id(void) {
#ifdef SCADABLE_NAMESPACE_ID
    return SCADABLE_NAMESPACE_ID;
#else
    return "ns_unset";
#endif
}
__attribute__((weak)) const char *scadable_gen_target(void) {
#ifdef SCADABLE_TARGET
    return SCADABLE_TARGET;
#else
    return "esp32";
#endif
}
__attribute__((weak)) const char *scadable_gen_build_version(void) {
#ifdef SCADABLE_BUILD_VERSION
    return SCADABLE_BUILD_VERSION;
#else
    return "v0.1.0-standalone";
#endif
}
__attribute__((weak)) const char *scadable_gen_build_sha(void) {
#ifdef SCADABLE_BUILD_SHA
    return SCADABLE_BUILD_SHA;
#else
    return "unknown";
#endif
}

// ─── Internal accessors for other modules ────────────────────────────────────

scadable_state_t scd_state_get(void) {
    return g.state;
}
void scd_state_set(scadable_state_t s) {
#ifdef ESP_PLATFORM
    LOCK(g.state_mtx);
#endif
    g.state = s;
#ifdef ESP_PLATFORM
    UNLOCK(g.state_mtx);
#endif
}
const char *scd_gateway_id(void) {
    return g.gateway_id[0] ? g.gateway_id : NULL;
}
const char *scd_namespace_id(void) {
    return g.namespace_id[0] ? g.namespace_id : NULL;
}
const char *scd_broker_url(void) {
    return g.broker_url[0] ? g.broker_url : NULL;
}
void *scd_mqtt_handle(void) {
    return g.mqtt;
}
void scd_mqtt_handle_set(void *h) {
    g.mqtt = h;
}

void scd_pending_inc(void) {
#ifdef ESP_PLATFORM
    LOCK(g.pending_mtx);
    g.pending_qos1++;
    UNLOCK(g.pending_mtx);
#else
    g.pending_qos1++;
#endif
}
void scd_pending_dec(int32_t msg_id) {
    (void)msg_id;
#ifdef ESP_PLATFORM
    LOCK(g.pending_mtx);
    if (g.pending_qos1 > 0) g.pending_qos1--;
    if (g.pending_qos1 == 0 && g.pending_drained_sem) {
        xSemaphoreGive(g.pending_drained_sem);
    }
    UNLOCK(g.pending_mtx);
#else
    if (g.pending_qos1 > 0) g.pending_qos1--;
#endif
}
uint32_t scd_pending_count(void) {
    return g.pending_qos1;
}

void scd_emit_event(const scadable_event_t *evt) {
    if (!evt) return;
    scadable_event_cb_t cb;
    void *user;
#ifdef ESP_PLATFORM
    LOCK(g.event_mtx);
#endif
    cb   = g.event_cb;
    user = g.event_cb_user;
#ifdef ESP_PLATFORM
    UNLOCK(g.event_mtx);
#endif
    if (cb) cb(*evt, user);
}

// ─── Topic helpers ──────────────────────────────────────────────────────────

#define TOPIC_FMT(out, sz, fmt, ...)                                                               \
    do {                                                                                           \
        const char *ns = scd_namespace_id();                                                       \
        const char *gw = scd_gateway_id();                                                         \
        if (!ns || !gw) return -1;                                                                 \
        int n = snprintf((out), (sz), (fmt), ns, gw, ##__VA_ARGS__);                               \
        return (n < 0 || (size_t)n >= (sz)) ? -1 : n;                                              \
    } while (0)

int scd_topic_data(char *out, size_t sz, uint32_t channel) {
    TOPIC_FMT(out, sz, "%s/%s/data/ch%u", (unsigned)channel);
}
int scd_topic_metric(char *out, size_t sz, uint32_t metric) {
    TOPIC_FMT(out, sz, "%s/%s/metrics/m%u", (unsigned)metric);
}
int scd_topic_logs_batch(char *out, size_t sz) {
    TOPIC_FMT(out, sz, "%s/%s/sys/logs/batch");
}
int scd_topic_diag_result(char *out, size_t sz) {
    TOPIC_FMT(out, sz, "%s/%s/diagnostics/result");
}
int scd_topic_presence(char *out, size_t sz) {
    TOPIC_FMT(out, sz, "%s/%s/presence");
}
int scd_topic_offline_announce(char *out, size_t sz) {
    TOPIC_FMT(out, sz, "%s/%s/sys/offline/scheduled");
}
int scd_topic_subscribe_ota(char *out, size_t sz) {
    TOPIC_FMT(out, sz, "%s/%s/ota/available");
}
int scd_topic_subscribe_env(char *out, size_t sz) {
    TOPIC_FMT(out, sz, "%s/%s/sys/env_vars");
}
int scd_topic_subscribe_cmd(char *out, size_t sz) {
    TOPIC_FMT(out, sz, "%s/%s/cmd/+");
}
int scd_topic_ota_progress(char *out, size_t sz) {
    TOPIC_FMT(out, sz, "%s/%s/ota/progress");
}

bool scd_topic_is_ota_notify(const char *topic) {
    char buf[160];
    if (scd_topic_subscribe_ota(buf, sizeof(buf)) < 0) return false;
    return strcmp(topic, buf) == 0;
}
bool scd_topic_is_env_change(const char *topic) {
    char buf[160];
    if (scd_topic_subscribe_env(buf, sizeof(buf)) < 0) return false;
    return strcmp(topic, buf) == 0;
}

// ─── NVS wrappers ───────────────────────────────────────────────────────────

#ifdef ESP_PLATFORM

int scd_nvs_get_str(const char *ns, const char *key, char *out, size_t out_size) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t sz     = out_size;
    esp_err_t err = nvs_get_str(h, key, out, &sz);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

int scd_nvs_get_blob(const char *ns, const char *key, void *out, size_t *out_size) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_get_blob(h, key, out, out_size);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

int scd_nvs_set_str(const char *ns, const char *key, const char *value) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

scadable_err_t scd_load_certs(char **cert_out, char **key_out) {
    nvs_handle_t h;
    if (nvs_open("scadable_certs", NVS_READONLY, &h) != ESP_OK) {
        return SCADABLE_ERR_NO_CERT;
    }
    size_t cert_sz = 0;
    size_t key_sz  = 0;
    if (nvs_get_str(h, "device_cert", NULL, &cert_sz) != ESP_OK) {
        nvs_close(h);
        return SCADABLE_ERR_NO_CERT;
    }
    if (nvs_get_str(h, "device_key", NULL, &key_sz) != ESP_OK) {
        nvs_close(h);
        return SCADABLE_ERR_NO_CERT;
    }
    char *cert = malloc(cert_sz);
    char *key  = malloc(key_sz);
    if (!cert || !key) {
        free(cert);
        free(key);
        nvs_close(h);
        return SCADABLE_ERR_INTERNAL;
    }
    if (nvs_get_str(h, "device_cert", cert, &cert_sz) != ESP_OK ||
        nvs_get_str(h, "device_key", key, &key_sz) != ESP_OK) {
        free(cert);
        free(key);
        nvs_close(h);
        return SCADABLE_ERR_NO_CERT;
    }
    nvs_close(h);
    *cert_out = cert;
    *key_out  = key;
    return SCADABLE_OK;
}

#else  // host build — returns "missing" so tests can stub via direct setters

int scd_nvs_get_str(const char *ns, const char *key, char *out, size_t out_size) {
    (void)ns;
    (void)key;
    (void)out;
    (void)out_size;
    return -1;
}
int scd_nvs_get_blob(const char *ns, const char *key, void *out, size_t *out_size) {
    (void)ns;
    (void)key;
    (void)out;
    (void)out_size;
    return -1;
}
int scd_nvs_set_str(const char *ns, const char *key, const char *value) {
    (void)ns;
    (void)key;
    (void)value;
    return 0;
}
scadable_err_t scd_load_certs(char **cert_out, char **key_out) {
    (void)cert_out;
    (void)key_out;
    return SCADABLE_ERR_NO_CERT;
}

#endif  // ESP_PLATFORM

// ─── Gateway ID derivation ──────────────────────────────────────────────────

static void derive_gateway_id(char *out, size_t sz) {
#ifdef ESP_PLATFORM
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out, sz, "esp-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
        return;
    }
#endif
    snprintf(out, sz, "esp-unknown");
}

// ─── MQTT event handler (ESP only) ──────────────────────────────────────────

#ifdef ESP_PLATFORM
static void
on_mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        scd_state_set(SCADABLE_STATE_CONNECTED);
        g.recovered_count++;
        ESP_LOGI(TAG, "mqtt connected (recovered_count=%u)", (unsigned)g.recovered_count);

        // Re-subscribe — clean session means subs don't survive reconnect.
        char tbuf[160];
        if (scd_topic_subscribe_ota(tbuf, sizeof(tbuf)) > 0) {
            esp_mqtt_client_subscribe(g.mqtt, tbuf, 1);
        }
        if (scd_topic_subscribe_env(tbuf, sizeof(tbuf)) > 0) {
            esp_mqtt_client_subscribe(g.mqtt, tbuf, 1);
        }
        if (scd_topic_subscribe_cmd(tbuf, sizeof(tbuf)) > 0) {
            esp_mqtt_client_subscribe(g.mqtt, tbuf, 1);
        }

        // Publish online presence (retained).
        if (scd_topic_presence(tbuf, sizeof(tbuf)) > 0) {
            char body[256];
            int n = snprintf(body, sizeof(body),
                             "{\"status\":\"online\",\"fw_version\":\"%s\","
                             "\"hostname\":\"%s\",\"target\":\"%s\"}",
                             scadable_gen_build_version(), scd_gateway_id(), scadable_gen_target());
            if (n > 0 && (size_t)n < sizeof(body)) {
                esp_mqtt_client_publish(g.mqtt, tbuf, body, n, 1, 1);
            }
        }

        scadable_event_t evt = {
            .type      = SCADABLE_EVT_CONNECTED,
            .connected = {.recovered_count = g.recovered_count},
        };
        scd_emit_event(&evt);
        break;
    }
    case MQTT_EVENT_DISCONNECTED: {
        scd_state_set(SCADABLE_STATE_CONNECTING);  // esp-mqtt will reconnect
        scadable_event_t evt = {.type = SCADABLE_EVT_DISCONNECTED};
        scd_emit_event(&evt);
        break;
    }
    case MQTT_EVENT_PUBLISHED: {
        scd_pending_dec(e->msg_id);
        scadable_event_t evt = {
            .type      = SCADABLE_EVT_PUBLISHED,
            .published = {.msg_id = e->msg_id},
        };
        scd_emit_event(&evt);
        break;
    }
    case MQTT_EVENT_DATA: {
        // Build NUL-terminated topic + payload copies (esp-mqtt buffers
        // are not NUL-terminated and may be reused after this callback).
        if (!e->topic || e->topic_len <= 0) break;
        char *topic = malloc((size_t)e->topic_len + 1);
        char *body  = malloc((size_t)e->data_len + 1);
        if (!topic || !body) {
            free(topic);
            free(body);
            break;
        }
        memcpy(topic, e->topic, (size_t)e->topic_len);
        topic[e->topic_len] = '\0';
        memcpy(body, e->data, (size_t)e->data_len);
        body[e->data_len] = '\0';

        if (scd_topic_is_ota_notify(topic)) {
            scd_ota_handle_notify(body, (size_t)e->data_len);
        } else if (scd_topic_is_env_change(topic)) {
            // Cloud nudged us — re-fetch the env table and emit
            // EVT_ENV_CHANGED for each key the customer subscribed to.
            scd_env_refresh_blocking();
        } else {
            ESP_LOGD(TAG, "unhandled topic: %s", topic);
        }

        free(topic);
        free(body);
        break;
    }
    case MQTT_EVENT_ERROR: {
        ESP_LOGW(TAG, "mqtt error type=%d", e->error_handle->error_type);
        scadable_event_t evt = {
            .type  = SCADABLE_EVT_ERROR,
            .error = {.code              = SCADABLE_ERR_NETWORK,
                      .esp_tls_err       = e->error_handle->esp_tls_last_esp_err,
                      .mbedtls_err       = e->error_handle->esp_tls_stack_err,
                      .cert_verify_flags = (uint32_t)e->error_handle->esp_tls_cert_verify_flags,
                      .retriable         = true},
        };
        scd_emit_event(&evt);
        break;
    }
    default:
        break;
    }
}
#endif  // ESP_PLATFORM

// ─── Public API ─────────────────────────────────────────────────────────────

scadable_err_t scadable_init(const scadable_config_t *cfg) {
    if (g.state != SCADABLE_STATE_UNINITIALIZED) return SCADABLE_OK;

#ifdef ESP_PLATFORM
    // 1. NVS — must be open before we can read certs / config.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return SCADABLE_ERR_INTERNAL;
    }

    // Lazy-init synchronization primitives once.
    if (!g.state_mtx) g.state_mtx = xSemaphoreCreateMutex();
    if (!g.event_mtx) g.event_mtx = xSemaphoreCreateMutex();
    if (!g.pending_mtx) g.pending_mtx = xSemaphoreCreateMutex();
    if (!g.pending_drained_sem) g.pending_drained_sem = xSemaphoreCreateBinary();
    if (!g.state_mtx || !g.event_mtx || !g.pending_mtx || !g.pending_drained_sem) {
        return SCADABLE_ERR_INTERNAL;
    }
#endif

    // 2. Resolve config: caller override → NVS → baked defaults.
    const char *broker = (cfg && cfg->broker_url) ? cfg->broker_url : NULL;
    const char *dev_id = (cfg && cfg->device_id) ? cfg->device_id : NULL;
    g.keepalive_secs =
        (cfg && cfg->keepalive_secs) ? cfg->keepalive_secs : SCD_DEFAULT_KEEPALIVE_SECS;
    g.log_batch_secs =
        (cfg && cfg->log_batch_secs) ? cfg->log_batch_secs : SCD_DEFAULT_LOG_BATCH_SECS;

    char nvs_buf[160];
    if (!broker && scd_nvs_get_str("scadable_cfg", "broker_url", nvs_buf, sizeof(nvs_buf)) == 0) {
        broker = nvs_buf;  // copied below, safe to alias scope
    }
    if (!broker) broker = scadable_gen_broker_url();
    snprintf(g.broker_url, sizeof(g.broker_url), "%s", broker);

    char ns_buf[64];
    if (scd_nvs_get_str("scadable_cfg", "namespace_id", ns_buf, sizeof(ns_buf)) == 0) {
        snprintf(g.namespace_id, sizeof(g.namespace_id), "%s", ns_buf);
    } else {
        snprintf(g.namespace_id, sizeof(g.namespace_id), "%s", scadable_gen_namespace_id());
    }

    if (dev_id) {
        snprintf(g.gateway_id, sizeof(g.gateway_id), "%s", dev_id);
    } else if (scd_nvs_get_str("scadable_cfg", "gateway_id", g.gateway_id, sizeof(g.gateway_id)) !=
               0) {
        derive_gateway_id(g.gateway_id, sizeof(g.gateway_id));
        // Persist so the dashboard sees the same ID across reboots.
        scd_nvs_set_str("scadable_cfg", "gateway_id", g.gateway_id);
    }

    // 3. Load certs (required for mTLS — but not for host tests).
#ifdef ESP_PLATFORM
    scadable_err_t cert_err = scd_load_certs(&g.cert_pem, &g.key_pem);
    if (cert_err != SCADABLE_OK) {
        ESP_LOGE(TAG, "no client cert in NVS namespace 'scadable_certs' — "
                      "flash one via the SCADABLE web-flasher");
        return cert_err;
    }
#endif

    // 4. Subsystems (independent — failure of any one shouldn't block init).
    scd_log_init(g.log_batch_secs);
    scd_env_init();
    scd_diag_init();
    scd_ota_init();

    g.state = SCADABLE_STATE_IDLE;
    return SCADABLE_OK;
}

scadable_err_t scadable_connect(void) {
    if (g.state == SCADABLE_STATE_UNINITIALIZED) return SCADABLE_ERR_NOT_INITIALIZED;
    if (g.state == SCADABLE_STATE_CONNECTED) return SCADABLE_OK;

#ifdef ESP_PLATFORM
    if (g.mqtt) {
        // Already exists (was disconnected) — just kick reconnect.
        scd_state_set(SCADABLE_STATE_CONNECTING);
        return esp_mqtt_client_reconnect(g.mqtt) == ESP_OK ? SCADABLE_OK : SCADABLE_ERR_INTERNAL;
    }

    // LWT presence payload — broker fires this on unexpected disconnect so
    // the dashboard flips to red without waiting for our keepalive.
    char lwt_topic[160];
    if (scd_topic_presence(lwt_topic, sizeof(lwt_topic)) < 0) return SCADABLE_ERR_INTERNAL;
    char lwt_payload[200];
    int lwt_n = snprintf(lwt_payload, sizeof(lwt_payload),
                         "{\"status\":\"offline\",\"reason\":\"unexpected\","
                         "\"fw_version\":\"%s\",\"hostname\":\"%s\"}",
                         scadable_gen_build_version(), scd_gateway_id());
    if (lwt_n <= 0 || (size_t)lwt_n >= sizeof(lwt_payload)) return SCADABLE_ERR_INTERNAL;

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                     = g.broker_url,
        .broker.verification.crt_bundle_attach  = NULL,  // mTLS → use our CA below
        .credentials.client_id                  = g.gateway_id,
        .credentials.authentication.certificate = g.cert_pem,
        .credentials.authentication.key         = g.key_pem,
        .session.keepalive                      = g.keepalive_secs,
        .session.last_will.topic                = lwt_topic,
        .session.last_will.msg                  = lwt_payload,
        .session.last_will.msg_len              = lwt_n,
        .session.last_will.qos                  = 1,
        .session.last_will.retain               = 1,
        .network.disable_auto_reconnect         = false,
        .network.reconnect_timeout_ms           = 5000,
    };

    g.mqtt = esp_mqtt_client_init(&mqtt_cfg);
    if (!g.mqtt) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        scd_state_set(SCADABLE_STATE_ERROR);
        return SCADABLE_ERR_INTERNAL;
    }

    esp_mqtt_client_register_event(g.mqtt, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);

    scd_state_set(SCADABLE_STATE_CONNECTING);
    if (esp_mqtt_client_start(g.mqtt) != ESP_OK) {
        scd_state_set(SCADABLE_STATE_ERROR);
        return SCADABLE_ERR_INTERNAL;
    }
    return SCADABLE_OK;
#else
    scd_state_set(SCADABLE_STATE_CONNECTING);
    return SCADABLE_OK;
#endif
}

scadable_err_t scadable_disconnect(void) {
    if (g.state == SCADABLE_STATE_UNINITIALIZED) return SCADABLE_ERR_NOT_INITIALIZED;
    if (g.state != SCADABLE_STATE_CONNECTED && g.state != SCADABLE_STATE_CONNECTING) {
        return SCADABLE_OK;
    }
    scd_state_set(SCADABLE_STATE_DISCONNECTING);
    // Best-effort flush of pending PUBACKs before tearing down the socket.
    scadable_flush(2000);
#ifdef ESP_PLATFORM
    if (g.mqtt) {
        // Publish graceful-offline retained presence so the dashboard
        // doesn't flash a red dot for the brief shutdown window.
        char tbuf[160];
        if (scd_topic_presence(tbuf, sizeof(tbuf)) > 0) {
            char body[200];
            int n = snprintf(body, sizeof(body),
                             "{\"status\":\"offline\",\"reason\":\"graceful\","
                             "\"fw_version\":\"%s\",\"hostname\":\"%s\"}",
                             scadable_gen_build_version(), scd_gateway_id());
            if (n > 0 && (size_t)n < sizeof(body)) {
                esp_mqtt_client_publish(g.mqtt, tbuf, body, n, 1, 1);
            }
        }
        esp_mqtt_client_stop(g.mqtt);
    }
#endif
    scd_state_set(SCADABLE_STATE_IDLE);
    return SCADABLE_OK;
}

scadable_state_t scadable_state(void) {
    return scd_state_get();
}

bool scadable_is_connected(void) {
    return scd_state_get() == SCADABLE_STATE_CONNECTED;
}

void scadable_on_event(scadable_event_cb_t cb, void *user) {
#ifdef ESP_PLATFORM
    if (g.event_mtx) LOCK(g.event_mtx);
#endif
    g.event_cb      = cb;
    g.event_cb_user = user;
#ifdef ESP_PLATFORM
    if (g.event_mtx) UNLOCK(g.event_mtx);
#endif
}

const char *scadable_strerror(scadable_err_t err) {
    switch (err) {
    case SCADABLE_OK:
        return "ok";
    case SCADABLE_ERR_NOT_INITIALIZED:
        return "not initialized; call scadable_init() first";
    case SCADABLE_ERR_NOT_CONNECTED:
        return "not connected; call scadable_connect()";
    case SCADABLE_ERR_INVALID_ARG:
        return "invalid argument";
    case SCADABLE_ERR_BACKPRESSURE:
        return "outbound queue full; wait for SCADABLE_EVT_PUBLISHED";
    case SCADABLE_ERR_TIMEOUT:
        return "operation timed out";
    case SCADABLE_ERR_NO_CERT:
        return "no client certificate in NVS; flash via web-flasher";
    case SCADABLE_ERR_TLS:
        return "TLS handshake failed (see event.error.esp_tls_err)";
    case SCADABLE_ERR_NETWORK:
        return "network unreachable";
    case SCADABLE_ERR_TEST_FAILED:
        return "diagnostic test reported failure";
    case SCADABLE_ERR_INTERNAL:
        return "internal error";
    default:
        return "unknown error";
    }
}
