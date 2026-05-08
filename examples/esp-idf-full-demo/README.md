# esp-idf-full-demo

Reference firmware exercising every public API in libscadable. Use this when
integrating libscadable into your own ESP-IDF project.

For the minimum 4-line "hello world", see [../esp-idf-hello-world/](../esp-idf-hello-world/).

## What it does

- Brings up wifi (creds in NVS namespace `wifi`)
- Calls `scadable_init()` with explicit config (keepalive, log batch interval)
- Registers an event callback (connected, disconnected, OTA available, env changed)
- Registers two diagnostic tests (wifi link, free heap) that gate OTA
- Connects to broker via mTLS (cert from NVS namespace `scadable_certs`)
- Publishes a discrete `ping` payload + two typed metrics every 5 s
- Logs a heartbeat every 30 s
- Every 5 min, announces a 30 s scheduled-offline window (Verdant-pattern demo)
- OTA + env_vars updates handled automatically in the background

## Build

```bash
idf.py set-target esp32-s3
idf.py build flash monitor
```

## Provisioning

This example expects the device to already have:

1. WiFi credentials in NVS namespace `wifi` (your provisioning flow puts them there).
2. Device certificate + private key in NVS namespace `scadable_certs` under
   keys `device_cert` and `device_key` (the SCADABLE web-flasher writes these
   at flash time).

If either is missing the binary boots but `scadable_init()` returns
`SCADABLE_ERR_NO_CERT` and you'll see that error in the monitor output.
