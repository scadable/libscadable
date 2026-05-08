# Host-side smoke test for libscadable.
# SCADABLE 2026 · Apache-2.0
#
# Compiles every src/*.c file with stub ESP-IDF symbols (no esp-mqtt, no
# nvs_flash) and runs a tiny smoke suite that exercises the public API.
# The point isn't to test ESP-only code paths — it's to catch:
#   - Compile / link breakage in the !ESP_PLATFORM branches.
#   - Public API contract drift (return codes, NULL handling, idempotency).
#   - JSON envelope shape regressions.
#
# Run:
#   cmake -S tests/host -B build/host && cmake --build build/host
#   ./build/host/test_libscadable

cmake_minimum_required(VERSION 3.16)
project(test_libscadable C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-function)

set(LIBSCADABLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_executable(test_libscadable
    test_libscadable.c
    "${LIBSCADABLE_ROOT}/src/lifecycle.c"
    "${LIBSCADABLE_ROOT}/src/publish.c"
    "${LIBSCADABLE_ROOT}/src/telemetry.c"
    "${LIBSCADABLE_ROOT}/src/logging.c"
    "${LIBSCADABLE_ROOT}/src/env.c"
    "${LIBSCADABLE_ROOT}/src/diagnostics.c"
    "${LIBSCADABLE_ROOT}/src/ota.c"
)

target_include_directories(test_libscadable PRIVATE
    "${LIBSCADABLE_ROOT}/include"
    "${LIBSCADABLE_ROOT}/src"
)

# SCADABLE_NO_GENERATED — skip the codegen header. We provide weak
# defaults inline in lifecycle.c.
target_compile_definitions(test_libscadable PRIVATE SCADABLE_NO_GENERATED)

# Disable ESP_PLATFORM — every src/*.c has #ifdef ESP_PLATFORM guards
# around the parts that need esp-idf. The else branches keep the host
# build linkable.

enable_testing()
add_test(NAME smoke COMMAND test_libscadable)
