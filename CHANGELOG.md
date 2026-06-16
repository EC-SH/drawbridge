# Changelog

## Unreleased - 2026-06-15

WAN-anchor inbound PSTN, teardown reliability, performance hardening, and soak observability.
**Inbound PSTN→extension ring-all is now hardware-verified pre-alpha** (the project's last
capability gate); **outbound teardown is hardware-confirmed**. Connect/teardown latency dropped
from ~1 s toward ~100 ms two-way audio.

### Added
- **Inbound PSTN ring-all (Mode 1)** (#102 / #80): an inbound call to the DID forks a delayed-offer
  INVITE to **every registered extension**; the first 200 OK wins (losing forks CANCELed), the anchor
  leg is answered on the handset's 200, and the media bridge starts against the answering handset.
  Hardware-verified pre-alpha — ring-all, two-way audio, and DTMF confirmed; soak hardening ongoing.
- **Soak telemetry in `/api/status`** (#81-84 / #103): a `telemetry` object exposing anchor
  connection state + estimated TLS socket count, TLS full-vs-resumed handshake counts, RTP playout
  underrun/overrun counters, client/session pool occupancy, heap/PSRAM headroom, and last-boot
  reset reason. Host build emits `0`/`"host"` so the schema is stable. See [docs/API.md](docs/API.md).

### Changed / Performance
- **TLS session resumption + warm cached control session + keepalive** (#93 / #99): the S3 has no
  ECC/ECDSA hardware, so the ECDHE handshake is software (~1 s); resumption is the workaround. A
  resumed connect drops from ~682 ms to ~116 ms.
- **Media cut-through** (#105): both-ways pre-warm + outbound TLS resumption to kill answer-time dead
  air (the upstream won't route a pre-Connect POST, so the outbound stream is primed one-shot at
  answer), plus handshake telemetry.
- **Playout-buffer latency fix** (#104): cap + drain the `PlayoutBuffer` (≤200 ms ceiling, oldest-
  sample drop on overrun) so a bursty TCP far-end stream can't accumulate hundreds of ms of one-way
  latency. Adds `TCP_NODELAY`.
- **WS-event worker pool** (#101): move blocking WebSocket-event work off the event thread to a
  worker pool (pool-ready for multi-call), shrinking the event task stack.
- **LWIP IRAM optimization** (#98): place hot LWIP paths in IRAM for lower RTP/SIP jitter.

### Fixed
- **Reliable outbound teardown** (#39 / #40): a handset hangup now reliably drops the upstream PSTN
  leg, via own-leg correlation (control our own leg, not the WS-surfaced far leg) plus a reconcile
  watchdog and the associated race fixes. Hardware-confirmed.
- **PSTN leg freed on hangup** (#95): drop the upstream leg by freeing audio sockets first
  (the drop worker previously couldn't spawn under internal-heap exhaustion; mbedTLS now uses
  external/PSRAM allocation). Hardware-confirmed ~1 s teardown.
- **Register-beep INVITE transaction** (#91): complete the register-beep INVITE transaction on
  teardown (the beep Call-ID lookup never matched because the header getter returned the whole line).
- **Audit cluster** (#41-#74, landed as #77-#89): concurrency & memory-safety, security hardening,
  lock-discipline/perf, BLF-stays-lit-on-hold, a vendor-neutrality scrub of the public docs, and
  expanded test coverage.

### Recovered features (#37)
- Call Hold/resume, Call Park, Page/Zone paging, BLF/Presence, and the telephony-provider credential
  layer (also itemized in the recovered/all-features entry below).

### Direction (in progress — not shipped)
- **ESP32-only pivot** (#96 / #97): deprecating the desktop/server product; #97 removed install/
  quickstart deadweight and the desktop "run on a computer/Pi" framing. The host build and its CI
  test gate are retained until an on-target/QEMU harness replaces them (open question in #96).
- **Configurable TLS re-warm heartbeat** (#107): periodic idle re-warm of the POST audio TLS session
  so even the first call after a long idle gap resumes instead of cold-handshaking; interval to be
  user-configurable. Being implemented now.

## Unreleased (fix/anchor-trunk-teardown) - 2026-06-13

Pre-existing commercial-softswitch outbound-trunk (WAN-anchor) teardown bugs — surfaced when the
PSTN/cell leg lingered after the handset hung up. Diagnosed on hardware with a
purpose-built CLI SIP UAC (`sip_probe.py`) registered to the device + serial
capture (a switched LAN hides the phone↔device unicast from a normal pcap).

### Fixed
- **`onAck` answered the handset ACK with `404`**: a WAN-anchor 200 OK's ACK fell
  through to `endHandle(getToNumber())`, routing the ACK at the dialed PSTN number
  (not a registered extension). You never respond to an ACK; tag-strict phones
  (Yealink) then retransmit → dialog storm → the softswitch reaps the call (~15s "self-drop").
  Absorb the ACK when `isAnchor()`.
- **Malformed teardown BYE**: `buildServerBye` re-prepended header names onto values
  that already carried them (`To: From:` / `Call-ID: Call-ID:`); Yealinks reject it
  and stay off-hook. `buildServerBye` now strips a leading header name from its
  From/To/Call-ID args (generalized `parkCallIdValue` → `stripHeaderName`).
- **Outbound drop never identified the participant**: only the inbound path stored
  the softswitch participant id, so `asyncDropCall("")` fell back to an empty
  `_activeParticipantId`. Store `ev.participantId` on the answered session; pass
  `getAnchorParticipantId()` from `onBye`/`onCancel`.
- **`dropCall()`/`answerCall()` POSTed an empty body with no `Content-Type`**: the
  softswitch participant-action endpoint requires an `application/json` body; the reference
  posts `"{}"` (and `makeCall` already did). The softswitch silently rejected the empty drop —
  the real reason teardown never fired even after the id/socket fixes. Now sends
  `Content-Type: application/json` + `"{}"`.

### Changed
- **`CONFIG_LWIP_MAX_SOCKETS` 10 → 16.** The W5500 runs in MACRAW mode, so the
  ESP32-S3's LWIP stack owns every socket (not the chip's 8-socket HW TCP engine).
  10 was too tight for the 3 persistent softswitch-anchor TLS sockets (control WS + GET + POST
  audio streams) on top of SIP/dashboard/SSH — exhaustion showed as `sock < 0` /
  mbedTLS `create_ssl_handle failed`, blocking the drop POST.
- Re-enabled the **ESP32-S3R8 8 MB octal PSRAM** for the headless eth build. The
  generated `sdkconfig` had drifted to PSRAM-off (a stale artifact from display-build
  tuning); `sdkconfig.defaults` already requests `CONFIG_SPIRAM=y`, so regenerating
  (`rm sdkconfig && idf.py build`) restores the 8 MB heap pool. (Heap was never the
  trunk constraint — sockets were — but it removes all resource pressure.)

## Unreleased (recovered/all-features) - 2026-06-13

### Added
- **Call Hold / resume**: a mid-dialog re-INVITE (RFC 3261 §12.2) is relayed
  untouched between the two legs (no `clearBody()`/`enforceG711()`, so the hold
  SDP + Content-Length stay intact); the session tracks `Held`/`Connected` from
  the SDP direction (`a=sendonly`/`recvonly`/`inactive` vs `sendrecv`), and CDR
  talk-time spans the hold. `SipMessage::getSdpDirection()`. `tests/Hold_test.cpp`.
- **Call Park** (park-orbit, virtual extensions `700`+): INVITE to a FREE orbit
  parks the caller (silent `a=inactive` answer); INVITE to an OCCUPIED orbit
  retrieves it — the retriever is answered with the parked party's SDP and the
  parked party gets an in-dialog re-INVITE with the retriever's SDP, so media
  renegotiates peer-to-peer; ring-back to the parker on timeout, else BYE; BYE
  bridging on either leg. `getParkedCalls()` dashboard view. `tests/Park_test.cpp`.
- **Page / Zone paging** (`98x` virtual extensions): `PbxConfig::PageZone`
  one-INVITE-per-member fork with bounded zone membership. `tests/PageZone_test.cpp`.
- **BLF / Presence**: RFC 6665 `SUBSCRIBE`/`NOTIFY` with the RFC 4235 "dialog"
  event package; busy-lamp-field change detection after each handled packet plus
  a 1 Hz sweep; bounded subscription pool (`POCKETDIAL_MAX_SUBSCRIPTIONS`).
- **Telephony provider API**: a bounded, write-only-secret credential table
  (`TelephonyApiConfig` / `TelephonyProvider`) that selects the boot-time WAN
  anchor provider; thread-safe RequestsHandler getters/setters for the dashboard.

### Changed
- `SipMessage` gains `getBody()` / `setBody()` (body accessor + Content-Length
  resync), backing the call-park retrieve SDP swap.
- New `PoolConfig.hpp` knobs: `POCKETDIAL_PARK_SLOTS`, `POCKETDIAL_PARK_TIMEOUT_SEC`,
  `POCKETDIAL_MAX_PAGE_ZONES`, `POCKETDIAL_ZONE_MEMBER_CAP`, `POCKETDIAL_MAX_SUBSCRIPTIONS`.

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
