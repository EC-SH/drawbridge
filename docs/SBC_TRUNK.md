# Edge Gateway / Upstream SIP Trunk (B2BUA) — Architecture Design

**Status:** Direction under active exploration (not yet built). | **Scope:** Engineering design. | **Last updated:** 2026-06-09

This document is the focused design for pocket-dial's planned **edge-gateway / upstream-trunk**
direction. It expands [FEATURE_ROADMAP.md §7](FEATURE_ROADMAP.md) into something an engineer can
build against, and it is honest about which long-standing invariants this track *consciously bends*.

It is deliberately **vendor-neutral**: it talks about *capabilities* — "a commercial softswitch /
CPaaS fabric", "a SIP trunk", "a call-control API", "the PSTN" — never a specific product. This is a
public repo doc.

Cross-references:
[ARCHITECTURE.md](ARCHITECTURE.md) ·
[SCALING.md](SCALING.md) ·
[THREAT_MODEL.md](THREAT_MODEL.md) ·
[RTP.md](RTP.md) ·
[FEATURE_ROADMAP.md](FEATURE_ROADMAP.md) ·
[PROVISIONING.md](PROVISIONING.md)

> **One-line framing.** Today pocket-dial is a *signalling-only, zero-DSP* LAN registrar: phones
> stream G.711 **peer-to-peer** and the box never sees an RTP packet ([SCALING.md §1](SCALING.md)).
> Trunking *up* to one provider changes exactly one thing — the two call legs live in different RTP
> domains, so for **trunk calls only** the box must bridge media. Everything else (signalling,
> registrar, dial plan, NAT) is a reuse of machinery that already exists.

---

## 1. Role & topology — what this is, and is not

pocket-dial becomes a **back-to-back user agent (B2BUA)** acting as a small **session border
controller (SBC) / edge gateway**: it terminates one SIP dialog toward the LAN and originates a
separate, independent SIP dialog toward a single upstream provider, gluing the two together.

```
   LAN side (trusted)                    │  WAN side (one upstream, outbound-trust)
                                         │
  ┌────────┐   REGISTER/INVITE   ┌───────┴────────┐   trunk REGISTER/INVITE   ┌──────────────┐   ┌──────┐
  │ phones │◄──────G.711 P2P────►│   pocket-dial   │◄────G.711 (TLS/SRTP?)────►│  upstream     │──►│ PSTN │
  │ (ext)  │   (LAN↔LAN media)   │  B2BUA / mini-  │    1 outbound dialog       │  softswitch / │   │      │
  └────────┘                     │      SBC        │    per external call       │  CPaaS fabric │   └──────┘
                                 └───────┬─────────┘                            └──────────────┘
                                         │  ▲
                          B2BUA media    │  │  media is bridged HERE (and only here):
                          bridge (G.711) ┘  └  trunk leg RTP ◄──relay──► extension leg RTP
```

**It IS:**

- A B2BUA: two legs, two Call-IDs, two dialogs; pocket-dial is the endpoint of each, not a
  transparent proxy. (Internally the engine is already "B2B-light" — it re-points request lines and
  rewrites `Via`/`Contact` on forks; see `RequestsHandler::buildInviteFork` / `redirectInvite`.)
- A **demarcation point** between the trusted LAN and exactly **one** upstream provider.
- A NAT/topology hider: the upstream sees only pocket-dial; the LAN extensions are never exposed.

**It is NOT:**

- A general-purpose SBC facing the public SIP-scanning internet. The trust posture is **outbound
  only** — the box trusts one upstream it dialed out to, not arbitrary inbound callers (see §5).
- A transcoder, mixer, conference bridge, or media recorder. It relays **G.711 only**, end to end,
  with no codec conversion ([FEATURE_ROADMAP.md §5 non-goals](FEATURE_ROADMAP.md)).
- A multi-trunk carrier-grade gateway. One upstream, sized for a small office.

---

## 2. The media model — the conscious exception

This is the one place the trunk track crosses the project's defining line. Read it carefully.

### 2.1 When the P2P invariant holds vs is bridged

| Call shape | Media path | Server in media path? | Cost |
|---|---|---|---|
| **LAN ↔ LAN** (ext dials ext, 999 page, etc.) | Direct phone-to-phone RTP after SDP O/A | **No** — unchanged | ~200 B `Session` bookkeeping ([SCALING.md §2](SCALING.md)) |
| **Trunk ↔ LAN** (inbound from / outbound to PSTN) | RTP relayed through pocket-dial | **Yes** — for that call only | One bidirectional G.711 relay (see §2.3) |

The reason the bridge is *unavoidable* for trunk calls, and not for LAN calls: in a LAN↔LAN call the
two phones share one routable RTP domain, so the box just forwards each side's SDP (`c=`/`m=`) and
steps out. A trunk call has an **extension on the private LAN** and a **carrier on the WAN** — they
cannot send RTP to each other's advertised address (private IP on one side; NAT, possibly TLS/SRTP on
the other). The B2BUA must terminate both SDP offers, advertise *its own* `c=`/`m=` to each side, and
shuttle packets between them. This is a bounded, documented exception to "the server never touches
RTP" — **not** a removal of the invariant for the registrar/LAN path.

### 2.2 What exists today vs what's new

The first server-side RTP code already landed: the **media beachhead** (`src/SIP/RtpSender.*`), the
one-way G.711 µ-law tone source behind virtual extension **440**. It proves out the pieces the bridge
reuses:

- A real UDP media socket on a dedicated port (5062), bound deterministically.
- A 20 ms-paced FreeRTOS sender task, pinned to **Core 0** at **priority 6** (one above SIP/UDP at 5)
  so pacing is never starved by signalling bursts and never steals from LVGL on Core 1.
- Zero hot-path heap: one fixed `PACKET_BYTES` buffer reused per frame.
- An ITU-T µ-law packetizer + RTP header builder (RFC 3550), all host-unit-tested.
- SDP-answer helpers `buildMediaSdp()` / `parseCallerRtp()` that advertise the server's own media
  and extract the caller's RTP `IP:port` from an INVITE.

> **Crucial gap to call out:** `RtpSender` is **send-only**. pocket-dial has **no RTP *receive*
> path** yet — no socket read loop, no jitter buffer. A B2BUA bridge needs **both directions**, so
> the new work is principally *the receive/relay half*, not the transmit half. The transmit pacing,
> socket, SDP-answer, and single-stream-cap discipline are reusable; the inbound read + forward is
> net-new. See [RTP.md](RTP.md) for the broader server-side-RTP exploration this builds on.

### 2.3 What a bridge leg actually does (per trunk call)

```
   extension leg                    pocket-dial B2BUA                   trunk leg
   (LAN, G.711)                                                     (WAN, G.711 [+SRTP])

   ext RTP ───────────────►  recv sock (ext side)  ──forward──►  send sock (trunk side) ───► upstream
                             [optional SRTP decrypt]   [optional SRTP encrypt]
   ext RTP ◄───────────────  send sock (ext side)  ◄──forward──  recv sock (trunk side) ◄─── upstream
```

No transcode (both sides are G.711, forced by `enforceG711()` / SDP rewrite). No mixing. No jitter
buffer for plain relay — packets are forwarded as they arrive, preserving the sender's timing.
Optional SRTP adds a per-packet AES-CTR + HMAC on the trunk side only (§5).

### 2.4 CPU / RAM budget — the honest constraint (it's NOT flash)

The flash budget is comfortable — the SSH-terminal build is ~1.96 MB of a 6 MB slot, leaving ~4 MB.
**The binding constraints are RAM, CPU, and — above all — Wi-Fi airtime**, exactly as quantified in
[RTP.md](RTP.md):

| Resource | Budget for the bridge | Notes |
|---|---|---|
| **Concurrent relayed calls** | ~**4–6 two-party calls (8–12 RTP streams)** on an S3 @ 240 MHz with media isolated on an otherwise-idle core; **~half that on a display build** (Core 1 is reserved for LVGL) | This is the *practical* trunk-call ceiling — far below `MAX_SESSIONS`. The session pool is no longer the limit; **media airtime is** |
| **Per-stream RAM** | RX socket + small ring/relay buffer + RTP state; on the order of a few KB per leg | Pre-allocate a bounded **relay pool** sized like the existing pools ([SCALING.md §1](SCALING.md)); never grow the heap per call |
| **CPU / deadline** | 20 ms send cadence must hold under SIP + HTTP + (display) LVGL load | Pin relay tasks to Core 0 alongside `RtpSender`; the half-duplex radio, not the CPU, is the first thing to saturate (see [RTP.md §0](RTP.md)) |
| **+ SRTP** | software AES-CTR + HMAC-SHA1 per packet, both directions | wolfSSL is already linked, so the crypto is in the binary; throughput, not code size, is the cost — **size SRTP-enabled ceilings lower than plaintext** |

> **Design rule.** Trunk media is the one place we accept a hard, low concurrency cap. Enforce it
> explicitly (refuse a new trunk-bridged call with `503`/busy when the relay pool is full), exactly
> as the 440 beachhead refuses a second stream with `486 Busy Here`. Never let media oversubscription
> silently degrade *every* call into glitching.

---

## 3. Registration / trunking to the upstream

Two ways to authenticate the box *to* the provider; both make pocket-dial a **UAC** (the originating
client), inverting its usual UAS/registrar role.

**(a) Registration trunk (dynamic IP).** The box sends an outbound `REGISTER` to the provider on an
interval, maintaining a binding so inbound PSTN calls can be delivered to it. This reuses the
registration-lease machinery conceptually (`parseRequestedExpires`, re-REGISTER on expiry) but in the
*opposite direction* — we are the registrant, not the registrar.

**(b) Static IP trunk.** No registration; the provider routes to a known IP. Simpler, but requires a
static address and provider-side IP ACLs. The box still authenticates per-request via digest on each
outbound INVITE.

### 3.1 Digest auth in the INVERSE role (the key reuse)

The digest primitives already exist (`src/Helpers/SipDigest.*`) and are being wired into the
registrar right now so the box, *as a UAS*, **challenges** phones (issues `401` + `WWW-Authenticate`,
verifies the phone's `Authorization` against a stored HA1 in `src/Helpers/SipSecretStore.*`).

For the trunk, the **same `SipDigest` math runs in reverse**: the upstream challenges *us* with `401`
(REGISTER) or `407` (INVITE), and the box — now the UAC — **computes** the digest response with the
trunk's own credentials and resends:

```
   pocket-dial (UAC)                         upstream softswitch/CPaaS (UAS)
        │  REGISTER / INVITE  (no creds)            │
        │ ─────────────────────────────────────────►│
        │            401 / 407  WWW-Authenticate     │   realm, nonce, qop=auth
        │ ◄─────────────────────────────────────────│
        │  recompute: HA1 = MD5(user:realm:secret)   │
        │             resp = MD5(HA1:nonce:nc:...:HA2)│   ← SipDigest::computeHa1/computeResponse
        │  REGISTER / INVITE + Authorization         │
        │ ─────────────────────────────────────────►│
        │            200 OK                           │
        │ ◄─────────────────────────────────────────│
```

> **Important storage distinction (vs the registrar role).** `SipSecretStore` deliberately persists
> only **HA1 = MD5(ext:realm:secret)** — enough to *verify* a phone, never to recover the secret, and
> tied to the box's own realm (`"pocketdial"`). The trunk role is different: the realm is the
> **provider's** realm (unknown until the `401`/`407` arrives), so the box must hold either the
> **plaintext trunk secret** or a per-provider-realm HA1 in order to *compute* a response. Treat
> trunk credentials as a **separate, sensitive config item** (§8), not a row in the phone secret
> store. This is the strongest argument for pairing the trunk with flash encryption ([THREAT_MODEL.md
> §7 P2](THREAT_MODEL.md)).

### 3.2 Routing toward the upstream

- **Outbound proxy / `Route`.** Many providers require requests be sent to a specific edge proxy
  regardless of the Request-URI domain. Add a configurable outbound proxy that becomes the
  pre-loaded `Route` (or simply the send target) for all trunk-leg requests.
- **`Contact` / `From` / `PAI`.** The trunk leg presents the box's trunk identity (the DID/number the
  provider assigned), not the LAN extension. Caller-ID toward the PSTN is set here.
- **OPTIONS keepalive.** The registrar already sends OPTIONS pings to keep LAN bindings warm
  (`buildOptionsPing`). The trunk leg reuses the *pattern* to keep the upstream binding and any NAT
  pinhole alive (§4) and to detect a dead trunk.

---

## 4. NAT for trunks — and why NOT ICE

A SIP trunk has to cross the office NAT to reach the provider. The correct tools are the classic
**far-end-NAT-traversal** mechanisms, all of which the engine either already does or can do cheaply:

| Mechanism | What it does | Status in tree |
|---|---|---|
| **`rport` (RFC 3581)** | Provider replies to the *source* IP:port it actually saw, not the (private) `Via` address | The engine already appends `;received=` on `Via`; `rport` is the symmetric add on the UAC side |
| **`Contact` / `Via` rewrite** | Advertise the box's public-reflexive address so in-dialog requests come back correctly | The fork path already rewrites `Via`/`Contact`; extend for the trunk leg |
| **Symmetric RTP / comedia latching** | Send media back to the `IP:port` packets actually *arrive* from, not the SDP-advertised one (which may be pre-NAT) | New on the relay's RX side; the 440 sender already binds a deterministic source port for symmetric-RTP-latching UAs |
| **Keepalive** | Periodic OPTIONS (signalling) + a small RTP/STUN-binding or CRLF (media) to hold the NAT pinhole open | OPTIONS pattern exists (`buildOptionsPing`) |

### Why ICE / STUN / TURN are the wrong tool here (out of scope)

ICE/STUN/TURN solve **peer-to-peer / WebRTC mesh** traversal between two endpoints that *both* sit
behind unknown NATs and must discover a working path by probing candidate pairs. **Trunking is not
that problem.** A SIP trunk is a single client (us) reaching one well-known, publicly-addressable
provider edge. That is solved by `rport` + symmetric RTP + keepalive — deterministic, ~no extra
state, no candidate gathering.

Specifically out of scope ([FEATURE_ROADMAP.md §5](FEATURE_ROADMAP.md) is explicit about this):

- **ICE candidate gathering / connectivity checks** — needless complexity for a one-upstream trunk.
- **A TURN *server* on the MCU** — would put *N relayed media streams* on the device on behalf of
  *other* peers, the exact media-load explosion the architecture forbids. (The trunk bridge already
  relays media, but only for the box's *own* trunk calls, capped per §2.4 — not as a relay service.)

So the gateway exploration **does not reintroduce** the WebRTC NAT machinery the project already ruled
out; it uses the lightweight SIP-trunk-native mechanisms instead.

---

## 5. Security

The trunk extends the trust model in [THREAT_MODEL.md](THREAT_MODEL.md) outward to the WAN. The good
news: the posture is **far safer than inbound public exposure**.

### 5.1 Outbound-trust posture (the headline)

The box **dials out to, and trusts, exactly one upstream**. It does **not** open an inbound public SIP
port to the internet. This sidesteps the dominant real-world VoIP threat — the constant background of
SIP scanners brute-forcing registrations and toll-fraud INVITEs against any box facing the public
internet. The attack surface added by the trunk is: (a) the one provider we trust, and (b) anything
that can spoof traffic *from* the provider's address. Harden (b) with source-IP pinning to the
provider's edge and, where supported, the transport security below.

### 5.2 Transport security toward the upstream

- **TLS / SIPS for signalling.** Encrypts the trunk-leg SIP (credentials in the `Authorization`
  digest are already not plaintext-recoverable, but the dialog, DIDs, and topology should not be in
  the clear over the WAN). **wolfSSL is already linked** for wolfSSH, so the TLS code is in the binary
  — the cost is RAM/CPU for the handshake + record layer, not flash. One long-lived trunk TLS
  connection is far cheaper than per-dashboard-connection TLS (the reason HTTPS-for-dashboard was
  deprioritized in [THREAT_MODEL.md §6](THREAT_MODEL.md) does **not** apply here: there is no
  browser-warning UX, and it's one connection, not many).
- **SRTP for media.** Encrypts the bridged G.711 on the trunk leg. Software AES-CTR + HMAC per packet,
  both directions — **this is the throughput cost that lowers the §2.4 concurrent-call ceiling**, so
  size SRTP-enabled tiers conservatively. SRTP applies to the *trunk* leg; the LAN leg's protection is
  the link layer (WPA2 on the SoftAP — [THREAT_MODEL.md §6](THREAT_MODEL.md)).

### 5.3 Credential storage at rest

Trunk credentials live in **NVS** (§8). As noted in §3.1 the trunk needs a *recoverable* secret (to
compute responses against the provider's realm), so unlike the admin PIN it cannot be stored one-way.
That makes it a high-value target for a **physical** flash read (threat **T-4** / **I-3** in
[THREAT_MODEL.md](THREAT_MODEL.md)). The durable fix is the already-planned **flash encryption +
Secure Boot v2** ([THREAT_MODEL.md §7 P2](THREAT_MODEL.md)); the trunk work should be sequenced to
*depend on* or at least *strongly recommend* enabling it, and the TUI should warn if trunk creds are
set while flash encryption is off.

> **Cross-ref:** the LAN-side boundary is unchanged — the open-SoftAP risks (I-1/I-2 eavesdropping)
> and their fix (WPA2) in [THREAT_MODEL.md](THREAT_MODEL.md) still apply to the *extension* leg. The
> trunk adds a WAN boundary, secured by TLS/SRTP + outbound-only trust.

---

## 6. Call routing / dial plan

The trunk plugs into the **existing** routing/forking machinery; almost nothing here is net-new logic,
only new *edges* in the routing graph.

### 6.1 Outbound (extension → PSTN)

```
  ext dials  9-1XXXXXXXXXX        (or any number not matching a local AOR / reserved code)
        │
        ▼
  onInvite() → dial-plan match: "not a local extension, not 777/999/star-code, matches an
        │      external/PSTN pattern" → route to the TRUNK leg instead of 404
        ▼
  build trunk-leg INVITE  (provider identity in From/Contact, digest on 407, B2BUA media bridge)
        ▼
  upstream → PSTN
```

Reuses `redirectInvite` / `buildInviteFork` to re-target the call; the only new step is "this target
is *the trunk*, so terminate the media and bridge it" rather than forwarding SDP untouched.

### 6.2 Inbound (PSTN → extension / ring group)

```
  upstream delivers INVITE for our DID  ──►  trunk leg INVITE arrives at pocket-dial
        │
        ▼
  DID → routing rule:  ring a single extension  OR  ring a ring/hunt group  OR  an IVR-less hunt
        │              (reuse startBroadcastFork / huntRingNext — the 999/ring-group engine)
        ▼
  fork to LAN extension(s); on answer, set up the B2BUA media bridge between the answering
  extension leg and the trunk leg; race-to-answer + CANCEL losers (existing paging logic)
```

The inbound path is essentially "a call from a new 'extension' called *the trunk*, whose destination
is resolved by a DID→target rule." Ring-all, hunt, call-forward (CFU/CFB/CFNA), and DND
([FEATURE_ROADMAP.md §1](FEATURE_ROADMAP.md)) all apply to inbound trunk calls for free, because they
key off the *target* extension, not the call's origin.

### 6.3 Interaction with reserved codes

- **777 (echo), 999 (all-page), 440 (tone), star-codes (`*60/*72/...`)** stay **local-only** — they
  must be matched *before* the "looks external → trunk" rule, so an extension can never accidentally
  dial them out the PSTN. The dial-plan precedence is: reserved/virtual codes → local AOR → trunk
  pattern → reject.
- **Emergency-number handling** must be explicit in the dial plan (always route to the trunk,
  never to a local code), and is a deployment-policy item to document per install.

---

## 7. Optional: upstream call-control / programmatic API

Orthogonal to the raw SIP trunk, the provider may expose an HTTP(S) **call-control API** (REST/webhook
style). This is a *separate layer* — it drives call setup/routing through the provider's platform
rather than (or alongside) raw SIP signalling:

- **Click-to-dial**: the dashboard/TUI triggers an outbound call via an HTTP request to the API
  instead of (or in addition to) an INVITE.
- **Programmable routing / presence**: the API informs inbound routing decisions or surfaces line
  state.

Implementation note: this is **HTTP-client** work (the box already has an HTTP *server* and wolfSSL
for TLS). It is the lowest-priority item (P2) and deliberately decoupled — the SIP trunk stands on its
own without it. Keep API credentials in the same protected NVS config surface as the trunk secret
(§8), and keep the call-control layer behind an admin gate.

---

## 8. Data model & config surface

### 8.1 Trunk config (new NVS namespace, e.g. `"trunk"`)

| Key | Meaning | Sensitivity |
|---|---|---|
| `enabled` | Trunk on/off | low |
| `mode` | `register` (dynamic) \| `static` (IP trunk) | low |
| `server` / `port` | Provider edge host:port | low |
| `outbound_proxy` | Optional `Route`/send target | low |
| `did` | The number/identity the provider assigned (From/Contact, caller-ID) | low |
| `username` | Trunk auth user | medium |
| `secret` | Trunk auth secret (**recoverable** — see §3.1/§5.3) | **high (pair with flash encryption)** |
| `transport` | `udp` \| `tcp` \| `tls` | low |
| `srtp` | off \| `sdes` (or DTLS-SRTP if added) | low |
| `nat_mode` | `rport` + symmetric-RTP toggle | low |
| `codecs` | locked to G.711 (`PCMU`/`PCMA`) — present for clarity, not really negotiable | low |
| `expires` | REGISTER lease (register mode) | low |

Mirror the existing config patterns: this is the same NVS open/set/commit shape as
`SipSecretStore` / `AdminAuth`, and it should be covered by the planned **config import/export** with
the `secret` field redacted/encrypted ([FEATURE_ROADMAP.md §3.2](FEATURE_ROADMAP.md)).

### 8.2 Dial-plan / DID-routing config

A small bounded table (mirror pool discipline — fixed cap, NVS-backed, no heap growth):

- **Outbound rules**: number-pattern → "send out trunk" (with optional prefix strip/prepend).
- **Inbound rules**: DID → target (extension or ring-group), reusing `pbx::RingGroup`.

### 8.3 TUI surface

The SSH "sysop terminal" (`src/Helpers/Tui.cpp`) gains a **Trunk / PSTN** screen alongside the
existing tabs (System Monitor · Network · PBX Config · Security · Reports/CDR · About):

- Trunk status (registered/static, last OPTIONS, last error), DID, transport/SRTP state.
- Edit trunk server/creds/NAT mode (creds entry warns if flash encryption is off — §5.3).
- Dial-plan editor (outbound patterns + inbound DID routing).
- Live relay-pool gauge (active bridged calls vs the §2.4 cap) so the operator can see the *real*
  trunk-media ceiling, not just `MAX_SESSIONS`.

---

## 9. Constraints, non-goals & phased build order

### 9.1 Honest constraints (RAM/CPU/airtime — NOT flash)

- **Trunk-call concurrency is media-bound, not pool-bound.** ~4–6 bridged calls on an S3 (half on a
  display build), set by **Wi-Fi half-duplex airtime** then CPU ([RTP.md §0](RTP.md)) — well below
  `MAX_SESSIONS`. Enforce a hard relay-pool cap and degrade gracefully (busy/`503`).
- **SRTP lowers that ceiling.** Per-packet AES+HMAC both directions; size SRTP tiers conservatively.
- **Trunk secret is recoverable at rest** → pairs with flash encryption / Secure Boot v2 (§5.3).
- **Flash is *not* a constraint** (~4 MB free); wolfSSL is already in the binary.
- **No RTP receive path exists yet** — the relay's RX/forward half is the principal new media code.

### 9.2 Non-goals (consistent with [FEATURE_ROADMAP.md §5](FEATURE_ROADMAP.md))

- ICE / STUN / TURN, and any TURN-relay-on-MCU (§4).
- On-MCU transcoding, mixing, conferencing, or recording (G.711 relay only).
- Inbound public SIP exposure / multi-trunk carrier gateway (one outbound upstream only).
- Naming or depending on any specific commercial softswitch/PBX/CPaaS product.

### 9.3 Phased build order (reused vs new)

| Phase | Deliverable | Reuses | Net-new |
|---|---|---|---|
| **1. Digest-UAC** | Answer `401`/`407` with computed credentials | `SipDigest::computeHa1`/`computeResponse` (inverse role) | UAC challenge-response state; trunk-cred storage (recoverable, separate from `SipSecretStore`) |
| **2. Outbound trunk / registration** | REGISTER (or static) to the upstream; outbound proxy/`Route`; OPTIONS keepalive; re-REGISTER | registration-lease pattern; `buildOptionsPing`; `Via`/`Contact` rewrite | UAC dialog toward provider; `rport`/symmetric-RTP NAT handling |
| **3. B2BUA G.711 bridge** | Terminate both SDP offers; relay media trunk↔ext; bounded relay pool + hard cap | `RtpSender` socket/pacing/SDP-answer; `enforceG711()`; `startBroadcastFork`/`huntRingNext`/`redirectInvite` for routing | **RTP receive + forward path** (both directions); comedia latching; dial plan / DID routing |
| **4. TLS/SIPS + SRTP** | Encrypted trunk signalling + media | wolfSSL (already linked) | SIP-over-TLS transport; SRTP encrypt/decrypt on the trunk leg |
| **5. (Optional) Call-control API** | Click-to-dial, programmable routing | HTTP/TLS client plumbing | API client + webhook handling |

> **Sequencing note.** Phase 1 (digest-UAC) rides directly on the digest work landing *now* for the
> registrar — same code, opposite role — so it is the natural first step. The media bridge (Phase 3)
> is the heaviest and the only true invariant-bender; gate it behind an explicit relay-pool cap from
> day one. TLS/SRTP (Phase 4) is feasible but is what makes the concurrency budget tight — treat its
> ceiling as separate from the plaintext ceiling.

---

## 10. Summary

pocket-dial can grow from a LAN-only signalling registrar into a small **B2BUA edge gateway** that
trunks up to one commercial softswitch/CPaaS fabric and through it to the PSTN. The design reuses the
existing engine almost wholesale — digest auth (in the inverse UAC role), the registration-lease and
OPTIONS-keepalive patterns, the fork/redirect/ring-group routing, and the `RtpSender` media
beachhead — and consciously bends exactly **one** invariant: for *trunk calls only*, the box becomes a
G.711 media bridge between two RTP domains (a handful of relays, no transcode/mix). The constraints
that matter are **RAM/CPU and Wi-Fi airtime** for that bridge (and SRTP throughput), not flash; the
security posture is **outbound-trust to one upstream** — far safer than facing the public SIP
internet — with TLS/SRTP and flash-encrypted trunk credentials as the durable hardening. NAT is solved
with trunk-native `rport`/symmetric-RTP, explicitly **not** ICE/TURN.
