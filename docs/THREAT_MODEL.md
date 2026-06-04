# Threat Model: pocket-dial ESP32 SIP PBX

**Date**: 2026-06-04 | **Version**: 1.0 | **Author**: Security Engineering | **Phase**: 1 (production hardening)

This document is a STRIDE-structured threat model for the **pocket-dial** ESP32 SIP PBX
and its HTTP dashboard. It focuses on the locally-reachable attack surface of a small
appliance that, by default, **runs its own open WiFi access point**. It complements the
broader `docs/SECURITY_AUDIT.md` (which tracks CVSS-scored findings) and records the
authentication change introduced in this phase: the dashboard's state-changing endpoints
are now gated by an admin PIN + server-side session, closing the previously
**unauthenticated admin** hole (audit finding SEC-04).

---

## 1. System Overview

- **Device**: ESP32 / ESP32-S3 microcontroller, single firmware image.
- **Role**: Self-contained SIP PBX — registers SIP extensions, bridges calls, carries
  RTP audio (G.711 / PCMU/PCMA).
- **Transports** (build-time `SIP_TRANSPORT`):
  - `wifi` — device hosts a **SoftAP** (its own WiFi network) and/or joins a station network.
  - `eth` — W5500 Ethernet (PoE); the device is a node on a wired LAN.
  - `display` — SoftAP + local touchscreen UI (LVGL) with captive-portal onboarding.
- **Listening services**:
  | Service | Port | Proto | Purpose |
  |---------|------|-------|---------|
  | HTTP dashboard | 80 (device) / 8080 (host) | TCP | Status, call control, WiFi provisioning, **admin auth** |
  | SIP | 5060 | UDP | Registration, INVITE, OPTIONS, session control |
  | RTP | dynamic | UDP | Media (G.711) |
  | DNS (captive portal) | 53 | UDP | Resolves all names to the device IP during onboarding |
- **Persistence**: ESP-IDF **NVS** flash, namespace `"storage"`. Stores WiFi mode/SSID/
  password, a captive-portal `decayed` flag, and (new this phase) the admin credential
  (`admin_salt`, `admin_hash`).
- **Crypto available**: mbedTLS on-device (SHA-256, `esp_random()` hardware CSPRNG). The
  admin module ships its own self-contained SHA-256 so the credential format is identical
  on device and on the host/CI simulator.
- **Data classifications handled**: call control state, WiFi credentials (a secret),
  device-integrity/config, admin credential, real-time voice media.

### Host vs. device note
The desktop/CI build (`SipServer` binary) is a **developer/test simulator**, not a
production deployment. On host there is no NVS, so the admin credential and sessions live
in process memory for a single run (documented in `AdminAuth.hpp`). The host build's RNG
uses `std::random_device`-seeded `std::mt19937_64`; the device build uses the hardware
CSPRNG `esp_random()`. The trust boundaries below describe the **device**.

---

## 2. Trust Boundaries

```
            (most hostile)                                     (least hostile)
  ┌──────────────────────────┐   ┌───────────────────┐   ┌────────────────────┐
  │  OPEN SoftAP RF link      │   │  Wired/station LAN │   │  Device internals   │
  │  WIFI_AUTH_OPEN — anyone  │──▶│  (eth transport,   │──▶│  firmware, NVS,     │
  │  in radio range can join, │   │   or joined STA    │   │  SIP engine, RAM    │
  │  no link-layer encryption │   │   network)         │   │                     │
  └──────────────────────────┘   └───────────────────┘   └────────────────────┘
            │                              │                        │
            ▼                              ▼                        ▼
   HTTP / SIP / RTP / DNS         HTTP / SIP / RTP            local-only state
```

| # | Boundary | From → To | Current controls |
|---|----------|-----------|------------------|
| TB-1 | **Open SoftAP link (dominant boundary)** | Any RF-range device → device services | **NONE at link layer** — `WIFI_AUTH_OPEN`. App-layer: same-origin/CSRF check + (new) admin PIN/session on mutating HTTP endpoints. |
| TB-2 | LAN (eth / joined STA) | LAN host → device services | Network is as trusted as the LAN's own segmentation. Same app-layer controls as TB-1. |
| TB-3 | Browser → HTTP server | Dashboard user → endpoints | `isSameOrigin()` (Origin vs. Host), HttpOnly + `SameSite=Strict` session cookie, no wildcard CORS, request-body cap (16 KB), per-socket recv timeout. |
| TB-4 | HTTP/SIP → NVS | Handlers → flash | Writes gated behind same-origin + auth (HTTP); salted/iterated admin hash; explicit `confirm=ERASE` for factory reset. |
| TB-5 | Physical | Holder of the device → flash/JTAG | **NONE by default** — no flash encryption, no Secure Boot (see roadmap). |

> **The open SoftAP (TB-1) is the dominant boundary and the root of most residual risk.**
> Anyone within WiFi range can associate with no credential and is then a peer on the
> device's IP network with full reachability to HTTP, SIP, RTP and DNS. Every threat below
> should be read with "the attacker is an associated, unauthenticated AP client" as the
> baseline. The device is **not WAN/Internet-exposed by default** (no port forwarding, no
> cloud component); the realistic attacker is local/proximate, not remote.

---

## 3. Assets

| Asset | Why it matters | Primary threats |
|-------|----------------|-----------------|
| **Call control** (force-disconnect, active sessions) | Denial of phone service; disrupting live calls | Spoofing, Tampering, DoS, EoP |
| **WiFi credentials** (station SSID/password in NVS) | Reusable secret; pivot to the upstream network | Info disclosure, Tampering |
| **Device integrity / config** (mode, factory reset, OTA image) | Bricking, persistent control, supply-chain implant | Tampering, EoP, DoS |
| **Admin access** (PIN, session token) | Master key to all mutating actions | Spoofing, brute force, token theft, EoP |
| **Voice media + signaling** (RTP G.711, SIP) | Call confidentiality and privacy | Info disclosure (eavesdrop), Repudiation |

---

## 4. STRIDE Threat Enumeration

Each row: threat → current mitigation → **residual risk**.

### Spoofing
| ID | Threat | Mitigation | Residual risk |
|----|--------|-----------|---------------|
| S-1 | **Unauthenticated admin actions** — any AP peer POSTs `/api/kill`, `/api/wifi/connect`, `/api/wifi/mode_ap`, `/api/factory-reset`. *(was SEC-04)* | **FIXED this phase.** Once an admin PIN is provisioned, all four mutating endpoints require a valid `pd_session` cookie (else `401`). Login issues a server-side session token. | Before first provisioning the device is intentionally open (onboarding) — see *First-run gap* below. PIN strength is user-chosen. |
| S-2 | Session-cookie forgery / guessing | Token is ≥128-bit (`esp_random()` on device), opaque, validated server-side via constant-time compare; not derived from any user input. | Brute-forcing a 128-bit token over the network is infeasible; theft (S/T-3) is the realistic path. |
| S-3 | SIP identity spoofing (register/INVITE as another extension) | SIP signaling input is validated/bounded (audit SEC-02 mitigated). | **No SIP digest authentication** — on the open AP an attacker can register/INVITE as any extension. Tracked as a SIP-layer gap; out of scope for the HTTP auth change. |

### Tampering
| ID | Threat | Mitigation | Residual risk |
|----|--------|-----------|---------------|
| T-1 | Rewriting WiFi credentials / operating mode via dashboard | Same-origin + admin session gate (post-provisioning). | First-run gap; physical attacker (T-4). |
| T-2 | **CSRF** — a malicious page on the AP makes the victim's browser fire side-effecting POSTs | `isSameOrigin()` rejects requests whose `Origin` host ≠ `Host`; session cookie is `SameSite=Strict`; no wildcard `Access-Control-Allow-Origin`. | A request with **no** `Origin` (curl, native app) is allowed by design — but on a provisioned device it still needs the session cookie, which a cross-site page cannot read (HttpOnly) or send (SameSite=Strict). |
| T-3 | Request-body / parser abuse (oversized body, split TCP segments) | 16 KB body cap (`413`), `Content-Length` parsing with overflow guard, per-client `SO_RCVTIMEO`. | Low. |
| T-4 | **Physical flash tamper** — rewrite NVS / reflash | None by default. | **High if device is physically obtained**: NVS (incl. WiFi password and admin hash) is readable/writable. Mitigation is Secure Boot v2 + flash encryption (roadmap P2). |
| T-5 | **Firmware / OTA tampering** | OTA is being added by a parallel workstream. | Unsigned OTA = remote persistent compromise. **Durable fix: signed images + Secure Boot v2 + flash encryption** (roadmap). Until then, OTA must be admin-gated and ideally restricted to the local link. |

### Repudiation
| ID | Threat | Mitigation | Residual risk |
|----|--------|-----------|---------------|
| R-1 | Admin/call actions are not attributable | Console logging exists; no tamper-evident audit trail; no per-user identity (single shared PIN). | Medium. Acceptable for a single-admin appliance; note that a single shared PIN cannot distinguish operators. |

### Information Disclosure
| ID | Threat | Mitigation | Residual risk |
|----|--------|-----------|---------------|
| I-1 | **RTP voice eavesdropping on the open AP** | None at app layer (G.711 is uncompressed, unencrypted). | **High on open SoftAP**: any peer can sniff/replay call audio. The single highest-leverage fix is **WPA2 on the SoftAP** (encrypts the whole link). SRTP would be the app-layer fix but is heavyweight on the MCU. |
| I-2 | **SIP signaling disclosure** (who calls whom, extensions, topology) | None (cleartext SIP). | **High on open SoftAP**; same fix as I-1 (WPA2 link encryption). SIP-over-TLS is the app-layer option but costly on-device. |
| I-3 | WiFi station password recoverable | Stored cleartext in NVS (audit SEC-03). Not exposed over HTTP. | Recoverable by a **physical** attacker (flash read). Fix = flash encryption (P2). |
| I-4 | Verbose error / stack leakage | Endpoints return generic JSON errors; no stack traces or internal paths. | Low. |
| I-5 | Admin hash disclosure | Stored as **salted, iterated SHA-256** (50k rounds, 128-bit random salt), never returned over HTTP. | Offline cracking requires a **physical NVS read**; the salt defeats rainbow tables and the iteration count slows guessing. Still, a short/numeric PIN is brute-forceable offline once flash is read — see *PIN strength*. |

### Denial of Service
| ID | Threat | Mitigation | Residual risk |
|----|--------|-----------|---------------|
| D-1 | Connection/slowloris exhaustion on HTTP | Detached per-connection threads, `SO_RCVTIMEO` (5 s), body cap. | A flood from the open AP can still pressure a constrained MCU. |
| D-2 | Malicious call teardown (`/api/kill` abuse) | Now admin-gated post-provisioning. | First-run gap; SIP-layer teardown (BYE spoofing) still possible without SIP auth (S-3). |
| D-3 | PIN-guess lockout used as self-DoS | Lockout is global/in-process (not per-IP), so an attacker can lock out the legitimate admin for the cooldown (60 s) — **but the device stays usable** because pre-existing sessions remain valid and only `login` is throttled. | Accepted trade-off; **per-IP lockout would be stronger** (see *Brute force*). Cooldown auto-clears (no permanent lock). |
| D-4 | RF jamming / deauth of the SoftAP | None (inherent to WiFi). | Out of scope; physical/RF layer. |

### Elevation of Privilege
| ID | Threat | Mitigation | Residual risk |
|----|--------|-----------|---------------|
| E-1 | Anonymous AP peer → full admin control | **FIXED**: PIN/session gate on all mutating endpoints. | First-run gap; physical/OTA paths (T-4/T-5). |
| E-2 | Read endpoints leaking privileged actions | `/api/status`, `/api/wifi/scan`, `/api/admin/status` are read-only and intentionally unauthenticated (the dashboard needs them to render). `/api/admin/status` returns only booleans (`provisioned`, `authenticated`) — no secrets. | Low. SSID list / status are observable by any AP peer (already visible on an open AP anyway). |

---

## 5. Detailed Notes on Key Threats

### 5.1 The first-run / onboarding gap (by design)
A factory-fresh device has **no admin PIN**. To preserve captive-portal onboarding (and the
existing CI smoke tests, which never set a PIN), the mutating endpoints behave exactly as
before **while unprovisioned**: same-origin gate only, no auth. The instant a PIN is set
(`POST /api/admin/set-pin`), the `401` gate engages. **Operational guidance: set the admin
PIN as the first onboarding step.** A future hardening could auto-provision a random PIN
shown on the device's screen/serial on first boot to eliminate even this window.

### 5.2 PIN brute force
- **Online**: `verifyPin` counts consecutive failures; after **5** it engages a **60 s**
  lockout during which even a correct PIN is refused (`429`). The counter resets after the
  cooldown (a bad streak does not permanently brick login) and on any successful login.
- **Offline**: a leaked hash (only obtainable via **physical NVS read**) is a salted,
  iterated SHA-256 (50,000 rounds, per-credential 128-bit salt). This defeats precomputation
  and slows guessing, but a 4-digit numeric PIN is only 10⁴ candidates — trivially crackable
  offline. **PIN strength is the user's responsibility**; recommend ≥6 alphanumeric chars,
  and note that the real backstop for offline attack is flash encryption (P2).
- **Limitation**: the lockout is a single in-process counter, **not per-IP**. This is simple
  and safe (auto-clearing) but means (a) one attacker IP can briefly lock the admin out of
  *new logins* (D-3), and (b) a botnet of AP peers shares one global budget. **Per-IP (or
  per-association) tracking would be stronger** and is recommended if the threat model later
  includes many simultaneous local clients.

### 5.3 Session token theft & replay
Tokens are 128-bit, server-side, with a **30-minute absolute expiry** and a fixed-capacity
table (8 slots; oldest/expired evicted). Cookie flags: `HttpOnly` (no JS access → blunts
XSS exfiltration) and `SameSite=Strict` (browser won't attach it cross-site → blunts CSRF).
**No `Secure` flag and no TLS**: on plain HTTP over the open AP, a network sniffer can
capture the cookie in transit and **replay** it until expiry. This is the same root cause as
I-1/I-2 and has the same headline fix — **WPA2 on the SoftAP encrypts the cookie in flight**.
Residual: a token has no rotation/binding to client IP; theft within the 30-min window is
usable. Logout (`/api/admin/logout`) and factory reset both destroy sessions immediately.

### 5.4 Captive-portal / DNS-spoof phishing
In onboarding mode the device answers **all** DNS queries with its own IP (port 53) to
trigger the OS captive-portal prompt. On an open AP an attacker could stand up a competing
portal/AP to phish the WiFi password or the admin PIN. The same-origin check prevents a
foreign page from driving the *real* device's API, but it cannot stop a user from typing
secrets into a look-alike. Mitigation is again **WPA2** (raises the bar to join/impersonate)
plus user guidance to provision over a trusted link.

### 5.5 Cleartext SIP + RTP on the open AP
This is the largest *confidentiality* gap and is **independent of the dashboard auth fix**.
The admin PIN protects *control*, not *media*. SIP signaling and G.711 RTP traverse the open
link in cleartext, so any associated peer can record calls and map who-calls-whom. App-layer
fixes (SIP-over-TLS, SRTP) are expensive on a constrained MCU and add key-management UX. The
**link-layer fix (WPA2 on the SoftAP) encrypts everything — dashboard, SIP, and RTP — at
once**, which is why it is the top recommendation below.

---

## 6. The TLS Question (HTTPS for the dashboard) — answered honestly

**Should the dashboard serve HTTPS (self-signed) on the ESP32?**

**Recommendation: not as the primary control. Ship HTTP + a mandatory admin PIN now, and
enable WPA2 on the SoftAP as the single highest-leverage hardening.** Self-signed HTTPS is
listed as a *documented optional* future enhancement, not the headline fix.

**Why self-signed HTTPS on this device has real downsides:**
1. **Browser trust UX is bad on a LAN appliance.** There is no public CA for `192.168.4.1`
   / `pocketdial.local`, so every visit throws a full-page certificate warning. On a
   no-screen appliance users are trained to "click through" warnings — which *erodes* the
   very trust signal TLS is supposed to provide and habituates users to ignore real warnings.
2. **Constrained-MCU cost.** TLS handshakes (RSA/ECC + the record layer) cost notable RAM and
   CPU on an ESP32 whose HTTP path already runs on a small per-connection thread stack.
   Concurrent TLS sessions can pressure heap and slow the dashboard.
3. **Cert provisioning UX.** A self-signed cert must be generated/stored per device (or a
   shared key baked into firmware — which is worse, a single compromise breaks every unit)
   and rotated; there is no clean trust-on-first-use story for a headless box.
4. **It only protects the dashboard.** HTTPS does nothing for SIP or RTP. The confidentiality
   gaps I-1/I-2 (call audio + signaling eavesdropping) would remain wide open.

**Why WPA2 on the SoftAP is the better lever:**
- It **encrypts the entire RF link** — dashboard HTTP, SIP signaling, *and* RTP media — with
  one change, directly closing I-1, I-2, and the cookie-replay vector in 5.3.
- It **gates association**: an attacker must know the AP passphrase to join at all, which
  removes the "anyone in range is a peer" baseline that powers nearly every threat here.
- It needs **no per-device certificates** and no browser-warning UX; the passphrase can be
  shown on the device screen / printed on a label.
- Trade-off: a WPA2 passphrase must be distributed to legitimate clients, and WPA2-PSK is
  still vulnerable to offline handshake cracking if the passphrase is weak — so pair it with
  a non-trivial passphrase. It is nonetheless a large net improvement over an open AP.

**Net**: HTTP + mandatory PIN (this phase) + WPA2 SoftAP (recommended P0) gives confidentiality
*and* control protection for the whole link. Optionally layer self-signed HTTPS later for the
dashboard if a specific deployment requires app-layer transport security on top of WPA2 — but
do it eyes-open about the warning UX and MCU cost.

---

## 7. Prioritized Hardening Roadmap

### P0 — do now / next (highest leverage, low-to-moderate effort)
- **Enable WPA2 on the SoftAP** (`WIFI_AUTH_WPA2_PSK` instead of `WIFI_AUTH_OPEN`), with a
  non-trivial per-device passphrase surfaced on-screen/label. *Closes the dominant boundary;
  encrypts dashboard + SIP + RTP; the top recommendation.*
- **Mandatory admin PIN — DONE this phase** (PIN + server-side session on all mutating HTTP
  endpoints; lockout; factory reset clears the credential). Make setting the PIN the first
  onboarding step in the UI/docs.
- **Guidance/UX**: require a PIN of ≥6 alphanumeric chars; warn against 4-digit numeric PINs.

### P1 — soon (meaningful, moderate effort)
- **SIP authentication** (digest auth on REGISTER/INVITE) to stop extension spoofing and
  BYE-based teardown (S-3, D-2) even on a trusted link.
- **Per-IP / per-association brute-force tracking** for `login` (replaces the global counter;
  removes the admin-lockout self-DoS in D-3).
- **Gate OTA behind the admin session** and restrict it to the local link until image
  signing lands.
- **Session hardening**: optional idle timeout in addition to absolute expiry; consider
  binding a token to the client association.

### P2 — durable platform hardening (higher effort, strongest guarantees)
- **Secure Boot v2 + flash encryption**: signs the firmware (defeats OTA/boot tampering,
  T-5/T-1) and encrypts NVS at rest (defeats physical recovery of the WiFi password and admin
  hash, I-3/I-5/T-4). This is the durable fix for the physical and supply-chain boundaries.
- **Signed OTA images** verified against the Secure Boot key.
- **Tamper-evident audit logging** for admin/call actions (addresses R-1) if multi-operator
  attribution becomes a requirement.
- **Optional self-signed HTTPS** for the dashboard, as a documented add-on on top of WPA2
  (see §6), for deployments that mandate app-layer transport security.

---

## 8. Summary

The change shipped in this phase removes the **unauthenticated-admin** elevation-of-privilege
hole (S-1 / E-1 / audit SEC-04): once provisioned, call control and WiFi/mode/factory-reset
require an admin PIN and a server-side session, layered on top of the existing same-origin
defense, while preserving open first-run onboarding and the existing CI smoke tests. The
**dominant residual risk is the open SoftAP** (TB-1), which leaves call media and signaling
eavesdroppable (I-1/I-2) and the session cookie replayable (5.3). The single highest-leverage
next step is **WPA2 on the SoftAP**, which encrypts the whole link in one move; physical and
firmware-supply-chain risks are durably addressed by **Secure Boot v2 + flash encryption**.
