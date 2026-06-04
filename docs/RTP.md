# Server-Side RTP on Pocket-Dial: Design Exploration

> **Status:** Proposed / exploration. No code written yet.
> **Audience:** A future engineer who will implement the recommended phase.
> **Scope:** What it would take to move RTP media *through* the device, what it
> buys us, what it costs on an ESP32-S3, and a phased path that stays "fast and light".

---

## 0. TL;DR (read this first)

* **Today the server touches zero media.** SIP signalling (REGISTER / INVITE /
  OK / BYE / the 777 echo and 999 all-page) flows through `RequestsHandler`, but
  the actual G.711 audio streams **peer-to-peer (P2P)** between phones. The
  server only rewrites/forwards SDP-bearing SIP messages; it never opens an RTP
  socket. See `docs/ARCHITECTURE.md` and the `m=audio` / `enforceG711()` path in
  `src/SIP/SipMessage.cpp` and `src/SIP/SipSdpMessage.cpp`.
* **Recommended first phase: Music-on-Hold (MoH) player + RTP statistics.** It is
  the lightest server-side RTP feature with the highest value-to-risk ratio. It
  is a *transmit-mostly, single-direction* path that does not require a jitter
  buffer or a mixer, and it proves out the RTP socket / packetiser / SDP-rewrite
  plumbing that every later phase reuses.
* **Realistic concurrent-stream ceiling on this hardware:** roughly
  **4–6 relayed two-party calls (8–12 RTP streams)** on an S3 at 240 MHz with
  Wi-Fi, *if* media handling is isolated on the otherwise-idle core (headless/
  Ethernet builds) and packets bypass the SIP `_mutex`. **Conference mixing (999)
  realistically caps at ~6–8 mixed participants** before the 20 ms deadline gets
  tight. On a **display build the practical ceiling is ~half that**, because
  Core 1 is reserved for LVGL.
* **Single biggest risk:** **Wi-Fi half-duplex / airtime contention**, not CPU.
  Relaying doubles every media packet (one in, one out) over a shared half-duplex
  radio that is already carrying SIP, HTTP dashboard polling, and (on display
  boards) nothing-to-spare. RTP is real-time and unforgiving: a few hundred ms of
  added queueing delay or a burst of loss turns into audible glitches, and there
  is no retransmit for UDP media.

---

## 1. Why P2P today, and what RTP-through-server would unlock

### 1.1 Why media is peer-to-peer today

The device is a **registrar + proxy/redirect-ish B2B-light signalling box**. When
A calls B:

1. A's INVITE (with SDP offering `m=audio <portA> RTP/AVP 0 8 101`) reaches the
   server. The server enforces G.711 µ-law(0)/A-law(8)/telephone-event(101) via
   `SipMessage::enforceG711()` and forwards the offer toward B.
2. B answers 200 OK with its own SDP (`m=audio <portB> ...`). The server forwards
   it back to A.
3. **The `c=` connection address and `m=` port in each SDP point at the phones
   themselves.** Once both sides have each other's `IP:port`, they stream RTP
   directly. The server is out of the media path entirely.

This is exactly why `PoolConfig.hpp` can say a `Session` "costs the server only
signalling/bookkeeping RAM — not bandwidth or DSP." A session is ~200 B of state.
It is the reason 8 concurrent calls fit on a plain ESP32. **The architecture is
fast and light precisely because the server never sees a single audio packet.**

### 1.2 What flowing RTP through the server would unlock

| Capability | Why P2P can't do it | What server-side RTP enables |
| :--- | :--- | :--- |
| **NAT traversal / media relay** | Two phones behind different NATs can't reach each other's `IP:port` directly. | Server becomes the rendezvous point (B2BUA media relay / TURN-like). Both phones send to the server; server forwards. |
| **Call recording** | Audio never reaches the server. | Server can fork a copy of each RTP stream to flash/PSRAM/network. |
| **Music-on-Hold (MoH)** | A phone on hold hears silence unless its own firmware plays a tone. | Server streams a stored G.711 clip to the held party. |
| **Real conferencing / mixing for 999** | Today's 999 is a *fork-and-pick*: it forks INVITEs to everyone and the **first** to answer wins; the rest are CANCELed (see `onInvite`/`onOk` in `RequestsHandler.cpp`). It is a 1:1 call, not a conference. | Server mixes N decoded PCM streams (sum + clip) and sends each participant the mix-minus-self. True all-page / talk-back. |
| **DTMF / announcements** | Phone-to-phone only. | Server can inject `telephone-event` (101) or play canned announcements ("all circuits busy", page chimes). |
| **RTP statistics (jitter / loss / MOS)** | Server sees no media, so no quality telemetry. | Server can compute per-stream jitter, loss %, and an estimated MOS for the dashboard. |

The high-value, low-cost subset is **MoH + RTP stats + announcements**. The
high-cost subset is **relay** (NAT) and **mixing** (999 conference).

---

## 2. Hard ESP32-S3 reality check ("fast and light")

### 2.1 The unit economics of one G.711 stream

G.711 at 8 kHz, 20 ms ptime:

```
8000 samples/s × 1 byte/sample × 0.020 s   = 160 bytes audio / packet
+ RTP header (12 B) + UDP (8 B) + IP (20 B) = 200 bytes on the wire / packet
packets per second                          = 1000 ms / 20 ms = 50 pps
payload bitrate                             = 160 B × 50 × 8   = 64 kbit/s
on-the-wire bitrate (one direction)         ≈ 200 B × 50 × 8   = 80 kbit/s
```

So **one RTP stream = 50 pps, ~64 kbit/s payload (~80 kbit/s on the wire),
each direction.** A two-party call is two streams each way.

### 2.2 Cost of a 2-party relay

A relay receives a packet on socket A and re-sends it to B (and vice versa). Per
two-party call:

* **Packets:** 50 pps in + 50 pps out, per direction = **~200 pps of relay work**
  (100 in, 100 out) for a single bidirectional call.
* **CPU:** Pure relay (no decode) is `recvfrom` → look up session → rewrite dest →
  `sendto`. That's a few µs of work plus two syscalls crossing the LwIP stack.
  The LwIP/socket path is the real cost, not arithmetic. Budget conservatively
  **~30–60 µs of CPU per relayed packet** including the LwIP traversal; at 200 pps
  that's ~6–12 µs/ms ≈ **0.6–1.2% of one core per call**. CPU is *not* the binding
  constraint for pure relay.
* **RAM:** A relay needs a small per-stream context (SSRC, last seq, remote
  `IP:port`, stats counters) plus a jitter buffer if used (see §2.5). Without a
  jitter buffer, ~200–400 B/stream. With a 60 ms jitter buffer of 160 B frames,
  add ~480 B–1 KB/stream. This is static-pool territory (§4).
* **Wi-Fi:** **This is the constraint.** Relaying *doubles* airtime: every audio
  packet is received and re-transmitted over the same half-duplex radio. A
  two-party relay = ~320 kbit/s of media airtime (4 × 80 kbit/s) plus 802.11
  per-frame overhead (ACKs, inter-frame spacing, retries) that dwarfs the payload
  at these tiny frame sizes. Small frames are *airtime-expensive*.

### 2.3 Cost of an N-party mixer (the 999 conference)

A mixer must **decode** each incoming stream to 16-bit PCM, **sum** them, **clip**
to int16, **re-encode** G.711, and send each participant the mix-minus-themselves.

```
per 20 ms frame, N participants:
  N × decode (G.711→PCM)        : 160 table lookups each  → cheap
  build mix bus                 : sum N×80 int16 samples
  per participant: subtract self, clip, encode (PCM→G.711)
  N × sendto                    : N syscalls / 20 ms
```

* **CPU:** Arithmetic is trivial (µ-law decode is a 256-entry LUT; encode is a
  small branch). The cost is again **syscalls and the per-frame deadline**: all N
  decodes + the mix + N encodes + N `sendto` calls must complete **every 20 ms**.
  At N=8 that's 8 recv contexts + 8 sends every 20 ms = 800 pps of socket work on
  one task, plus the mix math. Feasible but tightening.
* **RAM:** mix bus (80 × int16 = 160 B) + per-participant jitter buffer. With
  jitter buffers this is the dominant per-conference cost; budget ~1 KB ×
  N.
* **Hard limit:** the **20 ms wall clock**. If decode+mix+encode+send for all
  participants ever exceeds 20 ms, you drop a frame and everyone hears a click.
  Realistic mixed-participant ceiling: **~6–8** on a dedicated core; fewer if the
  core also runs SIP.

### 2.4 Concurrent calls vs. the static pools and memory headroom

Per `docs/SCALING.md`, internal DRAM headroom after IDF + Wi-Fi is ~290–320 KB on
a plain ESP32, and the existing pools cost ~37 KB. **The S3 has more SRAM and
PSRAM**, but two hard facts dominate:

1. **IRAM is ~100% used today.** Any new code that wants to be in IRAM (for
   speed / to run during cache misses) has essentially **no room**. RTP code must
   live in flash-cached IRAM-less code paths, which means it is subject to
   instruction-cache misses — acceptable for a 20 ms cadence, but it means the
   hot loop must not be latency-sensitive at the microsecond level.
2. **The session pool defaults to 8**, and each relayed/mixed stream needs its own
   RTP context + (optionally) jitter buffer in a **new static pool** (§4). These
   are not free like signalling sessions — a media session is ~1–2 KB, not 200 B.

**Realistic ceilings (rule of thumb, conservative):**

| Build | Media core available | Relayed 2-party calls | 999 mixed participants |
| :--- | :--- | :---: | :---: |
| Headless / Ethernet (S3) | Core 0 mostly free | **4–6** | **6–8** |
| Display (JC3248W535) | Core 1 = LVGL only; share Core 0 | **2–3** | **3–4** |

These are bounded first by **Wi-Fi airtime**, then by the **20 ms deadline**, then
by RAM. Wired Ethernet builds (W5500/LAN8720) relax the airtime constraint
substantially and are the only sensible target for serious relay/mixing.

### 2.5 Latency budget (jitter buffer adds delay)

Relaying/mixing adds delay on top of P2P:

```
P2P one-way:        capture(20) + network + playout         ≈ 40–80 ms typical
Relayed one-way:    capture(20) + net→server + server queue
                    + jitter buffer(40–80) + net→peer + playout
                    ≈ 100–180 ms
```

A **jitter buffer is mandatory** for mixing (you can't sum frames that arrive at
different times) and strongly recommended for relay over Wi-Fi (which reorders and
bursts). A 40–80 ms buffer (2–4 frames) is the practical floor. **Every ms of
buffer is a ms of added mouth-to-ear latency**, and the ITU-T G.114 comfort
ceiling is ~150 ms one-way. Server relay eats a big chunk of that budget, which is
another reason to **prefer P2P whenever possible** (§3b).

---

## 3. Architecture options

### (a) Pure relay / B2BUA — forward all RTP through the server

The server terminates both media legs. It rewrites the SDP it sends to each phone
so the `c=`/`m=` point at **the server's** relay IP and an allocated RTP port,
instead of the far phone. Each phone thinks it is talking to the server; the
server shuttles packets between the two legs.

* **Pros:** Solves NAT universally; enables recording, stats, and is the
  foundation for mixing; centralises media policy.
* **Cons:** Doubles airtime for *every* call (even ones that didn't need relay);
  adds latency to *every* call; consumes a media-session pool slot + ports +
  jitter buffer per call; biggest blast radius if the media task stalls.
* **Feasibility on this MCU:** Feasible for a *small* number of calls on a
  **wired** build. On Wi-Fi it is the riskiest option for the airtime reasons in
  §2.2. **Do not make this the default** — it taxes the "fast and light" promise
  on every call.

### (b) Selective relay — relay only when P2P fails / NAT detected

Default to P2P (today's behaviour). Only insert the server into the media path
when it is actually needed: e.g. the two endpoints are on different subnets, an
SDP `c=` is a private address unreachable from the peer, or a configured policy
flag forces it. This is the ICE/TURN philosophy applied minimally.

* **Pros:** Keeps the common case (same-LAN intercom — the device's primary use)
  fully P2P and zero-cost. Pays the relay tax only when unavoidable.
* **Cons:** Requires a heuristic for "do we need to relay?" (subnet compare on the
  offered `c=` vs. the registrar's known client address is a cheap, good-enough
  start). More signalling complexity in `onInvite`/`onOk`.
* **Feasibility:** Good. This is the right way to ship relay *if* relay is needed
  at all. But note: the device is primarily a same-LAN intercom, so the NAT case
  may be rare — validate the need before building it.

### (c) MoH player — stream a stored G.711 clip from flash/PSRAM (lightest, high value)

When a call is put on hold (re-INVITE with `a=sendonly`/`a=inactive`, or a feature
code), the server opens a *single transmit* RTP stream to the held phone and
plays a pre-encoded G.711 clip looped from flash or PSRAM.

* **Pros:** **Lightest possible server-side RTP.** Transmit-only: no jitter buffer,
  no decode, no mixing, no inbound media to schedule. The clip is already G.711
  (matches `enforceG711()`), so it is literally `read 160 bytes → packetise →
  sendto` every 20 ms. Proves the entire RTP socket/packetiser/SDP-rewrite
  pipeline that relay and mixing reuse. High perceived-quality win.
* **Cons:** Need a stored clip (flash space; a 30 s µ-law loop ≈ 240 KB — store
  in flash, optionally cache in PSRAM). Need to handle hold re-INVITE signalling.
* **Feasibility:** **Excellent.** This is the recommended first phase.

### (d) Conference mixer for 999

Replace the fork-and-pick 999 with a true mixer: every answering phone joins a
conference; the server mixes mix-minus-self and sends to each.

* **Pros:** Makes 999 a real all-page/talk-back. High wow-factor for an intercom.
* **Cons:** Most expensive option (§2.3): decode+mix+encode+send for all
  participants every 20 ms, N jitter buffers, hard real-time deadline. Highest
  risk of audio glitches under load.
* **Feasibility:** Feasible at small N (~6–8) on a dedicated core, but it is the
  last thing to build and should be gated behind measured headroom.

### Recommended phased path

```
Phase 1  ── MoH player (transmit-only) + RTP stats scaffolding   ← SHIP THIS FIRST
            • lightest, proves the RtpEndpoint/socket/packetiser/SDP-rewrite plumbing
            • single direction, no jitter buffer, no mixer
Phase 2  ── RTP statistics on a (optional) receive path
            • add an inbound RTP socket + jitter buffer + jitter/loss/MOS counters
            • surface on the dashboard via the existing snapshot model
Phase 3  ── Selective relay (option b), WIRED builds only by default
            • reuse Phase-1 packetiser + Phase-2 jitter buffer
            • gate behind subnet-mismatch heuristic; keep P2P as the default
Phase 4  ── 999 conference mixer (option d), gated by measured headroom
            • only after Phases 1–3 prove the scheduling model holds 20 ms
```

**"Light enough to ship": Phase 1 (MoH).** Everything past Phase 2 should be
opt-in at compile time and default to wired builds.

---

## 4. Implementation sketch

### 4.1 Where it hooks in

```
src/SIP/RequestsHandler.cpp   ← session wiring: on hold re-INVITE / 999, start/stop media
src/SIP/SipSdpMessage.*       ← SDP rewrite: point c=/m= at the server's relay IP:port
src/SIP/SipMessage.cpp        ← already has enforceG711(); media layer trusts 0/8/101
src/Media/RtpEndpoint.{hpp,cpp}   ← NEW: one UDP RTP socket + packetiser/depacketiser + stats
src/Media/RtpRelay.{hpp,cpp}      ← NEW: pairs two RtpEndpoints (relay) or fans out (mixer)
src/Media/MohPlayer.{hpp,cpp}     ← NEW (Phase 1): transmit-only G.711 clip looper
src/Media/JitterBuffer.{hpp,cpp}  ← NEW (Phase 2): fixed-depth reordering buffer
```

New module lives under a new `src/Media/` directory so it is cleanly separable and
compile-time excludable (`-DPOCKETDIAL_MEDIA=1`).

### 4.2 SDP rewrite (the signalling half)

`SipSdpMessage` already parses `c=` (`getConnectionInformation()`), `m=`
(`getMedia()`, `getRtpPort()`), and can replace the media line (`setMedia()`).
For relay/MoH we add a `rewriteMediaTarget(serverIp, allocatedPort)` that:

* replaces the `m=audio <port> ...` port with the server-allocated RTP port,
* replaces (or inserts) the `c=IN IP4 <addr>` with the server's media IP,
* leaves the `RTP/AVP 0 8 101` codec list intact (still G.711-enforced).

The rest of the SDP is untouched. This is a string-splice in the existing
`_messageStr`, consistent with how `enforceG711()` and `setMedia()` already work.

### 4.3 Packet path

```
PHASE 1 — MoH (transmit only):
  [MohPlayer task] every 20 ms:
     read 160 B from clip cursor (flash/PSRAM) → wrap in RTP (seq++, ts+=160, SSRC)
     → sendto(held_phone_ip:port)            (no recv, no jitter buffer)

PHASE 3 — Relay:
     recvfrom(legA_sock) → RtpEndpoint A depacketise → JitterBuffer A
     20 ms tick: pop JB A → repacketise (rewrite SSRC/seq/ts) → sendto(legB)
     (and symmetric B→A)

PHASE 4 — Mixer (999):
     for each leg: recvfrom → depacketise → decode µ-law→PCM16 → JitterBuffer
     20 ms tick:
        mixBus = Σ all legs' current PCM frame (saturating add, clip int16)
        for each leg i: out_i = mixBus − frame_i  → encode PCM16→µ-law → sendto(leg_i)
```

### 4.4 Jitter buffer (Phase 2+)

Fixed-depth ring of N frames (default 3–4 = 60–80 ms). Ordered insert by RTP
sequence number; pop one frame per 20 ms tick; conceal a missing frame by
repeating the last (or emitting comfort-silence). Keep depth a compile-time
constant so it lives in the static pool. **No dynamic resize on the hot path.**

### 4.5 Static-allocation & threading model

Consistent with the project's pool philosophy (`PoolConfig.hpp`, Issue #53):

* **New static pool** `_mediaPool` of `MediaSession` objects, sized by a new
  `POCKETDIAL_MAX_MEDIA` knob (default small, e.g. **2** on Wi-Fi / **4** on
  wired). Each holds its RtpEndpoint(s) + jitter buffer(s) + stats. Allocated at
  boot; `reset()`-recycled like clients/sessions. An INVITE that needs media but
  finds the media pool exhausted **falls back to P2P** (graceful) rather than 503.
* **RTP ports** allocated from a fixed range (e.g. 10000–10000+2×MAX_MEDIA),
  even ports for RTP per RFC 3550.
* **Dedicated FreeRTOS task** `media_task`, **priority equal-or-just-below the SIP
  receiver (5)**, driven by a 20 ms tick (or `recvfrom` with a 20 ms timeout):
  * **Display build:** pin to **Core 0** (Core 1 is LVGL-only — do not touch it).
    This shares Core 0 with HTTP/SIP, which is why the display ceiling is lower.
  * **Headless/Ethernet build:** the existing scheme moves SIP to Core 1 and
    leaves Core 0 freer; pin `media_task` to the core *not* running the SIP
    receiver so media and SIP don't fight for the same core.
* **Lock discipline:** media packets must **never** take the SIP `_mutex`. The
  media task reads the far-end `IP:port` from an immutable per-session snapshot
  captured at call-setup time (same spirit as the dashboard snapshot model).
  Socket `sendto` happens on the media task, never inside the registrar lock —
  identical to the existing Outbox pattern (Issue #24/#51).

### 4.6 Interaction with existing codec enforcement

The media layer **assumes G.711 0/8/101** because `enforceG711()` already
guarantees it in every SDP answer. The packetiser is therefore fixed-format: 160 B
payload, 20 ms, payload type 0 (µ-law) or 8 (A-law). DTMF (101) is passed through
as `telephone-event` RTP events. No format negotiation logic is needed in the
media path — a deliberate simplification that the existing signalling makes safe.

### 4.7 Media-path diagrams: P2P vs relayed

```
TODAY — Peer-to-peer (server out of media path):

   Phone A ───── SIP (INVITE/OK) ─────► [ pocket-dial ] ◄───── SIP ───── Phone B
        │                                  registrar/proxy                   │
        │                                  (rewrites SDP only)                │
        │                                                                     │
        └──────────────── RTP G.711 (direct, 64 kbit/s ea way) ──────────────┘
                          server NEVER sees these packets


RELAYED (Phase 3) — server terminates both media legs:

   Phone A ───── SIP ─────►┌──────────────────────────┐◄───── SIP ───── Phone B
        │                   │       pocket-dial        │                      │
        │                   │  RtpEndpoint A   B        │                      │
        └─ RTP ────────────►│  recv→JB→repacketise→send│◄──────────── RTP ────┘
            (to server)     │  (and symmetric B→A)     │   (to server)
                            └──────────────────────────┘
                          every packet crosses the radio TWICE (in + out)
```

---

## 5. Risks and recommendation

| Risk | Detail | Mitigation |
| :--- | :--- | :--- |
| **Wi-Fi half-duplex / airtime (BIGGEST)** | Relay doubles media airtime on a shared half-duplex radio; tiny 200 B frames are airtime-inefficient (802.11 overhead dominates). This — not CPU — caps concurrency. | Default to P2P; relay only selectively (option b); prefer wired (W5500/LAN8720) builds for relay/mixing; cap `POCKETDIAL_MAX_MEDIA` low on Wi-Fi. |
| **Added latency** | Relay + jitter buffer adds 60–140 ms one-way, eating the G.114 150 ms budget. | Keep jitter buffer shallow (60–80 ms); never relay calls that work fine P2P. |
| **CPU saturation → glitches** | The 20 ms deadline is hard. Missing it (cache miss storm, contention with LVGL/HTTP) = audible clicks; mixing is most exposed. | Dedicated `media_task` on a non-LVGL core; bound N; measure watermark before enabling Phase 4. |
| **IRAM exhaustion** | IRAM is ~100% used; RTP code can't go in IRAM, so the hot loop runs from flash-cached code subject to i-cache misses. | Keep the per-frame loop small and branch-light; 20 ms cadence tolerates cache misses; do not add IRAM-hungry code. |
| **RAM** | Media sessions are ~1–2 KB each (vs ~200 B signalling sessions); jitter buffers dominate. | Small static media pool; fall back to P2P on exhaustion, not 503. |
| **Security — RTP injection** | An attacker who learns a session's `IP:port`/SSRC can inject forged RTP (audio injection, DoS). The existing SIP rate-limiter does not cover RTP ports. | Validate inbound RTP source against the negotiated peer `IP:port`; check SSRC continuity; apply a per-port packet-rate cap analogous to the SIP token bucket; only open relay ports for the duration of a call. |
| **Conflicts with "fast and light"** | The whole value proposition is that the server is media-free. Relay/mixing inverts that. | Make all media features compile-time opt-in; ship MoH (which is cheap and obviously valuable) first; treat relay/mixing as wired-build, measured-headroom features. |

### Recommendation

**Build Phase 1 (MoH player) first.** It is the only server-side RTP feature that
is unambiguously worth it: it is light (transmit-only, no jitter buffer, no mixer),
it delivers obvious user value (held callers hear music instead of silence), and
it builds the exact `RtpEndpoint` / socket / packetiser / SDP-rewrite plumbing
that every later phase reuses — so it de-risks the rest at the lowest cost.

**Defer relay and mixing** until there is a demonstrated need (a real NAT
deployment for relay; a real all-page-talk-back requirement for mixing), and when
built, make them **opt-in and wired-build-first**. For a same-LAN intercom — the
device's primary mission — P2P media remains the right default, and keeping it
that way is what keeps the device fast and light.
