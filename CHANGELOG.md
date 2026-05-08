# Changelog

All notable changes to libscadable will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial public API surface in `include/scadable.h`
- ESP32 platform shim stubs in `src/platform/esp32/`
- ESP-IDF Component Registry manifest (`idf_component.yml`)
- `examples/esp-idf-hello-world/` reference project
- CI workflow for build verification

### Notes
- v0.1 is ESP32-first. Linux (Rust crate + `.so` library) ships in v0.2.
- ABI stability: not yet guaranteed. v1.0 will mark the public API as stable.
