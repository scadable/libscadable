# Migration Guide

Notes for upgrading between libscadable releases. Newest first.

## v0.0.1 → v0.1.0

The headline: v0.0.x was stubs that returned `OK` and did nothing. v0.1.0
actually implements the public API. There are no source-code changes
required in your firmware — every public function in `include/scadable.h`
keeps the same signature and same return-code contract — but the runtime
behavior changes substantially.

### What you get for free

If your code already calls the documented sequence:

```c
scadable_init(NULL);
scadable_connect();
scadable_publish(SCADABLE_CH_HELLO, payload, len, 1);
```

…then upgrading to v0.1.0 is a matter of bumping the dependency:

```yaml
# idf_component.yml
dependencies:
  scadable/libscadable: "^0.1.0"   # was ^0.0.1
```

Rebuild and reflash. Your device now actually publishes telemetry, streams
logs, and accepts OTA — the same call sites that did nothing in v0.0.x now do
the right thing.

### New runtime requirements

These weren't enforced by v0.0.x because nothing connected. v0.1.0 enforces
them:

1. **Device certificate must be in NVS namespace `scadable_certs`** under
   keys `device_cert` (PEM string) and `device_key` (PEM string). The
   SCADABLE web-flasher writes these at flash time. If they're missing,
   `scadable_init()` returns `SCADABLE_ERR_NO_CERT`.

2. **WiFi must be up before `scadable_connect()`.** v0.0.x returned `OK`
   regardless; v0.1.0 actually tries to open a TCP socket. Bring up wifi
   in `app_main()` before calling `scadable_init()`.

3. **The CMakeLists.txt in this component now requires `app_update`,
   `esp_event`, `esp_netif`, `esp_timer`, `esp_partition`, and `esp-tls`.**
   These are stock ESP-IDF components — nothing to install — but if your
   project pins old `idf_component.yml` lockfiles you may need to refresh.

### Behavioral changes that may surprise you

- **`SCADABLE_LOG_*` macros are now batched.** v0.0.x printed inline via
  `vprintf`; v0.1.0 buffers in a 128-record ring and flushes every
  `log_batch_secs` (default 5 s). Set `cfg.log_batch_secs = 0` to restore
  realtime behavior. The log lines still mirror to ESP-IDF's UART logger
  via `ESP_LOG_LEVEL` so `idf.py monitor` still shows them.

- **`scadable_publish()` returns `SCADABLE_ERR_BACKPRESSURE` when the outbound
  queue is full.** v0.0.x always returned `OK`. Loops that ignored the return
  code will now silently drop on a slow link. The fix: subscribe to
  `SCADABLE_EVT_PUBLISHED` and gate your next publish on it, or sleep a few
  ms and retry.

- **`scadable_flush()` now actually waits.** v0.0.x returned `OK` immediately;
  v0.1.0 blocks up to `timeout_ms` for outstanding QoS1 PUBACKs to land. Call
  this before `esp_deep_sleep_start()` if you publish QoS1.

- **OTA notify messages now apply automatically.** If you don't want this,
  don't subscribe to OTA — but the library subscribes for you on connect.
  Future v0.2 will gate this behind a config flag.

- **Diagnostics now gate OTA.** If you've registered `SCADABLE_TEST(...)`
  handlers and any of them returns `TEST_FAIL`, the next `ota/available`
  manifest is rejected. This is the "permission-gated deploy" half of the
  fleet-ops contract — but it can surprise customers who registered tests
  without expecting them to gate. To opt out for now, don't register any
  failing tests; v0.2 will add a `gate_ota: false` flag per test.

### What did NOT change

- Public function signatures in `include/scadable.h`.
- Error code values (`SCADABLE_OK = 0`, `SCADABLE_ERR_NOT_INITIALIZED = -1`,
  …).
- Event payload struct shapes.
- The `scadable_generated.h` placeholder format.
- Topic schema (already matched gateway-esp).

### Rollback

If something breaks, pin back to v0.0.1:

```yaml
dependencies:
  scadable/libscadable: "0.0.1"
```

…and open an issue at https://github.com/scadable/libscadable/issues with
the monitor output. v0.0.x doesn't actually do anything, so it's a safe place
to fall back to while we fix forward.
