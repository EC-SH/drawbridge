# Changelog

## Unreleased (fix/sip-engine-defects) - 2026-06-11

### Fixed
- **SIP Engine Defects**: Resolved 27 concurrency, locking, memory, and code quality issues identified in the SIP Engine defects sweep (commits `30691f9` and `21175ce`).
  - **Memory Safety**: Implement carry buffer for odd-length PCM16 reads to eliminate full-scale white noise on handset (Issue #2).
  - **Thread-Safety**: Guard `_rxTaskHandle` and `_activeParticipantId` under `_mutex` in `ThreeCxAnchorClient.cpp` (Issue #22).
  - **Concurrency**: Lock `_postClient` lifecycle operations with `_postMutex` in `ThreeCxAnchorClient.cpp` (Issue #23).
  - **Deadlock Mitigation**: Use `try_lock` in `vTaskDelete` cleanup loop in `ThreeCxAnchorClient.cpp` to prevent deadlocks (Issue #26).
  - **RTP & Media**: Replace open-coded mu-law encode loop with a symmetric `RtpSender::ulawEncodeBuffer` helper in `MediaBridge.cpp` (Issue #27).
  - **Deadlock Prevention**: Refactor `dropCall` thread spawning outside lock in `LoopbackAnchorClient.cpp` to prevent Registrar deadlock (Issue #28).
  - **Resource Management**: Guard ensureToken's media-streams-active check under `_mutex` (Issue #13).
  - **WS Reconnection**: Fix WS Auth header refresh to use updated tokens instead of re-sending dead boot token (Issue #14).
  - **JSON Parsing**: Extract `readJsonStringField` helper to deduplicate JSON parsing in `ThreeCxAnchorClient.cpp` (Issue #16).

## v1.2.0 - 2026-06-04

Production-hardening + feature release. Verified end-to-end on a JC3248W535
display board (ESP-IDF v6.0.1.5): builds, flashes, boots, joins Wi-Fi as a client,
serves the dashboard, and handles SIP registration/calls at single-digit-ms
latency.

### Added
- **Admin authentication**: PIN + server-side session layer (salted, iterated
  SHA-256; HttpOnly cookie; brute-force lockout). Dashboard Security panel
  (set-PIN / login / logout) gating every state-changing control. `THREAT_MODEL.md`.
- **OTA firmware updates**: dual-OTA partition table, streaming `/api/ota/upload`
  (bypasses the 16 KB body cap), anti-rollback confirmation on healthy boot, and
  a dashboard Firmware-Update panel. `OTA.md`.
- **Configurable SIP memory pools** (`PoolConfig.hpp`) with 3 documented hardware
  tiers. `SCALING.md`.
- **PBX features**: Call Detail Records (`GET /api/cdr`) and per-extension
  Do-Not-Disturb (`POST /api/dnd`, 480-on-INVITE).
- **Operator docs**: Setup, Hardware-Selection, Phone-Compatibility, Troubleshooting;
  plus server-side RTP design (`RTP.md`) and a technical feature roadmap.
- **SIP load/stress suite** (`tests/load/`) + on-device findings; CI now runs the
  unit suite, a partition-table guard, and a tag-triggered release workflow.

### Fixed
- **Display STA-mode watchdog**: coalesce/rate-limit the on-screen log so a log
  flood can't pin Core 1 on full-screen repaints (task-WDT eliminated).
- **Dashboard unreachable in STATION mode**: the HTTP accept loop's `std::thread`
  had a 3 KB pthread stack that `sendApiStatus` overflowed on-device; raised to
  8 KB and bound the listener to `INADDR_ANY` (robust across AP/STA + DHCP changes).

## Unreleased (fix/esp-idf-ci-failures) - 2026-06-03

### Fixed
- Isolate `esp_wifi` requirement in `main/CMakeLists.txt` to only `wifi` and `display` transport components, ensuring the `eth` transport compiles without wifi dependencies.
- Add `cppcheck` suppressions in `.github/workflows/ci.yml` for third-party QR code files (`qrcode.c` and `qrcode.h`).
- Support compiling under ESP-IDF v6.0+ locally by adding version-conditional CMake target dependencies for the split GPIO and SPI drivers, and adding conditional registry dependency for W5500 Ethernet driver (`espressif/w5500`).
- Resolve application binary partition overflow on local debug configuration by changing the default partition table setting to `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` (1.5MB).


## Unreleased (fix/cppcheck-warnings) - 2026-06-03

### Fixed
- Resolve cppcheck static analysis warnings and style issues:
  - Initialize `bytesReceived` in `src/Helpers/UdpServer.cpp` to avoid potential use of an uninitialized variable.
  - Add `main/host_compat.h` to provide guarded host-side `MACSTR` / `MAC2STR` macros so host builds and static analysis can process Wi‑Fi event logging without defining ESP-IDF headers.
  - Replace C-style cast with `reinterpret_cast` for `sockaddr` in `main/wifi/DnsServer.cpp` to satisfy cppcheck style checks.

Commit: https://github.com/EC-SH/drawbridge/commit/825354bf88afd1fcd965c7ab743999ce10b9e919

### Notes
- `host_compat.h` only defines macros when they are not already defined, so ESP-IDF builds remain unaffected.
- This change targets CI cleanliness and static analysis; runtime behavior on ESP32 devices is unchanged.
