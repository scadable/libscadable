# libscadable

The native library that customers link into their firmware to ship to the SCADABLE fleet operations platform. Drop it into your firmware, call a few functions, get managed MQTT + log streaming + telemetry + remote diagnostics + signed OTA updates for free.

**Status:** v0.1.0 — ESP32 first. Linux, STM32, nRF, RP2040 follow as customers ask. See [CHANGELOG.md](CHANGELOG.md) and [MIGRATION.md](MIGRATION.md) for upgrade notes.

**License:** Apache 2.0.

## Hello world (4 lines)

```c
#include "scadable.h"

void app_main(void) {
    wifi_up();                        // your WiFi setup
    scadable_init(NULL);              // reads cert + namespace from NVS (set at flash time)
    scadable_connect();               // non-blocking; auto-reconnects
    scadable_publish(SCADABLE_CH_HELLO, "world", 5, 1);
}
```

That's it. Cert was provisioned at flash time via the SCADABLE web-flasher. The library handles MQTT, mTLS, reconnection, and OTA in the background.

## Install

### ESP-IDF (component)

Add to your project's `idf_component.yml`:

```yaml
dependencies:
  scadable/libscadable: "^0.1.0"
```

Then in your component's `CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    REQUIRES libscadable
)
```

### Cargo (Rust on ESP32 / Linux — coming soon)

```toml
[dependencies]
libscadable = "0.1"
```

### Arduino + PlatformIO

`platformio.ini`:

```ini
[env:esp32-s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
lib_deps = scadable/libscadable@^0.1.0
```

## Public API

The full public surface is ~16 functions + 4 macros. See `include/scadable.h` for the canonical reference, or the per-function pages at https://docs.scadable.com/library/esp/api/.

| Group | Functions |
|---|---|
| Lifecycle | `scadable_init`, `scadable_connect`, `scadable_disconnect`, `scadable_is_connected`, `scadable_state`, `scadable_on_event` |
| Pre-sleep | `scadable_flush`, `scadable_announce_offline` |
| Publish | `scadable_publish` |
| Telemetry | `scadable_metric_set_u32`, `scadable_metric_set_f64` |
| Logging | `SCADABLE_LOG_DEBUG/INFO/WARN/ERROR` (macros) |
| Diagnostics | `SCADABLE_TEST(name, ctx)` (macro) + auto-registered via codegen |
| Env vars + secrets | `scadable_env_get`, `scadable_env_get_or`, `scadable_env_get_int/double/bool`, `scadable_secret_get`, `scadable_secret_get_or`, `scadable_on_env_change` |

## Templates

Don't start from scratch — fork a template:

- [scadable/esp-idf-starter](https://github.com/scadable/esp-idf-starter) — ESP-IDF (C)
- [scadable/arduino-platformio-starter](https://github.com/scadable/arduino-platformio-starter) — Arduino + PlatformIO (C++)
- More coming (Cargo + esp-rs, Linux daemon templates).

## Docs

Full reference at https://docs.scadable.com/library — Getting Started, API Reference, Recipes, Migration guides from competing tools (Particle, Memfault, AWS IoT Embedded-C, plain ESP-IDF MQTT).

## Examples

- [`examples/esp-idf-hello-world/`](examples/esp-idf-hello-world/) — minimum 4-line firmware (publish "world" once and idle).
- [`examples/esp-idf-full-demo/`](examples/esp-idf-full-demo/) — exercises every public API (lifecycle, publish, telemetry, log, env, secrets, diagnostics, OTA gate, scheduled-offline announce, event callbacks). Read this when integrating libscadable into your own project.

## Building from source

```bash
git clone https://github.com/scadable/libscadable
cd libscadable/examples/esp-idf-hello-world
idf.py set-target esp32-s3
idf.py build
```

Run the host-side smoke suite (no chip required, ~1 s):

```bash
cmake -S tests/host -B build/host && cmake --build build/host
./build/host/test_libscadable
```

## Contributing

This is the customer-facing surface — every change is a public-API change. PRs welcome but expect review for ABI stability + naming consistency.

Issues: https://github.com/scadable/libscadable/issues
