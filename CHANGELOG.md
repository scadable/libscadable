# Changelog

All notable changes to libscadable will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] — 2026-05-10

Broker verification migrated to the public-trust Mozilla root bundle. The
SCADABLE backbone now fronts `io.scadable.com:8883` with a Let's Encrypt
leaf, so chips no longer need a per-namespace CA flashed into NVS.

### Changed
- `scadable_connect()` always passes `crt_bundle_attach = esp_crt_bundle_attach`
  to the esp-mqtt config. The previous "use ca_pem if present, else fall back
  to the bundle" two-arm path is gone.
- `scd_load_certs()` lost its third `ca_pem_out` parameter — only device cert
  + key are read from NVS now.
- The `g.ca_pem` global and its NVS read in `scadable_init()` were removed.

### Migration
- Customers do not need to do anything: the function signature change is
  internal (header `internal.h`, not the public `scadable.h`).
- Provisioning flows that wrote `scadable_certs/ca_cert` to NVS keep working;
  the key is now ignored. Old chips on the field continue to verify against
  the same Mozilla bundle (it's been baked into mbedTLS via
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` since the 0.1.x line).
- Requires the dual-cert infra change in infrastructure-platform PR #24 +
  service-mqtt PR #8 to be live in production.

## [0.1.0] — 2026-05-08

First non-stub release. The previous v0.0.x cuts shipped the public header +
empty stubs so the ESP-IDF Component Registry would accept the manifest; this
release fills in the actual implementation. Customers who linked v0.0.x got a
binary that compiled but did nothing — v0.1.0 actually publishes telemetry,
streams logs, fetches env vars, runs diagnostics, and applies signed OTA.

### Added
- `scadable_init()` opens NVS, loads the device cert from namespace
  `scadable_certs`, resolves broker URL + namespace + gateway ID (config
  override → NVS → baked provisioning defaults), spins up subsystems.
- `scadable_connect()` builds an mTLS `esp-mqtt` client with an LWT presence
  payload, registers an event handler, and starts the client. Auto-reconnects
  with backoff built in.
- `scadable_disconnect()` publishes a graceful-offline retained presence,
  flushes pending QoS1 PUBACKs (up to 2s), then stops the client.
- `scadable_publish()` enqueues to `{ns}/{gw}/data/ch{N}` via
  `esp_mqtt_client_enqueue` with backpressure handling; returns
  `SCADABLE_ERR_BACKPRESSURE` when the outbound queue is full.
- `scadable_flush()` blocks until every outstanding QoS1 PUBACK lands or
  the timeout fires — the most-asked ESP32-MQTT operation, fixes the
  "publish then deep_sleep" message-loss footgun.
- `scadable_announce_offline()` publishes on a reserved `sys/offline/scheduled`
  subject so the cloud orchestrator suppresses offline alerts during
  scheduled-online windows (Verdant pattern).
- `scadable_metric_set_u32/f64` publish typed metrics on
  `{ns}/{gw}/metrics/m{N}` with a stable v1 JSON envelope.
- `SCADABLE_LOG_DEBUG/INFO/WARN/ERROR` macros feed a 128-record ring buffer
  drained by a FreeRTOS flusher task every `log_batch_secs` (default 5 s, 0 =
  realtime); offline-tolerant — buffers across MQTT reconnects.
- `scadable_env_get*` fetches runtime env vars + secrets via mTLS HTTPS GET
  to `https://edge.scadable.com/api/v1/gateways/{id}/env_vars`. In-RAM cache;
  refresh on startup + on EVT_ENV_CHANGED MQTT nudge from cloud.
- `SCADABLE_TEST(name, ctx)` macro + `scadable_register_test_()` for
  declaring diagnostic tests; results published on
  `{ns}/{gw}/diagnostics/result`. **OTA gate**: any `TEST_FAIL()` blocks the
  next `ota/available` notify from rolling out.
- OTA receiver subscribes to `{ns}/{gw}/ota/available`. On notify: parse
  manifest, dedup by `job_id`, run all diagnostics, stream firmware via
  `esp_https_ota` while computing SHA-256, verify hash, swap A/B partition,
  reboot. Mirrors gateway-esp's wire format byte-for-byte.
- Weak-symbol provisioning hooks (`scadable_gen_broker_url()` etc.) so a
  factory-emitted `scadable_generated.c` can override defaults without
  touching this library; standalone builds use the defaults inline.
- `examples/esp-idf-full-demo/` reference firmware exercising every public API.
- `tests/host/` smoke suite covering the !ESP_PLATFORM branches of every src
  file; runs in CI on Ubuntu.
- `.clang-format` config; CI now enforces formatting across `include/`,
  `src/`, `tests/`, `examples/`.

### Changed
- `idf_component.yml` version bumped 0.0.1 → 0.1.0 (first real release).
- `CMakeLists.txt` adds `ota.c` to SRCS and `app_update`, `esp_event`,
  `esp_netif`, `esp_timer`, `esp_partition`, `esp-tls` to REQUIRES.

### Notes
- v0.1 is ESP32 / ESP-IDF only. Linux (`.so`) and Cargo crate ship in v0.2.
- Public ABI is **not yet** stable. Plan: 1.0 marks the API as stable, with
  semver-promised back-compat from there.
- Boot validation (clearing the OTA pending-rollback flag after the new
  firmware proves itself) is left to the customer in v0.1 — call
  `esp_ota_mark_app_valid_cancel_rollback()` from their app once the device
  has been MQTT-connected for 60 s. v0.2 will move this into the library.
- Per-key env-change diff (only fire on actual changes vs every refresh) is
  v0.2 work; today every refresh fires the callback once per key.

## [0.0.1] — 2026-05-04

- Public API surface declared in `include/scadable.h`.
- Stub implementations for every public function (return OK, no side effects).
- ESP-IDF Component Registry manifest (`idf_component.yml`).
- `examples/esp-idf-hello-world/` reference project.
- CI workflow scaffolding.
