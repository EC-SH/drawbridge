# Issue Tracking & Architectural Roadmap

This document serves as the active issue tracker and architectural roadmap for **DRAWBRIDGE**. It tracks high-impact concurrency, performance, and hardware-specific issues identified during production deployments, along with their resolution status.

---

## Active Issues & Backlog Roadmap

This backlog is prioritized by architectural dependency and deployment urgency.

### 🔴 Critical Priority: Commercial-Softswitch Call Control & Media Loopback (WAN Bridge)

> **Status (2026-06-18):** The core WAN-anchor capability — RTP receive/decode (#61), the
> call-control client (#63), and bridge orchestration (#64) — **shipped via PR #39** and is
> hardware-confirmed. Since then, on `main` (newest first): **multi-call WAN anchor (#114)** —
> **4 concurrent PSTN calls hardware-validated** (PSRAM task stacks, per-call media bridge pool,
> self-healing abandoned-call + orphan-bridge reapers, `503` at capacity, 48-socket LWIP pool);
> **SIP RFC completeness wave (Phases A–C)** — retransmit layer (RFC 3261 §17), session timers
> (RFC 4028), mid-dialog UPDATE (RFC 3311), and attended+blind transfer (RFC 3515/3891 REFER+Replaces)
> all shipped; **inbound PSTN ring-all (#102/#80)** hardware-verified; **performance-hardening wave**
> — TLS session resumption (#93/#99/#109), media cut-through + playout-buffer (#104/#105), LWIP IRAM
> (#98), WS-event worker pool (#101); connect/teardown dropped from ~1 s toward ~100 ms.
>
> **Backlog expansion (2026-06-18):** Issues #117–#152 added covering: SSH lockout (#117), PBX/TUI
> UX (#118–#123), security foundation (#124, #125, #130–#132, #134–#135), reliability (#127, #128,
> #129, #133), provisioning (#136–#138), raw SIP trunk path (#142–#145), SIP provider interop
> (#146–#151), site federation (#152), and long-tail features (#126, #139–#141).
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

#### 🟡 Issue #117: SSH re-enable: disabling SSH in TUI must not self-lock — add web dashboard toggle
* **Status**: ⏳ Open
* **Labels**: `security`, `ssh`, `tui`, `priority-high`
* **Severity**: High
* **Description**: Disabling SSH via [4] SECURITY is currently irreversible from the network — the only SSH toggle is inside the SSH TUI. Add a `POST /api/ssh/enable` dashboard endpoint (admin-session-gated) and/or a dial-string escape hatch so an operator can recover without physical access or factory reset.

---

### 🟢 Medium Priority: PBX Features & TUI

#### 🟢 Issue #118: TUI: page zone (98x) management screen not yet exposed in SSH TUI
* **Status**: ⏳ Open
* **Labels**: `feature`, `tui`, `pbx`
* **Severity**: Medium
* **Description**: The engine supports up to 10 page zones (980–989) with NVS persistence, but zone creation and member assignment have no TUI surface. Operators must use the 999 all-page or scripted provisioning. A Page Zones tab (mirroring the Ring Groups tab) is needed for field usability.

#### 🟢 Issue #119: TUI: IVR tab is a non-functional stub — hide or remove until backend ships
* **Status**: ⏳ Open
* **Labels**: `tui`, `ux`
* **Severity**: Medium
* **Description**: The IVR tab under [3] PBX CONFIG renders and accepts input but silently discards it — the backend is not implemented. For a commercial release this must either be hidden entirely or show a clear "NOT YET AVAILABLE" banner with no editable fields.

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

#### 🟢 Issue #120: Park recall: play intercom beep after 5-minute hold with orbit+retrieval caller ID
* **Status**: ⏳ Open
* **Labels**: `pbx`, `feature-request`, `priority-high`
* **Severity**: High
* **Description**: When a call has been parked on an orbit for more than 5 minutes without retrieval, ring back the extension that parked it with an intercom auto-answer INVITE. Caller ID should show the orbit number and the retrieval star code (e.g., "Orbit 701 — dial **701 to retrieve"). Prevents forgotten parked calls from sitting indefinitely.

#### 🟢 Issue #121: TUI: clickable dialogs and mouse event handling (xterm mouse protocol)
* **Status**: ⏳ Open
* **Labels**: `tui`, `feature-request`, `priority-medium`
* **Description**: The SSH TUI is keyboard-only. Add mouse event support via the xterm SGR mouse protocol (`\x1b[?1006h`). Events arrive through `feed(bytes)` as `\x1b[<btn;col;rowM/m`. Use for: clicking list items, clicking guarded-confirm buttons, clicking tab headers. Keyboard navigation remains the primary path; mouse is an ergonomic enhancement.

#### 🟢 Issue #122: Browser-based TUI via xterm.js WebSocket session (no SSH proxy)
* **Status**: ⏳ Open
* **Labels**: `tui`, `feature-request`, `priority-medium`
* **Description**: Serve the full ANSI TUI in a browser via a WebSocket endpoint at `/ws/tui`. The browser runs xterm.js; the device runs the existing `Tui` engine with a WebSocket writer/reader instead of SSH. PIN auth on the WebSocket handshake (no SSH client required). This makes the TUI accessible from any browser on the LAN without an SSH client.

#### 🟢 Issue #123: Config export/import — NVS snapshot with optional password-gated anchor credentials
* **Status**: ⏳ Open
* **Labels**: `api`, `feature-request`, `priority-medium`
* **Description**: Add `GET /api/config/export` (JSON snapshot of extensions, ring groups, call-forward rules, dial plan, PIN hash) and `POST /api/config/import`. Anchor credentials are absent from the unprotected export — only included when the operator supplies a password that wraps the export in AES-256-GCM (PBKDF2 key derivation via mbedTLS). Pairs with NVS schema versioning (#133); export embeds `schema_ver` for import compatibility checking.

#### 🟢 Issue #126: Dial plan: NVS-backed prefix routing table with TUI editor
* **Status**: ⏳ Open
* **Labels**: `pbx`, `feature-request`, `priority-high`
* **Description**: Replace the hard-coded `9`-prefix outbound routing with a runtime-configurable prefix→route table persisted to NVS. Each entry: `prefix`, `strip_digits`, `route_to` (extension, trunk, or peer URI). TUI editor under [3] PBX CONFIG → **Dial Plan** tab. Prerequisite for DRAWBRIDGE-to-DRAWBRIDGE federation (#152) and SIP trunk routing (#142).

---

### 🔴 Critical Priority: Security Foundation

#### 🔴 Issue #124: WPA2-PSK on SoftAP — open access point is the primary threat surface
* **Status**: ⏳ Open
* **Labels**: `security`, `network`, `priority-critical`
* **Severity**: Critical
* **Description**: The SoftAP currently runs open (no passphrase). Any device on the same radio can join, reach the SIP registrar, and attempt to register extensions. Fix: set `authmode = WIFI_AUTH_WPA2_PSK` in `esp_wifi_ap_config_t` with a unique per-device PSK (generated at first boot, displayed via TUI [2] NETWORK, QR-encoded on the display build). This is the primary security control for SoftAP deployments; HTTPS (#134) and SRTP (#135) depend on this being in place first.

#### 🟡 Issue #125: SIP digest auth for INVITE — REGISTER auth ships; INVITE challenge is incomplete
* **Status**: ⏳ Open
* **Labels**: `sip-engine`, `security`, `priority-high`
* **Description**: REGISTER digest challenge (RFC 2617 MD5) is shipped in Secure mode. INVITE challenges are not uniformly applied — an attacker who can reach the UDP port can call any extension without credentials. Extend the challenge machinery to INVITE: 401 challenge on first attempt, accept only if the digest matches the registered extension's HA1.

#### 🟡 Issue #130: Security: per-source-IP brute-force lockout on /api/admin/login
* **Status**: ⏳ Open
* **Labels**: `security`, `priority-high`
* **Description**: The current global 5-attempt / 60-second lockout lets an attacker on the LAN self-DoS the legitimate admin (D-3 in THREAT_MODEL.md). Replace with a per-IP `unordered_map<uint32_t, LockoutState>` bounded to 64 entries (evict oldest on overflow). Each IP gets its own independent window.

#### 🟡 Issue #131: OTA: bind upload to admin session + local-link-only gate
* **Status**: ⏳ Open
* **Labels**: `ota`, `security`, `priority-high`
* **Description**: OTA upload is currently gated by PIN + CSRF only, accepting connections from any IP. Add: (1) reject upload requests from source IPs outside the device's own subnet; (2) require a `pd_session` cookie minted in the current boot (stale cookies from NVS not accepted for OTA). Interim hardening before Secure Boot v2 (#132).

---

### 🔴 Critical Priority: Raw SIP Trunk Path

> The current WAN anchor uses a proprietary call-control API. Issues #142–#145 together form the raw SIP trunk epic — enabling DRAWBRIDGE to register directly to any standard SIP provider as a SIP UAC.

#### 🔴 Issue #142: SIP trunk: outbound registration to upstream SIP provider (UAC REGISTER + re-register)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `priority-critical`
* **Severity**: Critical
* **Description**: Implement `SipTrunkClient` to send outbound REGISTER to a provider SIP proxy with digest auth. Credentials from NVS (`trunk_user`, `trunk_pass`, `trunk_proxy`). Re-registration on `Expires` timer with exponential backoff on failure. OPTIONS keepalive to detect trunk DOWN. TUI [3] PBX CONFIG → **Trunk** tab showing registration status. Prerequisite for all raw SIP trunk interop (#146–#151).

#### 🔴 Issue #143: SIP trunk: NAT traversal (rport, symmetric RTP/comedia, Contact rewrite)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `network`, `priority-critical`
* **Severity**: Critical
* **Description**: Standard NAT traversal for a trunk behind a typical office/home NAT: `rport` (RFC 3581) in Via headers; comedia RTP latching (send from the same port as receive); Contact rewrite to the observed public IP:port from Via `received`/`rport`. Explicitly out of scope: ICE, STUN, TURN (those are WebRTC/peer-mesh tools, not SIP trunking tools).

#### 🟡 Issue #145: SIP trunk: B2BUA media bridge for trunk leg ↔ extension leg
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `media`, `priority-high`
* **Description**: For raw SIP trunking the device acts as B2BUA: terminate the extension's RTP on one side and the trunk provider's RTP on the other, relaying G.711 via the existing `MediaBridge` pool. Inbound: answer trunk INVITE with a MediaBridge-owned SDP; fork to extension; splice. Outbound: send INVITE to trunk; splice on 200 OK. Depends on #142, #143.

#### 🟡 Issue #144: SIP trunk: TLS/SIPS signalling + SRTP media for encrypted trunk
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `security`, `media`, `priority-high`
* **Description**: Add TLS-wrapped TCP SIP transport (SIPS, port 5061) using `mbedTLS` (already linked). SDES key exchange (`a=crypto`) for SRTP media. Fall back to UDP/RTP if the provider doesn't advertise TLS/SRTP. Required by Telnyx, Voxtelesys, and most Tier-1 providers for production trunks. Depends on #142.

#### 🟡 Issue #152: DRAWBRIDGE-to-DRAWBRIDGE SIP federation (registration-based, no NAT/STUN/ICE/TURN)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `feature-request`, `priority-high`
* **Description**: Multi-site federation: Device B registers to Device A as a trunk extension (e.g. prefix `8xx`). Calls to `8xx` on Device A route to Device B, which delivers them locally. NVS config: `peer_uri`, `peer_user`, `peer_pass`, `peer_prefix`. TUI [3] PBX CONFIG → **Peers** tab. No ICE, no STUN, no TURN — both devices on known IPs (LAN or WireGuard/VPN overlay). Depends on dial plan (#126).

---

### 🟡 High Priority: SIP Provider Interoperability

> Interop validation issues against specific SIP providers. All depend on the raw SIP trunk foundation (#142–#145).

#### 🟡 Issue #146: Interop: Voxtelesys (wholesale carrier, TLS+SRTP)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `interop`, `priority-high`
* **Description**: Validate DRAWBRIDGE's raw SIP trunk path against Voxtelesys: REGISTER + digest auth, inbound DID, TLS on 5061, SRTP SDES, NAT traversal, DTMF PT101, call transfer (REFER), inbound PAI passthrough.

#### 🟡 Issue #147: Interop: Telnyx (CPaaS, API-first, TLS+SRTP required)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `interop`, `priority-high`
* **Description**: Validate against Telnyx SIP trunks: REGISTER to `sip.telnyx.com`, TLS required, SRTP enforced (Telnyx rejects unencrypted media), `X-Telnyx-*` header passthrough, E911 routing limitation documentation, concurrent call limit + 503 handling.

#### 🟡 Issue #148: Interop: Sansay VSXi/X Series SBC (carrier SBC peering)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `interop`, `priority-high`
* **Description**: Validate against Sansay SBC as upstream trunk: topology hiding (rewritten Contact/Via handling), SRTP, re-INVITE + RFC 3311 UPDATE (both shipped), attended transfer REFER relay, RFC 4028 session timer enforcement (shipped), early media 183 + `clearBody()` interaction.

#### 🟡 Issue #149: Interop: PortaOne/PortaBilling (CLEC/MVNO carrier platform)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `interop`, `priority-high`
* **Description**: Validate against PortaSwitch as upstream trunk: `P-Charge-Info` / `P-Called-Party-ID` header passthrough, Call-ID integrity for CDR correlation, 503 `Retry-After` handling, TLS on Porta deployments that require it. Commercially critical — PortaOne is the most common billing platform at CLECs (including the parent company).

---

### 🟡 High Priority: Provisioning & Reliability

#### 🟡 Issue #136: Provisioning TUI editor — MAC→extension map, capacity meter, token regen
* **Status**: ⏳ Open
* **Labels**: `tui`, `provisioning`, `priority-high`
* **Description**: Add a Provisioning tab under [3] PBX CONFIG showing the MAC→extension binding table (columns: MAC, Ext, Status, Last seen). Actions: `[A]` add binding, `[D]` delete/revoke, `[R]` regenerate token, `[W]` open/close TOFU window. Capacity meter: `X / MAX_CLIENTS` bindings used. Depends on ZTP provisioning MVP (#35).

#### 🟡 Issue #127: Platform reliability: task watchdog + heap/stack high-water-mark monitoring
* **Status**: ⏳ Open
* **Labels**: `reliability`, `diagnostics`, `priority-high`
* **Description**: Add a `ReliabilityMonitor` task (low-priority, 5s tick) that records per-task stack HWM, free heap (internal + PSRAM), and resets ESP task watchdog. Surface the data in `/api/status` and TUI [1] SYSTEM MONITOR. Alert (log + TUI indicator) when any stack HWM is within 512 bytes of overflow or free heap drops below a configured threshold.

---

### 🔵 Low Priority: Diagnostics & Observability

#### 🔵 Issue #128: /metrics — Prometheus text-format endpoint for external scraping
* **Status**: ⏳ Open
* **Labels**: `diagnostics`, `api`, `priority-medium`
* **Description**: Add `GET /metrics` returning Prometheus text format (no auth required for scraping): `sip_registrations_total`, `sip_calls_active`, `sip_calls_total`, `anchor_calls_active`, `heap_free_bytes` (internal + PSRAM), `uptime_seconds`, `rtp_packets_tx/rx`. Enables integration with Grafana/Prometheus in managed deployments.

#### 🔵 Issue #129: RFC 5424 syslog over UDP for centralized log aggregation
* **Status**: ⏳ Open
* **Labels**: `diagnostics`, `priority-medium`
* **Description**: Add a syslog sink alongside the current serial log: when `syslog_host` NVS key is set, emit RFC 5424-formatted UDP syslog datagrams (facility=LOCAL0, structured data for call events). Zero-alloc: format into a stack buffer; fire-and-forget `sendto`. No TCP/TLS — plain UDP syslog only (reliable delivery is the aggregator's concern).

#### 🔵 Issue #133: NVS schema versioning and migration framework
* **Status**: ⏳ Open
* **Labels**: `reliability`, `priority-low`
* **Description**: Add a `schema_ver` NVS key (uint16) written at first boot. On startup, if `schema_ver < CURRENT`, run a `migrations[]` table in order (`{from, to, fn}`). Each migration runs in an NVS transaction; failure boots with defaults for affected keys. Export (#123) embeds `schema_ver`; import rejects files with a newer schema than the running firmware.

#### 🔵 Issue #132: Security P2: Secure Boot v2 + flash encryption + signed OTA
* **Status**: ⏳ Open
* **Labels**: `security`, `priority-low`
* **Description**: ECDSA-signed firmware images (Secure Boot v2), NVS flash encryption at rest, and signed OTA images. One-way eFuse burn — requires a secured factory provisioning flow before enabling. Until then, OTA is hardened by the session-bind gate (#131). Do not ship this before the factory PKI/key-management flow is ready.

#### 🔵 Issue #134: Security P2: optional self-signed HTTPS for the HTTP dashboard
* **Status**: ⏳ Open
* **Labels**: `security`, `api`, `priority-low`
* **Description**: Opt-in HTTPS via `mbedTLS` (already linked). Self-signed cert generated at first boot, stored in NVS, fingerprint shown in TUI / `/api/status`. HTTP continues to work on the LAN; HTTPS is for compliance-sensitive deployments only. WPA2 (#124) is the correct primary control; HTTPS is additive.

#### 🔵 Issue #135: Security P2: SRTP for LAN media (complement to WPA2, not a replacement)
* **Status**: ⏳ Open
* **Labels**: `security`, `media`, `priority-low`
* **Description**: SDES key exchange in SDP offer/answer for LAN P2P calls. `enforceG711()` would need to preserve/inject `a=crypto` lines. Meaningful only on top of WPA2 (#124) — WPA2 encrypts the link; SRTP encrypts the application layer for wired/compliance deployments. Large effort relative to security gain given WPA2.

#### 🔵 Issue #150: Interop: Apidaze/Bandwidth (CPaaS trunk)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `interop`, `priority-medium`
* **Description**: Validate against Apidaze/Bandwidth SIP infrastructure: REGISTER + digest auth, inbound DID, TLS on 5061, SRTP SDES, `X-Apidaze-*` header passthrough, Bandwidth network peering behaviour, concurrent call limits.

#### 🔵 Issue #151: Interop: Metaswitch/Perimeta SBC (Microsoft-acquired carrier platform)
* **Status**: ⏳ Open
* **Labels**: `sip-trunk`, `interop`, `priority-medium`
* **Description**: Validate against Metaswitch Perimeta: strict RFC 3261 conformance, `History-Info` headers, RFC 4028 session timer enforcement (shipped), RFC 3311 UPDATE (shipped), REFER+Replaces (shipped). **Known gap**: Metaswitch may require `Require: 100rel` (RFC 3262 PRACK) which DRAWBRIDGE does not implement — document the gap before claiming full interop.

#### 🔵 Issue #137: Provisioning P2: DHCP Option 66 zero-touch (fork bundled dhcpserver)
* **Status**: ⏳ Open
* **Labels**: `provisioning`, `priority-low`
* **Description**: Inject Option 66 (TFTP server / provisioning URL) into DHCP responses for SoftAP mode. Phones discover the provisioning URL automatically at boot — no manual entry. Requires patching the IDF bundled `dhcpserver` (version-sensitive; documented maintenance cost). Only applies to SoftAP; wired deployments need operator-configured DHCP.

#### 🔵 Issue #138: Provisioning P2: multi-vendor auto-config renderers (Grandstream, Polycom, Cisco)
* **Status**: ⏳ Open
* **Labels**: `provisioning`, `priority-low`
* **Description**: Extend ZTP (#35) beyond Yealink `.cfg` format to cover Grandstream XML (`cfg<MAC>.xml`), Polycom two-file XML, and Cisco SPA/SEP XML. Detect vendor from HTTP `User-Agent` in the provisioning GET; serve the appropriate format. ~2–4 KB `.text` per vendor renderer.

#### 🔵 Issue #139: Voicemail: route unanswered calls to external SIP UA (on-device record is a non-goal)
* **Status**: ⏳ Open
* **Labels**: `voicemail`, `pbx`, `priority-low`
* **Description**: On-device voicemail (RTP record/playback) is a non-goal. Viable path: operator registers an external VM server as extension `900`; CFNA on each extension points to it. This works today via CFNA config (doc-only change). Engineering addition: MWI (`SUBSCRIBE`/`NOTIFY` for `message-summary`) relay from the VM UA back to the registered extensions so voicemail indicator lights work.

#### 🔵 Issue #140: Conferencing: relay to external mixer (on-device mixing is a non-goal)
* **Status**: ⏳ Open
* **Labels**: `pbx`, `priority-low`
* **Description**: N-way conference via relay to an external bridge (FreeSWITCH conference room, Jitsi). Operator configures a bridge extension (e.g. `800`); participants dial it; DRAWBRIDGE forwards each INVITE to the bridge. Engineering addition: multi-party session tracking (Session currently holds two parties) and transfer-into-conference (REFER to bridge URI).

#### 🔵 Issue #141: Multi-AP/mesh for SoftAP deployments beyond 16-station cap
* **Status**: ⏳ Open
* **Labels**: `network`, `priority-low`
* **Description**: ESP32 SoftAP hard-limits to ~16 stations. For larger deployments: (1) wired backbone of APs sharing the same SSID (recommended — no firmware changes, just deployment topology), or (2) ESP-NOW mesh with one unit as registrar. Document Option 1 as the recommended path. The correct long-term answer for >16 phones is the wired Ethernet build.

---

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

> **Goal**: let a DRAWBRIDGE handset place calls to **upstream extensions** over WAN by bridging its SIP/RTP world to a commercial softswitch's HTTP-based **call-control API**.
>
> **Why no SBC / NAT / STUN / TURN / ICE is needed**: the connector *terminates the handset's RTP locally* (one SIP hop, on-box) and re-originates the call into the softswitch over **HTTPS, not SIP/RTP** — so there is no second SIP/RTP peer for ICE to traverse. Both legs are 8 kHz, so media is a pure companding swap (G.711 ⇄ PCM16) with **no resampling and no media server**. Over WAN this only requires a flat L3 (WireGuard/overlay or public IP) so the connector's advertised SDP address is directly reachable.

### Architecture Decision
The connector is a **media-terminating SIP endpoint** that `REGISTER`s to pocketdial-desktop as an ordinary extension (e.g. `3000`, or a dialing prefix). DRAWBRIDGE's **existing** `onInvite` forward path (`RequestsHandler.cpp`) delivers the INVITE to it with **zero changes to the signaling-only server** — the registrar keeps sourcing no media. The connector mirrors the **`440` `RtpSender` beachhead**, except it:
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
