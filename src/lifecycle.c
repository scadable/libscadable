// lifecycle.c — init / connect / disconnect / state / event callback.
// SCADABLE 2026 · Apache-2.0
//
// STUB IMPLEMENTATION — wired up in Sprint 1 lane C. See vision doc §8.

#include "scadable.h"
#include <string.h>

static scadable_state_t s_state = SCADABLE_STATE_UNINITIALIZED;
static scadable_event_cb_t s_event_cb = NULL;
static void *s_event_cb_user = NULL;

scadable_err_t scadable_init(const scadable_config_t *cfg) {
    (void)cfg;
    if (s_state != SCADABLE_STATE_UNINITIALIZED) return SCADABLE_OK;
    // TODO: read cert from NVS namespace `scadable_certs` (set at flash time).
    // TODO: read namespace_id + broker_url from baked-in scadable_generated.h.
    // TODO: spawn internal task for MQTT client + reconnect loop.
    s_state = SCADABLE_STATE_IDLE;
    return SCADABLE_OK;
}

scadable_err_t scadable_connect(void) {
    if (s_state == SCADABLE_STATE_UNINITIALIZED) return SCADABLE_ERR_NOT_INITIALIZED;
    if (s_state == SCADABLE_STATE_CONNECTED) return SCADABLE_OK;
    // TODO: kick MQTT client, transition state to CONNECTING.
    s_state = SCADABLE_STATE_CONNECTING;
    return SCADABLE_OK;
}

scadable_err_t scadable_disconnect(void) {
    if (s_state != SCADABLE_STATE_CONNECTED) return SCADABLE_OK;
    // TODO: send LWT, flush pending PUBACKs (timeout 2s), stop reconnect.
    s_state = SCADABLE_STATE_IDLE;
    return SCADABLE_OK;
}

scadable_state_t scadable_state(void) { return s_state; }
bool scadable_is_connected(void) { return s_state == SCADABLE_STATE_CONNECTED; }

void scadable_on_event(scadable_event_cb_t cb, void *user) {
    s_event_cb = cb;
    s_event_cb_user = user;
}

const char *scadable_strerror(scadable_err_t err) {
    switch (err) {
        case SCADABLE_OK:                  return "ok";
        case SCADABLE_ERR_NOT_INITIALIZED: return "not initialized; call scadable_init() first";
        case SCADABLE_ERR_NOT_CONNECTED:   return "not connected; call scadable_connect()";
        case SCADABLE_ERR_INVALID_ARG:     return "invalid argument";
        case SCADABLE_ERR_BACKPRESSURE:    return "outbound queue full; wait for SCADABLE_EVT_PUBLISHED";
        case SCADABLE_ERR_TIMEOUT:         return "operation timed out";
        case SCADABLE_ERR_NO_CERT:         return "no client certificate in NVS; flash via web-flasher";
        case SCADABLE_ERR_TLS:             return "TLS handshake failed (see event.error.esp_tls_err)";
        case SCADABLE_ERR_NETWORK:         return "network unreachable";
        case SCADABLE_ERR_TEST_FAILED:     return "diagnostic test reported failure";
        case SCADABLE_ERR_INTERNAL:        return "internal error";
        default:                           return "unknown error";
    }
}
