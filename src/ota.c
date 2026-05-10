// ota.c — over-the-air firmware update receiver.
// SCADABLE 2026 · Apache-2.0
//
// Mirrors gateway-esp/src/handlers/ota.rs at the wire-format level so the
// cloud router doesn't special-case libscadable.
//
// Subscription: `{ns}/{gw}/ota/available` (set up in lifecycle.c on connect).
// Manifest payload (received from cloud):
//   {"version":"0.1.1","url":"https://...","sha256":"abc...","force":false,
//    "job_id":"01H..."}
//
// Flow (runs on dedicated worker task — never blocks MQTT callback):
//   1. Parse manifest. Skip if job_id was already processed this boot
//      (in-process dedup; survives reconnects, not restarts).
//   2. Skip if version matches running firmware AND !force (cloud may
//      republish a retained manifest on every reconnect).
//   3. Run all registered diagnostics — abort if any FAIL. This is the
//      "permission-gated deploy" half of the v0.1 contract: a release
//      can't roll out to a fleet member that's currently broken.
//   4. esp_https_ota — opens a writer into the inactive OTA slot. Streams
//      the firmware in 4 KiB chunks; the running app keeps serving traffic.
//   5. Verify SHA-256 against manifest. If mismatch, abort the OTA and the
//      currently-running slot stays the boot pointer (no risk).
//   6. esp_ota_set_boot_partition + reboot.
//
// Boot validation (in customer's main loop, NOT here): once the new firmware
// has been MQTT-connected for 60 s and all diagnostics pass, the customer or
// the library calls esp_ota_mark_app_valid_cancel_rollback(). If anything
// panics / fails to connect / watchdog-resets within that window, the
// bootloader reverts to the previous slot. No bricking.

#include "internal.h"
#include "scadable.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#    include "esp_app_format.h"
#    include "esp_crt_bundle.h"
#    include "esp_http_client.h"
#    include "esp_https_ota.h"
#    include "esp_log.h"
#    include "esp_ota_ops.h"
#    include "esp_partition.h"
#    include "esp_system.h"
#    include "freertos/FreeRTOS.h"
#    include "freertos/task.h"
#    include "mbedtls/sha256.h"
#endif

static const char *TAG __attribute__((unused)) = "scadable.ota";

// Dedup so a retained `ota/available` message doesn't re-trigger swap on every
// MQTT reconnect within a single boot. Survives reconnects, NOT restarts.
#define DEDUP_MAX 16
static struct {
    char job_ids[DEDUP_MAX][48];
    int count;
} OTA = {0};

void scd_ota_init(void) {
    OTA.count = 0;
}

static bool job_already_seen(const char *job_id) {
    for (int i = 0; i < OTA.count; i++) {
        if (strcmp(OTA.job_ids[i], job_id) == 0) return true;
    }
    if (OTA.count == DEDUP_MAX) {
        // Drop oldest — bounded so a misbehaving cloud can't grow this forever.
        memmove(OTA.job_ids[0], OTA.job_ids[1], (DEDUP_MAX - 1) * sizeof(OTA.job_ids[0]));
        OTA.count--;
    }
    snprintf(OTA.job_ids[OTA.count], sizeof(OTA.job_ids[0]), "%s", job_id);
    OTA.count++;
    return false;
}

// ─── Tiny manifest parser ───────────────────────────────────────────────────

typedef struct {
    char version[40];
    char url[256];
    char sha256[80];
    char job_id[48];
    bool force;
} ota_manifest_t;

// Looks for "key":"value" in payload. Returns true and copies value (no
// escapes — manifest values are simple strings). NUL-terminates value buf.
static bool extract_string_field(const char *payload, const char *key, char *out, size_t out_sz) {
    char keypat[64];
    snprintf(keypat, sizeof(keypat), "\"%s\"", key);
    const char *p = strstr(payload, keypat);
    if (!p) return false;
    p += strlen(keypat);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            out[i++] = p[1];
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return *p == '"';
}

static bool extract_bool_field(const char *payload, const char *key) {
    char keypat[64];
    snprintf(keypat, sizeof(keypat), "\"%s\"", key);
    const char *p = strstr(payload, keypat);
    if (!p) return false;
    p += strlen(keypat);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    return strncmp(p, "true", 4) == 0;
}

static bool parse_manifest(const char *payload, ota_manifest_t *m) {
    memset(m, 0, sizeof(*m));
    if (!extract_string_field(payload, "version", m->version, sizeof(m->version))) return false;
    if (!extract_string_field(payload, "url", m->url, sizeof(m->url))) return false;
    if (!extract_string_field(payload, "sha256", m->sha256, sizeof(m->sha256))) return false;
    if (!extract_string_field(payload, "job_id", m->job_id, sizeof(m->job_id))) return false;
    m->force = extract_bool_field(payload, "force");
    return true;
}

// ─── Progress publish ───────────────────────────────────────────────────────

static void publish_progress(const ota_manifest_t *m, const char *state, const char *err) {
    char topic[160];
    if (scd_topic_ota_progress(topic, sizeof(topic)) < 0) return;
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"v\":1,\"job_id\":\"%s\",\"version\":\"%s\","
                     "\"state\":\"%s\",\"error\":%s%s%s}",
                     m->job_id, m->version, state, err ? "\"" : "null", err ? err : "",
                     err ? "\"" : "");
    if (n > 0 && (size_t)n < sizeof(body)) {
        scd_mqtt_publish(topic, body, (size_t)n, 0, /*retain=*/false);
    }
}

// ─── Worker task ────────────────────────────────────────────────────────────

#ifdef ESP_PLATFORM

typedef struct {
    char *cert_pem;
    char *key_pem;
} ota_creds_t;

// SHA-256 streaming on top of esp_https_ota's chunk callback.
static struct {
    mbedtls_sha256_context sha;
    bool active;
} HASH;

static esp_err_t ota_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && HASH.active && evt->data && evt->data_len > 0) {
        mbedtls_sha256_update(&HASH.sha, (const unsigned char *)evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

static const char *hex_lower(unsigned char *bytes, size_t n, char *out, size_t out_sz) {
    static const char hex[] = "0123456789abcdef";
    if (out_sz < n * 2 + 1) return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0xF];
    }
    out[n * 2] = '\0';
    return out;
}

static void perform_ota(ota_manifest_t *m) {
    char *cert_pem = NULL;
    char *key_pem  = NULL;
    if (scd_load_certs(&cert_pem, &key_pem, NULL) != SCADABLE_OK) {
        // CDN doesn't require client cert (it's public-read), but we may need
        // it for a redirect through edge — pass it anyway when present.
        cert_pem = NULL;
        key_pem  = NULL;
    }

    mbedtls_sha256_init(&HASH.sha);
    mbedtls_sha256_starts(&HASH.sha, /*is224=*/0);
    HASH.active = true;

    esp_http_client_config_t http_cfg = {
        .url               = m->url,
        .timeout_ms        = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .event_handler     = ota_http_event,
        .client_cert_pem   = cert_pem,
        .client_key_pem    = key_pem,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err                 = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || !handle) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        publish_progress(m, "failed", esp_err_to_name(err));
        HASH.active = false;
        mbedtls_sha256_free(&HASH.sha);
        free(cert_pem);
        free(key_pem);
        return;
    }

    int total_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "OTA download size=%d", total_size);

    int last_pct = -1;
    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(handle);
        if (total_size > 0) {
            int pct = (int)((int64_t)read * 100 / total_size);
            if (pct >= last_pct + 10) {
                ESP_LOGI(TAG, "OTA progress %d%% (%d/%d)", pct, read, total_size);
                last_pct = pct;
            }
        }
    }

    HASH.active = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        publish_progress(m, "failed", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        mbedtls_sha256_free(&HASH.sha);
        free(cert_pem);
        free(key_pem);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        publish_progress(m, "failed", "incomplete download");
        esp_https_ota_abort(handle);
        mbedtls_sha256_free(&HASH.sha);
        free(cert_pem);
        free(key_pem);
        return;
    }

    // Verify SHA-256 BEFORE finalize so a hash mismatch leaves the running
    // slot untouched.
    unsigned char digest[32];
    mbedtls_sha256_finish(&HASH.sha, digest);
    mbedtls_sha256_free(&HASH.sha);
    char got[65];
    hex_lower(digest, sizeof(digest), got, sizeof(got));
    if (strcasecmp(got, m->sha256) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch: got=%s expected=%s", got, m->sha256);
        publish_progress(m, "failed", "sha256_mismatch");
        esp_https_ota_abort(handle);
        free(cert_pem);
        free(key_pem);
        return;
    }

    err = esp_https_ota_finish(handle);
    free(cert_pem);
    free(key_pem);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        publish_progress(m, "failed", esp_err_to_name(err));
        return;
    }

    publish_progress(m, "swapped", NULL);
    // Brief pause so the progress publish actually leaves the send buffer
    // before the restart kills the TCP connection.
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "OTA complete, restarting");
    esp_restart();
}

static void ota_worker_task(void *arg) {
    ota_manifest_t *m = (ota_manifest_t *)arg;
    perform_ota(m);
    free(m);
    vTaskDelete(NULL);
}

#endif  // ESP_PLATFORM

void scd_ota_handle_notify(const char *payload, size_t len) {
    (void)len;
    ota_manifest_t m;
    if (!parse_manifest(payload, &m)) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "malformed OTA manifest");
#endif
        return;
    }

    if (job_already_seen(m.job_id)) {
#ifdef ESP_PLATFORM
        ESP_LOGI(TAG, "skipping duplicate manifest job_id=%s", m.job_id);
#endif
        return;
    }

    if (strcmp(m.version, scadable_gen_build_version()) == 0 && !m.force) {
#ifdef ESP_PLATFORM
        ESP_LOGI(TAG, "skipping — manifest version=%s matches running fw (force=false)", m.version);
#endif
        return;
    }

    // Diagnostics gate. If any test fails we refuse the swap and surface the
    // reason in the dashboard. This is the "won't auto-deploy if any
    // diagnostic fails" half of the v0.1 spec.
    if (!scd_diag_run_all_blocking()) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "OTA gated by failing diagnostic — aborting swap");
#endif
        publish_progress(&m, "gated_by_diagnostic", "diagnostic_failed");
        return;
    }

    publish_progress(&m, "started", NULL);

#ifdef ESP_PLATFORM
    ota_manifest_t *heap_m = malloc(sizeof(*heap_m));
    if (!heap_m) {
        publish_progress(&m, "failed", "oom");
        return;
    }
    *heap_m = m;
    // 12 KiB stack — esp_https_ota + mbedtls SHA-256 + http_client all fit.
    if (xTaskCreate(ota_worker_task, "scd-ota", 12288, heap_m, tskIDLE_PRIORITY + 3, NULL) !=
        pdPASS) {
        publish_progress(&m, "failed", "task_spawn_failed");
        free(heap_m);
    }

    // Notify the customer their dashboard will show "OTA in progress".
    scadable_event_t evt = {
        .type          = SCADABLE_EVT_OTA_AVAILABLE,
        .ota_available = {.new_version = m.version},
    };
    scd_emit_event(&evt);
#else
    // Host build: no actual OTA possible; just emit the event for tests.
    scadable_event_t evt = {
        .type          = SCADABLE_EVT_OTA_AVAILABLE,
        .ota_available = {.new_version = m.version},
    };
    scd_emit_event(&evt);
#endif
}
