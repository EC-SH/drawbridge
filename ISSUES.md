# Issue Tracking & Architectural Roadmap

This document serves as the active issue tracker and architectural roadmap for **DRAWBRIDGE**. It tracks high-impact concurrency, performance, and hardware-specific issues identified during production deployments, along with their resolution status.

---

## Active Issues & Backlog Roadmap

This backlog is prioritized by architectural dependency and deployment urgency.

### 🟡 Issue #175: Fielded devices keep their NVS-persisted admin extension after the `101`→`1001` default change
* **Status**: ⏳ Open / Decision needed
* **Labels**: `security`, `provisioning`
* **Severity**: Medium

#### Description
`_adminExt` is NVS-persisted here (`loadAdminExt()`/`saveAdminExt()`, ns `pbxcfg`, key `admin_ext`) — the persisted value wins at boot, so the admin-http-gate landing's compile-time default change `101`→`1001` has no effect on any already-provisioned unit. On such a unit the `*4887` HTTP-open trigger works from the **old** admin extension while the docs say `1001`. Decide: one-time NVS migration on upgrade, document-and-leave (least invasive), or re-provision at next field visit. Don't default into silently changing which handset can open the dashboard. GitHub #175.

---

### 🟡 Issue #176: Already-provisioned admin PINs beginning `4887` are shadowed by the `*4887` star-code
* **Status**: ⏳ Open / Documented residual
* **Labels**: `security`, `dtmf`
* **Severity**: Low

#### Description
`*4887` is matched incrementally in `onDtmfInfo` before the `*PIN#code` parser, so a PIN beginning `4887` can never complete a DTMF admin command. Set-pin now rejects the prefix (new/changed PINs only); a device provisioned before the guard keeps the collision, and the salted+hashed store rules out a boot-time scan. Affects only the `Channel::Dtmf` menu — SSH/HTTP login with the same PIN are unaffected. Practical ceiling: documentation (CHANGELOG) + optionally a behavioral warning when the star-code fires and the call continues with `#`-digits. GitHub #176.

---

### 🟡 Issue #177: Park-orbit INVITE response occasionally never reaches the caller on real hardware (host build unaffected)
* **Status**: ⏳ Open / Needs on-device instrumentation
* **Labels**: `sip`, `hardware-testing`, `reliability`
* **Severity**: Medium
* **Description**: Found 2026-07-22 by `.smoke/office_smoke.py`'s park+retrieve scenario run against real LilyGO T-ETH-ELITE hardware (COM5): an extension's INVITE to a free park orbit (`700`) sometimes gets **no response at all** (client-side timeout, even after RFC 3261 §17.1.1-style retransmission) — despite the serial log confirming the server-side handler ran to completion (`Park: <ext> parked on 700` is the very last statement in `onParkInvite`'s PARK branch, meaning the 200 OK was constructed and `_outbox.emplace_back`'d). Root cause NOT yet isolated: candidates are (a) something between outbox-enqueue and the actual `sendto()` silently failing for this specific response, or (b) a resource/table exhaustion specific to a real ESP32 handling several near-simultaneous real-hardware clients (the same log window showed `Register beep: table full, skipping beep for <ext>` and a register-beep going unanswered from a brand-new client, suggesting SOME class of inbound-delivery-to-client issue beyond just this one response). **Not reproduced on the WSL host build** — `office_smoke.py` passed the same scenario 10/10 across three consecutive fresh-server runs there, so this looks hardware/timing-specific rather than a logic bug in `onParkInvite` itself. A 1s settle delay after REGISTER made an *isolated* single-call repro succeed, but did NOT fix it when park is the SECOND call from an extension that just completed a prior call (the office-smoke scenario's actual shape) — so "let the client settle after registering" is not the real fix, just a data point. Needs on-device instrumentation (temporary ESP_LOG around the outbox flush, or a LAN-side packet capture) to pin down whether the response is genuinely never sent or sent-but-lost. Every OTHER call type exercised in the same test run (777 echo, blind transfer, attended transfer, DND-invite, ring group, 999 page) got its response back fine in the same session, so this looks park-specific, not a blanket real-hardware SIP-response problem. **Note for whoever picks this up:** `office_smoke.py`'s `invite()` now retransmits per RFC 3261 §17.1.1 — if the orbit is slow to respond on real hardware, a retransmitted PARK INVITE can arrive *after* the orbit's already been Parked, landing in `onParkInvite`'s RETRIEVE branch instead (which frees the orbit and re-INVITEs). That would look exactly like "the caller never got its response" from the harness's point of view, but the actual defect would be the harness's own retransmit racing the real response, not a silent drop. Rule that out with a packet capture before chasing outbox/sendto theories further.

---

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
> **Backlog expansion (2026-06-18, wave 1):** Issues #117–#152 added covering: SSH lockout (#117),
> PBX/TUI UX (#118–#123), security foundation (#124, #125, #130–#132, #134–#135), reliability
> (#127–#129, #133), provisioning (#136–#138), raw SIP trunk path (#142–#145), SIP provider interop
> (#146–#151), site federation (#152), and long-tail features (#126, #139–#141).
>
> **Backlog expansion (2026-06-18, wave 2):** Issues #153–#163 added covering: owner/sysop privilege
> model (#153), mDNS pre-gate (#154), SSH dead on wifi/lan8720 (#155), `owner@` username plumbing
> (#156), littlessh buffered repaint (#157), first-run wizard (#158), command palette (#159),
> accessibility STATIC mode (#160), RFC 3262 PRACK (#161), TUI provider de-dup (#162), CDR
> persistence (#163). Issue #84 closed / consolidated into #127.
>
> **Backlog expansion (2026-06-25, wave 3 — Media / Conferencing):** Issues #164–#170 added for the
> on-device **N-way conference Mix Bus** epic (#164) and its open pieces: `MediaBridge` integration
> (#165), the handset→bus input ring (#166), the mix-tick driver decision (#167), the int32 PIE
> parity kernel pending TRM verification (#168), per-port repacketization for non-20 ms ptime
> codecs (#169), and VAD energy-gating (#170). **This wave deliberately overturns a documented
> non-goal** — `docs/FEATURE_ROADMAP.md` §6 / `docs/SCALING.md` §1 declare on-MCU mixing impossible;
> the scalar `MixBus` + master-clock tick + per-port state machine in `drawbridge-conference-mixer/`
> (compiled, `-Wall -Wextra`-clean, self-test green) demonstrate otherwise. Roadmap reconciliation is
> in scope for #164.
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

---

### 🟡 High Priority: SSH Foundation & First-Boot Flow

#### 🟡 Issue #153: Security: owner/sysop two-role privilege model — real authz, not landing-screen separation
* **Status**: ⏳ Open
* **Labels**: `security`, `ssh`, `priority-high`
* **Severity**: High — MUST-FIX before commercial deployment
* **Description**: Both SSH roles currently share one admin PIN. Add a dedicated owner credential (`owner_salt`/`owner_hash`), resolve role at auth time (constant-time comparison against both hashes), stash role in session state, and gate destructive operations (`applyFactoryReset()`, NVS backup/restore) with a per-action `isOwner()` check. Extend brute-force lockout key from `Channel` to `(Channel, Principal)` to prevent sysop PIN spraying from locking out the owner recovery path. See also: #117 (SSH re-enable lockout), #130 (per-IP HTTP lockout).

#### 🟡 Issue #154: Infra: hoist `mdns_init` before provisioning gate so `drawbridge.local` resolves on first boot
* **Status**: ⏳ Open
* **Labels**: `network`, `priority-high`
* **Description**: mDNS currently starts inside the `SipServer` constructor — after the provisioning gate. A factory-fresh unit never starts mDNS, so `drawbridge.local` does not resolve and `ssh owner@drawbridge.local` fails on first boot. Fix: hoist `mdns_init()` + `mdns_hostname_set("drawbridge", NULL)` into `app_main` before the provisioning gate in each entry point. Also locks the mDNS hostname to `"drawbridge"` (product identity — not `pocketdial.local`).

#### 🟡 Issue #155: SSH: `SshServer::start()` never called in wifi + lan8720 entry points — SSH dead on those builds
* **Status**: ⏳ Open
* **Labels**: `ssh`, `priority-high`
* **Description**: `esp_main.cpp` (wifi) and `esp_main_eth_lan8720.cpp` link littlessh but never call `SshServer::start()`. SSH is silently absent on those firmware variants — no error, just no port 22. Fix: add `SshServer::start()` before the provisioning gate in both entry points, mirroring `esp_main_eth.cpp`.

#### 🟡 Issue #156: SSH: wire `owner@` username through `ll_on_open` in littlessh so Guided Mode is reachable
* **Status**: ⏳ Open
* **Labels**: `ssh`, `tui`, `priority-high`
* **Description**: `ll_on_open` currently discards the SSH username (`(void)user`). As a result `ssh owner@drawbridge.local` opens the same Expert Mode as `ssh sysop@drawbridge.local` — Guided Mode is unreachable over eth/wifi/lan8720 SSH. Fix: stop discarding `user`; store it in session state and pass it to `setUsername()` (same as the wolfSSH path already does). Depends on privilege model (#153).

#### 🟡 Issue #157: SSH performance: littlessh buffered repaint — one `lssh_write()` per screen, not per `put()`
* **Status**: ⏳ Open
* **Labels**: `ssh`, `performance`, `priority-high`
* **Description**: The littlessh `setWriter` lambda calls `lssh_write()` on every `Tui::put()` call — a full screen render sends ~50–150 encrypted SSH packets. Fix: buffer the whole screen into a persistent `_frame` string (`.reserve(8192)` at session open, `.clear()` keeps capacity), then a single `lssh_write()` per repaint. No changes to `Tui.cpp`. `_frame` lives on the heap (PSRAM-friendly), outside the SIP zero-alloc hot-path.

#### 🟡 Issue #158: TUI P1: first-run wizard — guided onboarding for fresh-deployed unit
* **Status**: ⏳ Open
* **Labels**: `tui`, `feature-request`, `priority-high`
* **Description**: No guided first-boot flow exists. When no owner PIN is provisioned, trigger a wizard: (1) welcome + hostname, (2) set owner PIN, (3) optional sysop PIN, (4) registrar mode selection, (5) live test-call verify (dial 777), (6) quick-reference card. `owner@` login only; Esc exits to Expert at any step. Requires a `ext_name_<dn>` NVS key for extension names (~1 KB budget).

---

### 🟢 Medium Priority: TUI & Accessibility

#### 🟢 Issue #159: TUI P2: command palette (`:` / Ctrl-P) — verb-based command entry over existing setter callbacks
* **Status**: ⏳ Open
* **Labels**: `tui`, `feature-request`, `priority-medium`
* **Description**: Power-user / scripted management: type `:set ext 1001 name "Reception"` or `:trunk status` instead of navigating the menu tree. N=64 byte buffer, Tab completion over a fixed verb list, dispatches to the existing setter callbacks. Owner-only verbs (`reboot`, `factory-reset`) require `isOwner()`. Additive — keyboard navigation unchanged.

#### 🟢 Issue #160: TUI P1: accessibility — STATIC/screen-reader mode, ASCII tier auto-select, mouse OFF by default
* **Status**: ⏳ Open
* **Labels**: `tui`, `priority-medium`
* **Description**: The 1Hz `tickLive()` → full repaint is hostile to screen readers. Three additive fixes: (1) STATIC mode gate — `tickLive()` skips repaint; refresh on Ctrl-L (promotes existing `_monFrozen` early-return); (2) ASCII tier auto-select for SR clients — `glyph→WORD` table when `setUnicode(false)` is triggered (code path exists, trigger doesn't); (3) mouse mode OFF at session open, opt-in only (capture breaks selection-to-speech). A11Y-1/2 from `docs/design/accessibility.md`.

---

### 🟡 High Priority: Media — On-Device N-Way Conference Mix Bus

> **Overturns a documented non-goal.** `docs/FEATURE_ROADMAP.md` §6 and `docs/SCALING.md` §1 currently
> state that on-MCU media mixing is impossible ("the server never touches RTP" / "no DSP budget"). The
> WAN anchor already breaks the no-touch-RTP rule for the PSTN leg, and linear int16 PCM is already the
> internal currency at the anchor boundary (`MediaBridge` / `feedRx` / `writeAudio` speak linear; µ-law
> lives only at the handset rim). The missing operation is the **summing junction**. A working scalar
> reference + master-clock tick + per-port concurrency model lives in `drawbridge-conference-mixer/`
> (`CONFERENCE_MIXER.md` + `src/` + `test/mixbus_selftest.cpp`), built standalone with no ESP toolchain.

#### 🟡 Issue #164: Media: on-device N-way conference Mix Bus (the summing junction) — EPIC
* **Status**: ⏳ Open
* **Labels**: `media`, `feature-request`, `priority-high`, `epic`
* **Description**: Add a single `MixBus` that owns the **mix tick** as master clock. Each call attaches a **port** (two `PlayoutBuffer` rings: leg→bus and bus→leg) and hears the **N−1 mix** — the saturated sum of all *other* ports. An ordinary 1:1 call falls out as the **N=2 case**, so the mixer subsumes the point-to-point anchored path and the single-bridge special-casing retires. **The one invariant the design hinges on: never saturate the running mix** — sum into an int32 accumulator, subtract self, saturate exactly once at per-port output. The over-saturation trap (four legs at +30000 where a pre-clipped mix makes a loud talker hear 2767 instead of 32767) is asserted green in the self-test. Bundle status: scalar `MixBus` (attach/detach/tick), `Free→Active→Draining→Free` per-port state machine (tick is sole ring-clearer — the #100 lesson generalized to N legs), and the confirmed-ISA PIE kernel all compiled and `-Wall -Wextra`-clean. **Scope also includes reconciling `docs/FEATURE_ROADMAP.md` §6 + `docs/SCALING.md` §1** so the non-goal language matches reality. Sub-issues: #165–#170. Source: `drawbridge-conference-mixer/CONFERENCE_MIXER.md`.

#### 🟡 Issue #165: Media: wire the Mix Bus into `MediaBridge` (the integration diff)
* **Status**: ⏳ Open
* **Labels**: `media`, `priority-high`
* **Description**: `MediaBridge` keeps the RTP sockets + rim companding and rewires two callbacks at the bus: RX `mulawDecodeBuffer → bus.inputFrame(port, pcm, n)`, TX `bus.outputFrame(port, pcm, n) → ulawEncodeBuffer`. `startBridge`: `port = bus.attach()` then wire; `stopBridge`: `bus.detach(port)` then stop sockets. The anchor/PSTN leg is already linear → it drops onto the bus as just another port via `bus.inputFrame(anchorPort, …)` with no compand. Retire the one-mutex single-bridge path once N ports + one shared tick driver replace it. Depends on #164. (§7 of the design doc.)

#### 🟡 Issue #166: Media: add the handset→bus input-direction `PlayoutBuffer` (second ring per port)
* **Status**: ⏳ Open
* **Labels**: `media`, `priority-high`
* **Description**: Today only the anchor→handset direction is buffered. The mixer needs the handset→bus direction buffered too so the tick can drain one frame per port in lockstep and absorb each leg's RTP jitter. Same `PlayoutBuffer` class, a second instance per port. A late leg contributes zeros for that tick and the tick never stalls. Depends on #164. (Trap #3 / §3 of the design doc.)

#### 🟡 Issue #167: Media: mix-tick driver — dedicated 20 ms timer vs. existing sender cadence (jitter-stability decision)
* **Status**: ⏳ Open
* **Labels**: `media`, `priority-high`, `needs-investigation`
* **Description**: The tick cadence is inviolable; everything slots around it. **Open decision (wants hardware data):** hang the tick off the existing 20 ms RTP sender cadence, or run a dedicated periodic task/timer — whichever is more jitter-stable on the **T-ETH-ELITE** (eth) build, where the W5500 MACRAW path and LWIP socket pressure already shape timing. Measure tick-to-tick jitter both ways on hardware before committing. Depends on #164. (§3 / §8 of the design doc.)

#### 🟢 Issue #168: Media: int32 sum-once-subtract PIE kernel — TRM-verify before trusting (scalar stays default)
* **Status**: ⏳ Open
* **Labels**: `media`, `performance`, `priority-medium`, `needs-investigation`
* **Description**: The small-conference path (`pie/mix_sum4_s16.S`, ≤5-way) uses **only confirmed S3 instructions** (`ee.vld.128.ip` / `ee.vadds.s16` / `ee.vst.128.ip`, envelope cross-checked against esp-dsp `dsps_add_s16_aes3.S`) and structurally cannot hit the over-saturation trap because it never forms the all-inclusive sum. The **large/variable-N int32 parity path** needs ops not yet confirmed to assemble — widen 8×int16→4×int32, `ee.vadds.s32`/`ee.vsubs.s32`, saturating narrow int32→int16 (or the QACC `ee.vmulas.s16.qacc` multiply-by-ones accumulate trick, ~16-leg headroom in 20-bit segments). **Verify against the S3 TRM extended-instruction chapter first; keep the scalar `mix_accumulate`/`mix_minus_self` bodies as the default until then.** Vectorize last — at ≤8 narrowband legs the scalar mix is ~64k adds/s, a rounding error on a 240 MHz core. Depends on #164. (§6 of the design doc.)

#### 🟢 Issue #169: Media: per-port repacketization layer for non-20 ms ptime codecs before the bus
* **Status**: ⏳ Open
* **Labels**: `media`, `priority-medium`
* **Description**: The bus `FRAME` must equal the RTP ptime. The moment a non-20 ms-ptime codec appears (G.723.1 = 30 ms, G.729 = 10 ms, Opus 2.5–60 ms) each such port needs repacketization to the bus frame size **before** it reaches the bus. This is a correctness/jitter problem, not a compute one — noted but not solved in the bundle. Depends on #164. (§8 trap #5 of the design doc.)

#### 🔵 Issue #170: Media: VAD energy-gating of mix participation (conference-grade quiet)
* **Status**: ⏳ Open
* **Labels**: `media`, `feature-request`, `priority-low`
* **Description**: To make the bus *sound* like a conference rather than merely be correct, gate the mix to ports actually speaking — drops the noise floor and clip risk together. The per-frame energy test is **sum-of-squares**, a clean vector MAC (`ee.vmulas.s16.accx`), the same kernel shape as the Goertzel/correlation DSP. Naive clip is fine for one or two simultaneous talkers; add this when scaling participant counts. Depends on #164. (§6c of the design doc.)

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

#### 🔵 Issue #161: SIP P2: RFC 3262 100rel / PRACK — interop gap with strict RFC platforms (Metaswitch)
* **Status**: ⏳ Open
* **Labels**: `sip-engine`, `priority-medium`
* **Description**: DRAWBRIDGE does not implement reliable provisional responses (100rel/PRACK). Most phones and providers operate fine without it, but Metaswitch/Perimeta and some carrier-class platforms send `Require: 100rel` and may reject calls with `420 Bad Extension`. Fix: parse `Require: 100rel`, send `183`/`180` with `RSeq`, await PRACK before proceeding. Bounded state (`pendingPrack`, `rseq` counter) on `Session`. See also: #151.

#### 🔵 Issue #162: Code quality P2: de-duplicate TUI providers/setters between wolfSSH and littlessh backends
* **Status**: ⏳ Open
* **Labels**: `tui`, `code-quality`, `priority-medium`
* **Description**: ~300 lines of TUI provider/setter glue are copy-pasted verbatim between `SshServer.cpp` (wolfSSH) and `SshServerLittlessh.cpp`. Extract to a shared `SshServerTuiProviders.cpp` / `TuiProviders.h`. Also: port wolfSSH resize-debounce to littlessh `ll_on_pty`; remove dead pubkey-auth path (~130 lines, no caller); collapse 10 copy-pasted `wtr_t`+send tails into one helper.

#### 🔵 Issue #163: Platform P2: CDR persistence across reboots — NVS-backed ring buffer
* **Status**: ⏳ Open
* **Labels**: `api`, `diagnostics`, `priority-medium`
* **Description**: CDR data is lost on every reboot (OTA, watchdog, power event). Fix: write each CDR entry to NVS on call completion; pre-populate the in-memory buffer from NVS on boot. NVS write endurance (~100k cycles/key) is sufficient for typical office call volumes. Admin-gated `POST /api/cdr/clear` wipes both NVS and in-memory ring. Pairs with NVS schema versioning (#133).

> **Note: #84 closed / consolidated into #127.** The "last-boot reset reason in `/api/status`" (#84) is a subset of the task watchdog + heap/stack high-water-mark monitoring issue (#127). All deliverables from #84 are covered by #127.

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

### 🟢 Issue #130: Security: per-source-IP brute-force lockout on /api/admin/login
* **Status**: ✅ Resolved (2026-07-22) — 292/292 host gtest suite (8 new tests in `AdminAuthLockout_test.cpp`), hardware-confirmed on COM5 (5 wrong PINs → `401`×4 then `429`, matching `kMaxFailedAttempts`)
* **Labels**: `security`, `priority-high`
* **Description**: The prior global 5-attempt/60-second lockout on the `Http` channel let an attacker on the LAN self-DoS the legitimate admin (D-3 in THREAT_MODEL.md) — one flood of wrong PINs from any IP locked out every other IP for 60s, repeatable indefinitely. **Fix**: added a bounded (`kMaxIpLockoutEntries` = 64) `std::unordered_map<uint32_t, IpLockout>` in `AdminAuth.cpp`, keyed by source IP, evicting the least-recently-seen entry on overflow (same discipline as `RequestsHandler::_rateBuckets`). `AdminAuth::verifyPin()`/`isLockedOut()` gained a defaulted `sourceIp` parameter: passing a non-zero IP on the `Http` channel routes lockout accounting through the per-IP table instead of the legacy channel-wide counter; every other existing caller (SSH, DTMF, or any test that doesn't pass an IP) is source-compatible and unaffected. `HttpServer::sendApiAdminLogin()` now resolves the caller's peer IP via a new `getPeerIpU32()` helper (`getpeername()`) and threads it through. Setting/clearing the admin credential also clears the per-IP table, matching the existing per-channel reset behavior.

### 🟢 Issue #178: Blind transfer never notified the transferred-away party (silent drop, no BYE)
* **Status**: ✅ Resolved (`5eeb2af`, 2026-07-22) — confirmed via `.smoke/office_smoke.py` on WSL host build (10/10 across 3 runs) and hardware-validated on COM5
* **Labels**: `sip`, `transfer`, `bug`
* **Severity**: Medium
* **Description**: Found 2026-07-22 by `.smoke/office_smoke.py`'s blind-transfer scenario: `onRefer()`'s blind-transfer branch calls `endCall()` on the transferor's original dialog and then redirects the transferor to the Refer-To target — but `endCall()` is purely internal bookkeeping (session/CDR/park/DTMF-state cleanup), it never sends anything on the wire. Every OTHER teardown path in `RequestsHandler.cpp` (the transfer-bridge peer relay, the park peer relay, the ordinary two-party BYE relay) explicitly `buildServerBye()`s the far side before/while tearing down; this path alone skipped that step. Consequence: the party the transferor was talking to before the transfer never received a BYE — their phone would keep showing an active call (media just goes quiet) until the user manually hangs up, with no protocol signal a transfer had happened. **Fix**: resolve "the other party" from the session (`getSrc()`/`getDest()`, whichever isn't the transferor — handles either transfer direction) and send a proper in-dialog BYE using the dialog's captured From/To (`getDialogFrom()`/`getDialogTo()`, populated from the original 200 OK) before `endCall()` erases the session.

### 🟢 Issue #171: DTMF CLASS codes bypass `setDnd()`/`setForward()`, dashboard goes stale
* **Status**: ✅ Resolved (`5eeb2af`, 2026-07-22) — confirmed via `.smoke/office_smoke.py` on WSL host build (10/10 across 3 runs) and hardware-validated on COM5
* **Labels**: `concurrency`, `dashboard`, `tech-debt`
* **Description**: `onDtmfInfo` (`RequestsHandler.cpp:5959`) runs inside `handle()`'s `_mutex` lock (`RequestsHandler.cpp:206`, non-recursive). `setDnd()`/`setForward()` (`RequestsHandler.cpp:3530`/`3611`) each independently take that same `_mutex`, so `onDtmfInfo` cannot call them without deadlocking — and doesn't. Instead, `*60`/`*80` (`RequestsHandler.cpp:6157`/`6167`) write `_dnd` directly, and `*73`/`*72NNNN` (`RequestsHandler.cpp:6176`/`6256`) write `_forwards` directly, bypassing the setters entirely (there's an acknowledging comment at `:6149`, "We're already inside `_mutex`"). Consequence: `_snapshot.dnd`/`_snapshot.forwards` (what the HTTP dashboard's status endpoint actually reads, `HttpServer.cpp:824-825`) are refreshed *only* inside those setters — so a DTMF-triggered DND/forward change never appears on the dashboard until an unrelated HTTP-side call happens to touch the same extension. Found during the [admin-plane HTTP-only plan](docs/PLAN_ADMIN_HTTP_ONLY.md)'s Phase 1 audit (2026-07-15); tracked here rather than folded into that plan since the real fix needed a small refactor, not a one-line patch. **Fix**: factored the inline snapshot-rebuild block out of `setDnd()`/`setForward()` into `refreshDndSnapshotLocked()`/`refreshForwardsSnapshotLocked()` — takes only `_snapshotMutex`, safe to call from anywhere already holding `_mutex` — and wired both into all four DTMF star-code handlers (`*60`/`*80`/`*72NNNN`/`*73`). **Not fixed as part of this**: `*72NNNN`'s inline path still skips `setForward()`'s virtual-extension guard (rejecting `777`/`999` as forward targets), and the `*PIN#101`/`*PIN#999` raw-NVS-write divergence flagged in the same audit — both still open, worth a follow-up issue if not already tracked elsewhere.

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
