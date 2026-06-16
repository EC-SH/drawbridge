# Issue Tracking & Architectural Roadmap

This document serves as the active issue tracker and architectural roadmap for **pocket-dial**. It tracks high-impact concurrency, performance, and hardware-specific issues identified during production deployments, along with their resolution status.

---

## Active Issues & Backlog Roadmap

This backlog is prioritized by architectural dependency and deployment urgency.

### 🔴 Critical Priority: Commercial-Softswitch (3CX) Call Control & Media Loopback (WAN Bridge)

> **Status (2026-06-15):** The core WAN-anchor capability — RTP receive/decode (#61), the
> call-control client (#63), and bridge orchestration (#64) — **shipped via PR #39** and is
> hardware-confirmed. Since then, on `main` (newest first): **inbound PSTN ring-all (#102/#80)**
> forks an inbound DID call to every registered extension with two-way audio (**hardware-verified
> pre-alpha** — this cleared the project's inbound-PSTN capability gate); **outbound teardown
> (#39/#40, #95)** is hardware-confirmed; and a **performance-hardening wave** landed — TLS session
> resumption + warm control session (#93/#99), media cut-through both-ways pre-warm + outbound TLS
> resumption (#105), playout-buffer cap+drain (#104), LWIP IRAM tuning (#98), and the WS-event
> worker pool (#101). Soak telemetry now ships in `/api/status` (#81-84/#103). Connect/teardown
> dropped from ~1 s toward ~100 ms. The items below remain genuinely open follow-ups.
>
> **In flight (not shipped):** a **configurable TLS re-warm heartbeat (#107)** — periodic idle
> re-warm of the POST audio TLS session so even the first call after a long idle gap resumes instead
> of cold-handshaking; the interval will be user-configurable. Being implemented now.
>
> **Direction (in progress, #96/#97):** the project is pivoting to **ESP32-only** and deprecating
> the desktop/server product. PR #97 removed the install/quickstart deadweight and the desktop
> framing; the host build + its CI test gate are retained until an on-target/QEMU harness replaces
> them (open in #96). This affects the host-development items below.

#### 🟢 Issue #86: WAN Anchor: Optimize WS Event Task Stack Size
* **Status**: ✅ Resolved (PR #101)
* **Labels**: `performance`, `anchor`, `memory-safety`
* **Severity**: Critical
* **Description**: The WebSocket task stack was sized at 16,384 bytes to accommodate blocking HTTP GET calls (like `getParticipantStatus`) executing directly inside the WebSocket callback thread. PR #101 moved the blocking WS-event work off the event thread to a **worker pool** (also making the anchor pool-ready for multi-call), allowing the event task stack to shrink.

#### 🔴 Issue #29: Anchor/Loopback: Call Setup and Teardown Latency Optimizations
* **Status**: ✅ Resolved (Implemented on branch `optimize-call-performance`)
* **Labels**: `performance`, `anchor`
* **Severity**: Critical
* **Description**: Optimize call setup and teardown latency for the commercial-softswitch Media Anchor client and Loopback mock client. Parse status directly from WebSocket Upset attached_data, implement exponential back-off on GET stream retries, configure 2-second HTTP client timeouts, and reduce mock simulation delays.

---

### 🟡 High Priority: Platform Compatibility & Host Development

> **Note (#96/#97):** the ESP32-only pivot deprecates the desktop/server product, which
> de-prioritizes the two host-media items below — they are likely **WON'T-DO** unless the host
> build is kept as a full runtime target rather than just a dev/test harness. Pending the #96
> test-harness decision.

#### 🟡 Issue #62: Real Desktop (host) Media Transport
* **Status**: ⏳ Open / Planned (likely de-scoped by the ESP32-only pivot, #96/#97)
* **Labels**: `api-integration`, `media`, `desktop`
* **Severity**: High
* **Description**: Currently, the `RtpSender` socket and pacing are gated behind `#if ESP_PLATFORM`, leaving desktop builds with no-op stubs. Implement standard POSIX UDP socket writes and a platform-independent 20ms pacing loop to support audio bridging on desktop (Linux/Windows) gateway installations.

#### 🟡 Issue #87: WAN Anchor: Implement Desktop/Host Media Transport Support
* **Status**: ⏳ Open / Planned (likely de-scoped by the ESP32-only pivot, #96/#97)
* **Labels**: `api-integration`, `media`, `desktop`, `anchor`
* **Severity**: High
* **Description**: `ThreeCxAnchorClient` is currently completely stubbed out on host/desktop builds. Refactor the network socket, task, and HTTP/WebSocket client interfaces to use POSIX/Windows compatible headers (instead of `esp_websocket_client` / `esp_http_client`), allowing the WAN-anchor gateway integration to be testable and runnable on local PCs.

---

### 🟢 Medium Priority: Hardware Validation & Deployment Features

#### 🟢 Issue #44: End-to-end SIP call test needed on JC3248W535EN hardware
* **Status**: ⏳ Open / Planned
* **Labels**: `hardware-testing`, `verification`
* **Severity**: Medium
* **Description**: Once physical smart display units (Guition JC3248W535EN) are re-connected, perform a verification suite to confirm that high-frequency screen redraws and touch events do not starve or block the real-time SIP signaling loop on Core 1.

#### 🟢 Issue #35: [Feature Request] Zero-Touch Phone Auto-Provisioning (HTTP)
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature-request`, `provisioning`
* **Severity**: Medium
* **Description**: Implement a local HTTP directory service to serve auto-generated `.cfg` and `.xml` configuration files to standard IP phones (Yealink, Grandstream, Cisco) upon boot, automatically mapping MAC addresses to local extensions from NVS storage.

---

### 🔵 Low Priority: Diagnostics & Hobbyist Compatibility

#### 🔵 Issue #32: [Feature Request] Live SIP Tracer in the Web Terminal
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature-request`, `diagnostics`
* **Severity**: Low
* **Description**: Stream live SIP signaling packets (incoming/outgoing UDP payloads) directly to the web dashboard landing page's CRT terminal using WebSockets for real-time diagnostics.

#### 🔵 Issue #33: [Feature Request] PCAP Dump Endpoint for Wireshark analysis
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature-request`, `diagnostics`
* **Severity**: Low
* **Description**: Expose an HTTP endpoint `/api/diagnostics/pcap` to export a rolling ring-buffer of captured network packets in raw PCAP format for native analysis in Wireshark.

#### 🔵 ~~Issue: SIP core: Arduino IDE platform detection guards need verification (ESP32/ARDUINO defines)~~ — won't-do (Arduino support removed, ESP32-only #96)
* **Status**: ⏳ Open / Planned
* **Labels**: `build-system`, `compatibility`
* **Severity**: Low
* **Description**: Audit preprocessor directives (`ESP32`, `ARDUINO`, `ESP_PLATFORM`) to guarantee compiling out the box for hobbyists utilizing the Arduino IDE workspace instead of standard ESP-IDF. (Tracked on GitHub under the Arduino-guard verification item; not to be confused with the audit's GitHub issue #41 [C-1], which is the now-resolved SIP message-pool race below.)

#### 🔵 Issue #88: WAN Anchor: Compile-Gate Peak Amplitude Diagnostics on Hot Path
* **Status**: ⏳ Open / Planned
* **Labels**: `performance`, `anchor`
* **Severity**: Low
* **Description**: `writeAudio()` computes the peak amplitude of each PCM sample block. This runs on Core 1's real-time loop every 20ms. Compile-gate this diagnostic loop out in release builds (`#ifndef NDEBUG` or similar) to save CPU cycles on the hot path.

---

## API Integration: WAN-Anchor Call-Control Connector (Epic Reference)

> **Goal**: let a pocket-dial / tincan handset place calls to **upstream extensions** over WAN by bridging pocket-dial's SIP/RTP world to a commercial softswitch's HTTP-based **call-control API**.
>
> **Why no SBC / NAT / STUN / TURN / ICE is needed**: the connector *terminates the handset's RTP locally* (one SIP hop, on-box) and re-originates the call into the softswitch over **HTTPS, not SIP/RTP** — so there is no second SIP/RTP peer for ICE to traverse. Both legs are 8 kHz, so media is a pure companding swap (G.711 ⇄ PCM16) with **no resampling and no media server**. Over WAN this only requires a flat L3 (WireGuard/overlay or public IP) so the connector's advertised SDP address is directly reachable.

### Architecture Decision
The connector is a **media-terminating SIP endpoint** that `REGISTER`s to pocketdial-desktop as an ordinary extension (e.g. `3000`, or a dialing prefix). pocket-dial's **existing** `onInvite` forward path (`RequestsHandler.cpp`) delivers the INVITE to it with **zero changes to the signaling-only server** — the registrar keeps sourcing no media. The connector mirrors the **`440` `RtpSender` beachhead**, except it:
* answers `200 OK` with its **own** SDP carrying a real media address and `a=sendrecv` (the `440` path uses the server media port but is send-only/tone),
* bridges audio to the softswitch instead of synthesizing a tone,
* maps SIP `BYE`/`CANCEL` ↔ upstream participant `drop`.

### Reference: commercial-softswitch call-control API (verified)
* **License**: a per-vendor licence tier that exposes call-control access; an API client scoped to **call-control access**; a **route-point DN** for origination. (Vendor-specific SKU/entitlement names live in the gitignored `reference/`, not in this public doc.)
* **Auth**: OAuth2 `client_credentials` → `POST https://{fqdn}/connect/token` → Bearer. ⚠️ `expires_in` may misreport ~60 s for a ~1 h grant — **trust the JWT `exp` claim**, and do **not** refresh early (a new token invalidates the one a live media stream is holding).
* **Two planes** — don't confuse them: a configuration/management REST plane (OData, config only) vs the **`/callcontrol`** plane (calls + media). This connector is entirely `/callcontrol`.
* **Originate**: `POST /callcontrol/{dn}/makecall` (or `/callcontrol/{dn}/devices/{deviceId}/makecall` for a deterministic participant id under concurrency).
* **Participant control**: `POST /callcontrol/{dn}/participants/{id}/{drop|answer|divert|routeto|transferto}`.
* **Audio (bidirectional)**: `GET /callcontrol/{dn}/participants/{id}/stream` receives the party's audio; `POST` to the same path **injects** audio. Format both ways: **PCM 16-bit, 8 kHz, mono**, HTTP chunked octet-stream.
* **Events**: `wss://{fqdn}/callcontrol/ws` (participant-updated, DTMF, etc.), each event carrying an `entity` path to `GET`. Polling `participants` at ~300 ms is a proven fallback.
* **Codec alignment**: softswitch PCM16 @ 8 kHz ⇄ G.711 @ 8 kHz is a 256-entry lookup-table companding conversion — same rate, **no resampling**. 20 ms / 160-sample framing matches `RtpSender`.

### Foundations (✅ Completed — the enabling base, branch `2.0`)
* 🟢 **`RtpSender` media beachhead** (`119ca84`): first server-sourced RTP path — virtual ext **`440`** streams a one-way G.711 µ-law tone (PCMU PT0, 8 kHz, 20 ms) to the caller, answering with the server's **own** SDP (media port `5062`). Pure helpers `linearToUlaw` / `synthTone` / `buildRtpHeader` are platform-independent and host-unit-tested (`tests/Rtp_test.cpp`); the real socket + 20 ms FreeRTOS pacing task are ESP-only; single-stream cap (2nd dial → `486`).
* 🟢 **Media crashloop fixes** (`b7e82d5`): `udp_receiver_task` 8➔16 KB and `rtp_media_tx` 4➔6 KB stacks (the new SDP/UAC chain overflowed them), plus an `HttpServer::acceptLoop` guard for `std::thread`-spawn `std::system_error` (uncaught throw was rebooting the device).

---

## Resolved Issues

### 🟢 Issue #61: RTP Receive Path + µ-law→PCM16 Decode (uplink)
* **Status**: ✅ Resolved (PR #39 — `RtpReceiver.cpp`)
* **Labels**: `api-integration`, `media`, `rtp`
* **Description**: The inverse of `RtpSender` — receive the handset's RTP on the media socket, strip the RTP header, decode G.711 µ-law➔PCM16, and feed it to the anchor `POST /stream` with a small jitter buffer. Shipped as part of the WAN-anchor media path.

### 🟢 Issue #63: Call-Control Client (token + makecall + /stream + events)
* **Status**: ✅ Resolved (PR #39 — `ThreeCxAnchorClient.cpp`)
* **Labels**: `api-integration`, `anchor`, `http`
* **Description**: The outbound anchor leg per the call-control specification: JWT `exp` token lifecycle management, `makecall` trigger, concurrent `GET`/`POST /stream` (chunked transfer) over mTLS, and WebSocket `wss://` updates to detect call connection and remote hangups. Hardware-confirmed.

### 🟢 Issue #64: Bridge Orchestration (virtual-ext intercept + leg mapping)
* **Status**: ✅ Resolved (PR #39 — `RequestsHandler` ↔ `ThreeCxAnchorClient`)
* **Labels**: `api-integration`, `sip`, `media`
* **Description**: The SIP state machine wired to the anchor leg: intercept INVITEs on virtual extension blocks, negotiate `a=sendrecv` SDP, map SIP INVITE ➔ `makecall`, bridge audio streams, and map SIP BYE/CANCEL ↔ upstream participant drops (reliable teardown via reconcile + watchdog). Hardware-confirmed.

> **Audit cluster (PR #77).** The multi-agent audit's concurrency & memory-safety findings —
> GitHub issues **#41** [C-1] SIP message-pool data race, **#54** [M-2] message-pool exhaustion
> cliff, **#65** [L-1] rx-task TLS-socket leak, **#69** [L-5] park-slot pool pinning, and **#70**
> [L-6] virtual-peer `make_shared` in the hot path — are all **resolved** and closed on GitHub.
> (These audit-renumbered issues are tracked on the GitHub issue tracker; they are distinct from
> this document's older internal numbering.)

### 🟢 Issue #80 / #102: Inbound PSTN Ring-All (Mode 1)
* **Status**: ✅ Resolved (PR #102) — **hardware-verified pre-alpha**
* **Labels**: `api-integration`, `anchor`, `sip`, `media`, `hardware-testing`
* **Description**: An inbound PSTN call to the DID now forks a delayed-offer INVITE to **every
  registered extension** (ring-all); the first 200 OK wins (losing forks CANCELed), the anchor leg is
  answered on the handset's 200, and the `MediaBridge` starts against the answering handset. Two-way
  audio and DTMF confirmed on hardware. Root cause of the prior failure: a route-point-already-Connected
  upset was misclassified as outbound by #43. This cleared the project's inbound-PSTN capability gate;
  soak/stability hardening continues.

### 🟢 Issue #39 / #40 / #95: Reliable Outbound Anchor Teardown
* **Status**: ✅ Resolved (PR #39, #95) — hardware-confirmed
* **Labels**: `api-integration`, `anchor`, `bug`, `critical`
* **Description**: A handset hangup now reliably drops the upstream PSTN leg. Root fixes: control our
  **own** leg (`makecall` result id, else dest-suffix) rather than the WS-surfaced far leg; absorb the
  anchor ACK; well-formed teardown BYE; store the participant id on the answered session; and free the
  audio sockets first so the drop worker can spawn (mbedTLS moved to external/PSRAM allocation, #95).
  Plus a reconcile watchdog and the participant-action `application/json "{}"` body fix.

### 🟢 Issue #91: Register-Beep INVITE Transaction Not Completed on Teardown
* **Status**: ✅ Resolved (PR #91)
* **Labels**: `bug`, `sip`
* **Description**: The register-beep INVITE transaction was never completed on teardown because the
  beep Call-ID lookup never matched — `SipMessage`'s header getter returns the whole `"Name: value"`
  line, not the bare value. Fixed the lookup.

### 🟢 Issues #93 / #99 / #105 / #104 / #98 / #101 / #81-84 / #103: Anchor Performance & Observability
* **Status**: ✅ Resolved (PRs #99, #105, #104, #98, #101, #103) — hardware-confirmed
* **Labels**: `performance`, `anchor`, `observability`, `media`
* **Description**: A performance-hardening + observability wave on the WAN anchor:
  * **#93/#99** TLS session resumption + warm cached control session + keepalive (the S3 has AES/SHA
    but **no ECC/ECDSA hardware**, so the ECDHE handshake is software ~1 s; resumption is the
    workaround — resumed connect ~682 ms → ~116 ms).
  * **#105** Media cut-through: both-ways pre-warm + outbound TLS resumption to kill answer-time dead
    air (the upstream won't route a pre-Connect POST, so the outbound stream is primed one-shot at
    answer), with handshake telemetry.
  * **#104** Playout-buffer cap + drain (≤200 ms ceiling, oldest-sample drop) to kill accumulated
    one-way latency on a bursty TCP far-end; adds `TCP_NODELAY`.
  * **#98** LWIP IRAM optimization for lower RTP/SIP jitter.
  * **#101** Move blocking WS-event work to a worker pool (pool-ready for multi-call); see #86.
  * **#81-84/#103** Soak telemetry in `/api/status` (anchor/TLS/playout/pool/heap-PSRAM/reset-reason).

### 🟢 Issue #48: `RequestsHandler` Mutex Lock Contention under Status Polling
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `performance`, `concurrency`

#### Resolution Details
1. Decoupled HTTP status polling endpoints from the main SIP UDP receiver thread.
2. Implemented a double-buffered `RegistrarSnapshot` and a secondary, lightweight `_snapshotMutex` in `RequestsHandler`.
3. Scheduled a 1Hz statistics snapshot sweep in `RequestsHandler::tick()` to update the snapshot within the core signaling locked section.
4. Updated dashboard query APIs (`getActiveClients()`, `getActiveSessions()`, etc.) to query the snapshot lock-free, completely bypassing core mutex lock contention.

---

### 🟢 Issue #49: Core Task Pinning Imbalance (SIP Signaling & HTTP sharing Core 0)
* **Status**: ✅ Resolved (v1.2.0 / `c7eb41d`)
* **Labels**: `architecture`, `esp32`

#### Resolution Details
1. Pinning imbalance corrected via compile-time define `POCKETDIAL_UDP_RX_CORE` to balance SIP signaling (Core 1) and background HTTP tasks (Core 0).
2. Keeps real-time signaling separate from display rendering and HTTP queries.

---

### 🟢 Issue #50: Synchronous Client Handling blocking `HttpServer` Accept Loop
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `bug`, `network`

#### Resolution Details
1. Converted `HttpServer::acceptLoop()` to use a non-blocking POSIX `select()` architecture with a 250ms timeout.
2. Dispatched client connections to detached threads (`std::thread(...).detach()`), running `handleClient()` asynchronously.
3. Prevents slow clients or long socket operations from stalling the accept loop, securing the dashboard from connection-stall DoS.

---

### 🟢 Issue #51: Move Socket Syscalls outside `RequestsHandler` Critical Sections
* **Status**: ✅ Resolved (v1.1.0 / `eb125ab`)
* **Labels**: `performance`, `refactoring`

#### Resolution Details
1. Outbound socket operations (`sendto`) are decoupled and accumulated inside a local `_outbox` vector.
2. Mutex locked blocks are held strictly during state machine mutations, and the accumulated outbox events are dispatched outside the critical path, reducing lock-hold durations to microseconds.

---

### 🟢 Issue #53 / #54: Null Pointer Dereference `*(RequestsHandler*)nullptr` in Onboarding Setup Mode
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `bug`, `critical`, `smart-display`

#### Resolution Details
1. Changed the `HttpServer` constructor to accept a pointer `RequestsHandler* handler = nullptr` instead of a raw reference, enabling nullable registrar instantiation.
2. Added nullptr guards `if (_handler != nullptr)` inside all web endpoints.
3. Moved `isValidMessage()` to the public section of `SipMessage` to ensure visibility across handlers.
4. Modified `main/esp_main_display.cpp` to pass `nullptr` during onboarding AP fallback mode, avoiding undefined behavior and eliminating Tensilica CPU LoadProhibited boot panics.

---

### 🟢 Issue #55: Dynamic Heap Allocation in Real-Time SIP Signaling Loop
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `performance`, `memory-safety`, `reliability`

#### Resolution Details
1. Eliminated all dynamic runtime heap allocations (`new` and `std::make_shared`) within the active UDP signaling path.
2. Pre-allocated static vectors for up to 32 `SipClient` and 8 `Session` objects inside `RequestsHandler`.
3. Reused pooled elements via a slot-recycling `reset()` pattern. Rejects additional incoming registrations with a robust `503 Service Unavailable` when the pool is saturated, protecting against heap fragmentation and OOM crashes.

---

### 🟢 Issue #56: Buffer Overflow Risk via `strcpy` in WiFi Config Initialization
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `bug`, `security`

#### Resolution Details
1. Replaced all unsafe `strcpy` calls with size-limited, bounds-checking `strlcpy` inside `main/esp_main.cpp` and `main/esp_main_display.cpp`.
2. All SSID (32 bytes) and password (64 bytes) operations are strictly bounded to prevent stack and heap buffer overflows.

---

### 🟢 Issue #57: Unchecked NVS and Driver Return Codes in Display Boot Path
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `bug`, `reliability`

#### Resolution Details
1. Enforced strict return status validations on all `nvs_get_u8` and `nvs_get_str` calls inside `main/esp_main_display.cpp`, defaulting safely to fallback setup mode if flash keys do not exist.
2. Added status validation on the DNS socket `sendto` syscall in `main/wifi/DnsServer.cpp`.

---

### 🟢 Issue #54: Session Pool Slots Permanent Exhaustion
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `bug`, `critical`, `regression-risk`

#### Resolution Details
1. Implemented a `release()` method on the `Session` class to clear all references (`_src`, `_dest`, `_inviteMessage`, `_pendingTargets`, and `_callID`).
2. Configured the `RequestsHandler`'s `endCall()`, `sweepExpired()`, and `forceDisconnect()` methods to explicitly invoke `release()` on active session objects from the pre-allocated pool upon termination.
3. Updated `allocateSession()` to scan and reclaim inactive session slots in the `_sessionPool` whose `Call-ID` is empty or no longer present in the active `_sessions` map, allowing infinite setup/teardown cycles.

---

### 🟢 Issue #55: Address of Record (AOR) Input Injection
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `bug`, `security`, `input-validation`

#### Resolution Details
1. Created `RequestsHandler::isValidAor()` which strictly whitelists alphanumeric characters and `.`, `-`, `_`, `+`.
2. Added AOR sanitization checks inside `onRegister()` and `onInvite()`, rejecting malformed inputs with a `400 Bad Request` response.

---

### 🟢 Issue #56: Compile-time Gated Default-Open Mode (Option B)
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `security`, `configuration`

#### Resolution Details
1. Introduced compile-time guard `POCKETDIAL_OPEN_REGISTRAR` in `RequestsHandler.hpp`, defined by default so that the registrar starts "open" for easy deployment.
2. If `POCKETDIAL_OPEN_REGISTRAR` is commented/undefined, the registrar switches to closed mode, rejecting unauthenticated or non-matching registrations and invites with a secure `403 Forbidden` response.

---

### 🟢 Issue #57 (B): Thread-Safe Buffered Logging Under Lock
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `performance`, `concurrency`

#### Resolution Details
1. Implemented a private `_logQueue` and helper `queueLog()` inside `RequestsHandler` to buffer all `std::cout`/`std::cerr` print statements inside locked critical sections.
2. Configured `handle()`, `tick()`, and `forceDisconnect()` to capture the accumulated logs, clear the queue, release the main mutex `_mutex`, and safely output the logs to the console completely outside of the locked section.

---

### 🟢 Issue #58: Distributed Scanner Memory Exhaustion via Rate-Limit Buckets
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `bug`, `security`, `dos-prevention`

#### Resolution Details
1. Configured `RequestsHandler::tick()` to periodically sweep rate-limit buckets older than 60 seconds from the `_rateBuckets` map.
2. Added a hard cap of `MAX_BUCKETS = 256` inside `allowPacket()`, falling back to drop additional scanning source IP packets to prevent denial-of-service memory exhaustion.

---

### 🟢 Issue #59: Whole-Message Header Mutations Corrupting SIP Body
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `bug`, `reliability`

#### Resolution Details
1. Implemented `SipMessage::findHeader()` to calculate the header-to-body boundary (`\r\n\r\n` or `\n\n`) and restrict searches strictly within the `[0, headerLimit)` range.
2. Modified all header setters in `SipMessage.cpp` (`setVia`, `setFrom`, `setTo`, etc.) to use `findHeader()`, protecting identical substrings in the SDP media/audio body from accidental mutations.
