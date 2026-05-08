// env.c — env vars + secrets (delivered at runtime via mTLS HTTPS pull from edge).
// SCADABLE 2026 · Apache-2.0
// STUB IMPLEMENTATION — wired up in Sprint 1 lane F (env+secrets) + lane C (libscadable).

#include "scadable.h"
#include <string.h>
#include <stdlib.h>

static scadable_env_change_cb_t s_env_cb = NULL;
static void *s_env_cb_user = NULL;

const char *scadable_env_get(const char *key) {
    (void)key;
    // TODO: lookup in in-memory cache populated by mTLS HTTPS pull from
    // edge endpoint GET /internal/env.
    return NULL;
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
    return (*end == '\0') ? (int32_t)n : fallback;
}

double scadable_env_get_double(const char *key, double fallback) {
    const char *v = scadable_env_get(key);
    if (!v) return fallback;
    char *end;
    double n = strtod(v, &end);
    return (*end == '\0') ? n : fallback;
}

bool scadable_env_get_bool(const char *key, bool fallback) {
    const char *v = scadable_env_get(key);
    if (!v) return fallback;
    if (strcmp(v, "true")==0||strcmp(v, "1")==0||strcmp(v, "yes")==0)  return true;
    if (strcmp(v, "false")==0||strcmp(v, "0")==0||strcmp(v, "no")==0) return false;
    return fallback;
}

const char *scadable_secret_get(const char *key) {
    (void)key;
    // TODO: lookup in in-memory cache populated by mTLS HTTPS pull from
    // edge endpoint GET /internal/secrets. Cache is encrypted-NVS-persisted
    // so secrets survive reboot + OTA (per vision doc A5: ESP32 flash encryption).
    return NULL;
}

const char *scadable_secret_get_or(const char *key, const char *fallback) {
    const char *v = scadable_secret_get(key);
    return v ? v : fallback;
}

void scadable_on_env_change(scadable_env_change_cb_t cb, void *user) {
    s_env_cb = cb;
    s_env_cb_user = user;
}
