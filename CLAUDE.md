# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

pocket-dial is a self-contained SIP PBX that runs on a single ESP32-S3 (no router/SIP trunk needed) **and** as a desktop/server binary. One C++17 SIP engine (`src/`) is compiled three ways: ESP-IDF firmware, Arduino sketches, and a host executable. RTP media flows **peer-to-peer** between phones; the device only brokers signaling.

## Build & test

The engine is platform-abstracted with preprocessor guards. The same `src/` builds under all of:

### Host / desktop (this is what CI gates on, and the fastest dev loop)
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/SipServer --ip 127.0.0.1 --port 5060 --web 8080   # main.cpp entry point
```
When `IDF_PATH` is **unset**, the root `CMakeLists.txt` builds the host binary + GoogleTest suite. When `IDF_PATH` is **set**, the same file delegates to ESP-IDF instead — so unset the IDF env for host work.

### Unit tests (GoogleTest, pulled via FetchContent)
```bash
ctest --test-dir build/tests --output-on-failure          # all tests
./build/tests/sip_parser_tests --gtest_filter='*Pbx*'     # a single test/group
```
Tests live in `tests/*.cpp` (SIP parser, PBX star-codes, RTP/SDP). They link a subset of `src/SIP/` compiled as host stubs (`RtpSender.cpp` has no-socket fallbacks under `!ESP_PLATFORM`).

### ESP-IDF firmware (ESP-IDF v5.1+ through v6.0.1)
Transport is selected by the `SIP_TRANSPORT` CMake var, which picks the `main/esp_main_*.cpp` entry point and its driver/component set:

| `SIP_TRANSPORT` | Entry point | Board / target |
|---|---|---|
| `eth` (default) | `esp_main_eth.cpp` | W5500 wired/PoE (esp32s3) |
| `wifi` | `esp_main.cpp` | generic ESP32-S3 SoftAP |
| `display` | `esp_main_display.cpp` | Guition JC3248W535 AXS15231B touch display (LVGL) |
| `lan8720` | `esp_main_eth_lan8720.cpp` | classic ESP32 internal EMAC — needs `set-target esp32` |

```bash
idf.py set-target esp32s3
idf.py -D SIP_TRANSPORT=display build
idf.py -p COM4 -D SIP_TRANSPORT=display flash monitor
```
- This machine's local build/flash workflow (IDF env sourcing, the COM4 display board, NVS-injection provisioning) is documented in auto-memory `build-flash-test-workflow.md` — read it before flashing.
- `SIP_CONSTRAINED=1` shrinks the object pools for low-RAM classic ESP32 (pair with `sdkconfig.defaults.esp32_constrained` + `partitions_4mb.csv`).

### Cross-compile (Orbic ARMv7 musl static, issue #82)
```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/armv7-linux-musleabihf.cmake -DBUILD_TESTING=OFF
```

### CI gates (`.github/workflows/ci.yml`)
Host job runs **cppcheck** (`--error-exitcode=1`, blocking), clang-tidy (non-blocking baseline ~59), the GoogleTest suite, a `partitions.csv` dual-OTA layout guard, and `tests/http/test_api.sh` against the running host binary. A separate matrix job compiles the firmware across IDF v5.1.2/v5.2.1 × esp32/esp32s3 × wifi/eth (+ one display leg). PRs that touch `src/` or `main/` must pass cppcheck and the unit suite.

## Architecture

### Engine core (`src/SIP/`)
- `SipServer` owns the `UdpServer` socket + `RequestsHandler` + `SipMessageFactory`; on host builds it also runs a `_tickThread`. On ESP, the platform entry point drives `tick()`.
- `UdpServer` (`src/Helpers/`) is the threading/socket abstraction — one receive thread, platform-pinned to a core.
- `SipMessageFactory` parses raw UDP into `SipMessage`/`SipSdpMessage` (O(n) non-mutating index-walking parser, no RTTI), then `RequestsHandler` dispatches by method through a handler table.
- `RequestsHandler` is the heart: the registrar (`_clients`), call state machines (`_sessions`), star-codes/PBX features, the `777` echo and `999` broadcast virtual extensions, and the thread-safe **dashboard query API** (`getActiveClients`, `getActiveSessions`, CDR, DND, call-forwarding).

### The two invariants that shape all engine code
1. **Zero heap allocation in the packet hot path.** Every `SipClient`, `Session`, and `SipMessage` is pre-allocated in fixed pools at boot (`src/SIP/PoolConfig.hpp`). These pool sizes ARE the device's hard concurrency limits; exhaustion degrades gracefully to `503 Service Unavailable` (or a one-off heap fallback for the message pool) — it never crashes. Don't `new`/`make_shared` in `RequestsHandler` or any UDP loop; use the pool allocators.
2. **No blocking I/O under the registrar lock.** Generate responses into a local outbox vector inside the `_mutex` scope, then fire `sendto`/socket calls *after* releasing it. See the worked GOOD/BAD examples in `CONTRIBUTING_FIRMWARE.md` — that file is the enforced coding standard (bounds-checked strings via `strlcpy`/`snprintf`, all NVS/driver/syscall returns checked, no unchecked pointer derefs in onboarding paths).

### Dual-core topology (ESP only)
Core affinity differs by transport and is wired through compile defs in `main/CMakeLists.txt`:
- Non-pure-Ethernet transports define `POCKETDIAL_HAS_WIFI`.
- The **display** build reserves Core 1 for the LVGL graphics task and runs the SIP engine + UDP receiver on Core 0 via `POCKETDIAL_UDP_RX_CORE=0`. Other transports leave SIP on Core 1 (the `UdpServer` default).
- Never dispatch networking/file I/O onto the LVGL core. Cross-core state must go through the snapshotted dashboard getters, not raw shared mutexes.

### Platform abstraction pattern
Code branches on `defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)` vs `__linux__` vs `_WIN32`. When editing `src/`, preserve all three arms. Headers like `host_compat.h` (in `main/`) and the `!ESP_PLATFORM` stubs in `RtpSender.cpp` exist so the engine stays host-compilable and unit-testable.

### SIP interoperability quirks (don't "fix" these — they're deliberate)
- `clearBody()` strips caller SDP from forwarded `180 Ringing` (prevents early-media loops on Yealink-class phones) and rewrites `Content-Length`.
- `enforceG711()` forces the codec list to `0 8 101` on forked INVITE / 200 OK SDP. Any SDP mutation MUST resync `Content-Length` afterward (this was the historical `777`/`999` bug class).

## Layout notes
- `src/` — cross-platform engine (Helpers + SIP). The single source of truth; sketches and firmware all compile it.
- `main/` — ESP-IDF component: per-transport entry points, `drivers/` (AXS15231B QSPI panel), `ui/` (LVGL CGA dashboard + QR), `wifi/` (captive-portal DNS).
- `sketches/` — standalone Arduino IDE ports (the `SipServer_JC3248W535` sketch is **deprecated** in favor of the ESP-IDF display build).
- `.smoke/` — hardware smoke scripts (serial capture, SIP probe, NVS provisioning generator).
- `tests/http/test_api.sh` is the single HTTP smoke suite used by both CI and on-hardware runs.
- `docs/` — deep-dives (ARCHITECTURE, SCALING, THREAT_MODEL, OTA, PROVISIONING, RTP). `ISSUES.md` is the live architectural roadmap/issue tracker.
- Build output dirs (`build*/`) and `*.log` files in the repo root are local scratch — don't commit them.

## SSH note
`src/Helpers/SshServer.cpp` is implemented behind `POCKETDIAL_HAS_WOLFSSH` but the wolfSSL/wolfSSH managed components are **disabled** in `main/idf_component.yml` — they don't compile against ESP-IDF v6.0.1 (use v5 APIs removed in v6). Re-enable only when wolfSSL ships v6 support or when pinning IDF v5.x.
