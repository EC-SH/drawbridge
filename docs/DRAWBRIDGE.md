# DRAWBRIDGE — System Architecture Overview
**Working codename:** DRAWBRIDGE (provisional — the device answers the door for nobody; the bridge lowers outbound only)  
**Status:** Draft for review · v0.1  
**Owner:** Glomar / Engage Communications  
**Scope:** Full-stack architecture for a LAN-sovereign edge PBX whose only PSTN connectivity is an authenticated, outbound, operator-owned media API. Covers edge device, media anchor, CLEC core integration, and fleet plane.

---

## 0. Doctrine
* **One sentence:** A real PBX on the customer LAN, with zero inbound WAN surface, reaching the PSTN exclusively through outbound TLS to infrastructure the operator owns end-to-end.
* **Design invariants (violations are bugs, not trade-offs):**
  1. **No inbound listener on the WAN interface.** Ever. No SIP trunk registration, no exposed RTP range, no management port. All external connectivity is initiated by the device.
  2. **No shared secrets.** Device identity is an mTLS certificate issued by the operator CA. SIP passwords do not exist anywhere in the system's external surface.
  3. **LAN survivability.** Internal calling, paging, and intercom function with the WAN cable cut.
  4. **Anchor independence.** The device speaks an abstract anchor contract (§4). 3CX Call Control API is the first binding, not the architecture.
  5. **Auditable opacity.** Outbound-only posture + inspectable firmware = a fleet the operator can provably not reach into uninvited. The security posture is the product.

---

## 1. System Topology

```
 CUSTOMER PREMISES                    │            ENGAGE INFRASTRUCTURE (GCP / edge)
                                      │
 ┌──────────┐  SIP/RTP (LAN only)     │
 │ Handsets ├──────────────┐          │
 └──────────┘              │          │   mTLS, outbound only
 ┌──────────┐         ┌────┴─────┐    │   ┌──────────────────┐      ┌─────────────────┐
 │ Door/    ├─────────┤DRAWBRIDGE│════╪══▶│  MEDIA ANCHOR     │─SIP──┤ CLEC CORE       │
 │ paging   │  GPIO/  │  (T-ETH  │    │   │  (per-tenant      │ /TDM │ trunks · LCR    │
 └──────────┘  audio  │ Elite S3)│    │   │   route point)    │      │ STIR/SHAKEN     │
                      └────┬─────┘    │   │  PCM ⇄ JSON ctrl  │      │ E911/RAY BAUM   │
                       PoE 802.3af    │   └────────┬─────────┘      │ CNAM · fraud    │
                       (single cable) │            │                 │ number inventory│
                                      │   ┌────────┴─────────┐      └─────────────────┘
                                      │   │  FLEET PLANE      │
                                      │   │ CA · ZTP · OTA    │
                                      │   │ telemetry · audit │
                                      │   └──────────────────┘
        ══▶  = WSS control channel + HTTPS PCM streams (all device-initiated)
```

Four roles. Each is independently replaceable; the contracts between them are the architecture.

| Role | Lives | Owns | Replaceable by |
| :--- | :--- | :--- | :--- |
| **Edge device** | Customer LAN, PoE | Local dialplan, registrar, media bridge, survivability | Bigger silicon, same firmware contract |
| **Media anchor** | Engage GCP region nearest footprint | RTP ⇄ PCM termination, per-tenant route points, anchor API | 3CX / Apidaze → Porta / FreeSWITCH / custom (§4) |
| **CLEC core** | Existing Engage infrastructure | Trunks, attestation, E911, CNAM, fraud, DIDs | Nothing — this is the moat |
| **Fleet plane** | Engage GCP | CA, provisioning, OTA, telemetry | Grows from scripts to service |

---

## 2. Edge Device — Hardware Commitments
* **Target:** LilyGO T-ETH Elite S3 (ESP32-S3-WROOM-1, 16 MB flash, 8 MB PSRAM, W5500 SPI Ethernet, 802.3af PoE Class 0, microSD).

| Resource | Assignment | Notes |
| :--- | :--- | :--- |
| **SPI2 (FSPI)** | W5500: SCLK IO48, MOSI IO21, MISO IO47, CS IO45, INT IO14 | 40 MHz target; validate trace integrity to 50+ |
| **SPI3** | microSD: SCLK IO10, MOSI IO11, MISO IO09, CS IO12 | Hard isolation from NIC host — SD I/O must never block media |
| **Core 0** | `esp_eth` task, lwIP `tcpip` thread, TLS record processing | Network core |
| **Core 1** | Media bridge, playout buffer, SIP UA, app logic | Media/app core |
| **PSRAM** | TLS session buffers, playout buffers, call state, prompt cache | Internal SRAM reserved for hot-path pbufs and DMA |
| **Flash 16 MB** | Firmware A/B (2× ~3 MB), prompt/tone assets, config NVS | OTA requires A/B from day one |
| **microSD** | CDR spool, voicemail, debug captures | Treat as untrusted/removable; encrypt at rest |
| **PoE** | Sole power path for v1 | Single-cable install is a product feature; brownout detection mandatory |

Ethernet is via `esp_eth` W5500 MACRAW → lwIP. The W5500's internal TCP/IP stack is unused (see prior analysis; ceiling is SPI pps/bandwidth, ~8–10 Mbps practical).

---

## 3. Edge Device — Software Architecture

### 3.1 Component map
```
┌─────────────────────────────────────────────────────────────┐
│                        DRAWBRIDGE FIRMWARE                   │
│                                                              │
│  LAN PLANE                      WAN PLANE                    │
│  ┌────────────────────┐         ┌─────────────────────────┐  │
│  │ SIP registrar/UA   │         │ Anchor Client            │  │
│  │  (handset reg,     │         │  ├ Control: WSS JSON     │  │
│  │   ext↔ext direct   │         │  ├ Media: HTTPS PCM      │  │
│  │   media, REFER)    │         │  │   (GET rx / POST tx)  │  │
│  ├────────────────────┤         │  └ Identity: mTLS client │  │
│  │ Local dialplan     │         ├─────────────────────────┤  │
│  │  + VSC layer       │         │ Anchor Abstraction (§4)  │  │
│  ├────────────────────┤         │  binding: 3cx-ccapi      │  │
│  │ DTMF feature codes │         └─────────────────────────┘  │
│  │ paging / intercom  │                                      │
│  └─────────┬──────────┘                                      │
│            │            MEDIA BRIDGE                          │
│            │   ┌──────────────────────────────┐               │
│            └──▶│ RTP leg ⇄ playout buffer ⇄   │◀── PCM stream │
│                │ PCM leg  (G.711⇄lin16 LUT)   │               │
│                └──────────────────────────────┘               │
│                                                              │
│  PLATFORM: config store (NVS) · SSH-only mgmt · OTA A/B ·     │
│            telemetry agent · watchdog/health · island-mode FSM│
└─────────────────────────────────────────────────────────────┘
```

### 3.2 LAN plane
* **SIP registrar/UA:** Handsets register to the device; standards-enough SIP that commodity Yealink/Grandstream/Poly endpoints work without per-model hacks. Ext↔ext calls negotiate direct media — the S3 touches signaling only, so internal calling consumes no media budget.
* **Dialplan + VSC layer:** Local extensions, feature codes, and vertical service codes resolved on-device. External patterns route to the WAN plane. (This layer is shared lineage with pocket-dial's DTMF/VSC programming model — see §12.)
* **DTMF:** For LAN legs, RFC 4733 from handsets; for API legs, DTMF arrives as JSON events from the anchor — no in-band detection required on the hot path.

### 3.3 WAN plane
* **Control channel:** One persistent WSS connection to the anchor. Carries call state events, DTMF events, and call-control requests (make/answer/divert/transfer/drop), request/response keyed by ID. Reconnect with jittered exponential backoff; channel health is the island-mode trigger.
* **Media channels:** Per active external call, one full-duplex raw PCM path (16-bit/8 kHz/mono): chunked HTTPS GET for far-end audio, chunked POST for near-end. Connections persistent and pre-warmed where possible — the TLS handshake is the expensive event; steady-state AES-GCM is hardware-accelerated and cheap.
* **Identity:** mTLS client cert from the Engage CA, device-unique, provisioned at manufacture or first-boot claim (§7). The anchor authenticates the device; per-call authorization is tenant policy at the anchor.

### 3.4 Media bridge — the one hard real-time problem
Per external call: `handset RTP (G.711, 20 ms) ⇄ bridge ⇄ anchor PCM (TCP)`.
* **Codec work:** G.711 ⇄ linear is a 256-entry table lookup each way. Effectively free.
* **RTP → TCP direction (near-end out):** Packetize to fixed-size PCM chunks, write to POST stream. TCP coalescing is fine here; the anchor's jitter tolerance is the operator's problem and the path is short.
* **TCP → RTP direction (far-end in):** This is the fiddly piece. TCP delivers in-order but bursty (delayed ACKs, retransmit stalls, congestion bursts). The playout buffer must convert that into metronomic 20 ms RTP toward the handset:
  - Adaptive depth, target 60–100 ms initial, floor 40 ms, ceiling 200 ms; depth adapts on observed inter-chunk arrival variance.
  - **Underrun policy:** Comfort noise / last-packet replay for ≤2 frames, then silence; never stall the RTP clock.
  - **Overrun policy:** Time-compress by frame drop (PCM makes this trivial — no codec state to corrupt).
  - **Instrument from day one:** Buffer depth histogram, underrun/overrun counters per call, exported via telemetry. This data drives anchor placement decisions.
* **Clocking:** RTP egress driven by a hardware timer, not task scheduling; media task on Core 1 at priority above everything except the timer ISR.

### 3.5 Management
SSH only (key auth, LAN interface only), consistent with the pocket-dial redesign: no web UI, no management exposure of any kind on WAN. Configuration is a versioned text artifact (NVS-backed, exportable), changes auditable. Fleet-pushed config arrives over the control channel as signed payloads; the device applies only operator-signed config.

---

## 4. Anchor Abstraction Contract
The portability seam. The device firmware speaks this interface; bindings implement it.

```
interface MediaAnchor {
  // control plane (persistent, multiplexed)
  events:    stream<CallEvent>        // ringing, answered, dropped, dtmf, hold...
  request(CallAction) -> Result       // makecall, answer, divert, transfer, drop

  // media plane (per active call)
  rx:  stream<PCM16LE@8k mono>        // far end → device
  tx:  stream<PCM16LE@8k mono>        // device → far end

  // identity
  auth: mTLS client certificate
}
```

* **Binding 1 — `3cx-ccapi` (Testing-Working):** WSS event channel + `/callcontrol/{dn}/participants/{id}/stream` GET/POST against a per-tenant route point on Engage's hosted 3CX. (Currently verified and working).
* **Binding 2 — `apidaze` (Testing-Working):** Direct integration with the VoIP Innovations / Apidaze API. (Currently verified and working).
* **Binding 3 — `porta-switch` (Roadmap):** Integration with the Porta softswitch API.
* **Binding 4 — `engage-native` (later):** FreeSWITCH/rtpengine + thin control daemon, or fully custom, speaking the same contract — same wire shape, operator-defined, no per-tenant license drag, deployable to whatever region/edge the latency map says. The contract above is deliberately so small that this is a bounded project.
* **Rule:** No platform-specific leaks above the binding layer. Code review enforces it; an in-tree mock binding (loopback anchor) keeps everyone honest and enables CI without a PBX.

---

## 5. Call Flows
* **Outbound PSTN:** Handset dials → device dialplan classifies external → `request(makecall)` on control channel → anchor originates via CLEC core (LCR, attestation applied upstream) → `answered` event → device opens PCM rx/tx streams → bridge marries RTP leg to PCM leg. Teardown from either side propagates through events.
* **Inbound PSTN:** DID terminates at tenant route point → anchor emits `incoming` event → device applies dialplan (ring group, time-of-day, IVR) → on local answer, bridge as above. The PSTN never learns the customer's IP exists.
* **Ext ↔ Ext:** Registrar + direct media. Device is signaling-only; WAN irrelevant.
* **Island mode (WAN/anchor loss):** Control-channel health FSM declares island → LAN calling fully functional → external dials get local intercept tone/announcement → optional analog/cellular failover is explicitly out of scope for v1 (it would reintroduce the attack surface the architecture exists to eliminate; if a customer requires it, it's a separate hardened module and its own ADR). Recovery: reconnect, re-sync call state, re-register presence with anchor.
* **Feature codes / VSC:** Resolved locally; codes that require PSTN action (e.g., call forward to external number) translate to anchor control requests or tenant config updates pushed upstream.

---

## 6. Identity & Security Model
* **PKI:** Engage operates a private CA (offline root, online issuing intermediate). Each device: unique keypair generated on-device (key never leaves), CSR at provisioning, short-lived certs (90 d) auto-renewed over the control channel. Revocation = fleet kill switch.
* **No credentials class:** SIP trunk passwords structurally absent → credential-stuffing trunk fraud (the classic small-ITSP bankruptcy event) cannot occur against this product line. LAN handset registration uses per-extension secrets confined to the LAN and never valid upstream.
* **Surfaces:** WAN — zero listeners, outbound 443 only (a firewall rule a customer's IT department will actually approve). LAN — SIP/RTP to local subnet + SSH (keys only). Physical — flash encryption + secure boot on the S3; SD treated as hostile.
* **Fraud posture:** All toll decisions execute at the CLEC core where fraud controls already live; the device cannot originate PSTN traffic except through its authenticated tenant route point with tenant policy applied. Per-tenant rate/spend limits are anchor-side config.
* **Transparency property:** Outbound-only + signed OTA + reproducible builds (goal) = the operator can demonstrate non-access. Publish the claim; let it be audited. This is a differentiator, market it as one.

---

## 7. Provisioning & Fleet Operations
* **Zero-touch flow:** Device manufactured with bootstrap identity → customer plugs into PoE → device DHCPs, dials fleet plane, presents bootstrap cert → operator claims device to tenant (one click against serial) → device receives tenant cert, DID mapping, dialplan, anchor endpoint → handsets register → dial tone. Target: plug-in to first call < 5 minutes, zero on-site technical skill.
* **OTA:** A/B partitions, signed images, staged rollout rings (lab → friendly tenants → fleet), automatic rollback on boot-loop or post-update health check failure. Updates fetched over the existing outbound channel.
* **Telemetry (up the channel the device already holds open):** Registration state, call counts, per-call MOS-proxy stats (playout buffer histograms, underruns, RTT to anchor), resource health (heap/PSRAM watermarks, SPI error counters, temperature), island-mode events. No audio content, ever — telemetry schema is published and auditable, consistent with §6.
* **CDR:** Authoritative CDRs live at the anchor/core (billing already happens there); device keeps a local spool for survivability reconciliation.

---

## 8. Capacity & Performance Budgets
Per-external-call cost on the device: one LAN RTP leg + one TLS PCM duplex stream + bridge.

| Budget Item | Figure | Basis |
| :--- | :--- | :--- |
| **Concurrent external calls (engineering ceiling)** | 10–12 | SPI bandwidth + pps + TLS record processing, tuned build |
| **Concurrent external calls (spec/sell)** | 8 | Margin for telemetry, OTA, SD activity, bad days |
| **Ext ↔ Ext concurrent** | Effectively unbounded | Direct media; dialog state in PSRAM |
| **Added latency vs direct trunk** | ~50–100 ms budget | Anchor hop + playout buffer; shrink via anchor placement |
| **PCM per call** | 256 kbps full duplex | 2 × 128 kbps; SPI path ≈ 8–10 Mbps practical |
| **TLS sessions resident** | Dozens | PSRAM-backed mbedTLS buffers |
| **Device cost target** | < $40 BOM | Board ~$20 retail; custom spin later if volume warrants |

Failure-mode ordering under overload mirrors the relay analysis: jitter tail ──▶ playout adaptation ──▶ call admission control. Implement call admission control (reject Nth call with busy) rather than degrading all calls — degraded audio on every call is a worse product than an honest busy signal on one.

* **Scale note:** A tenant needing >8 simultaneous PSTN calls gets a second device or bigger silicon behind the same anchor contract — the architecture is horizontal at the edge by construction.

---

## 9. Failure Modes & Survivability Matrix

| Failure | Customer Experience | Device Behavior |
| :--- | :--- | :--- |
| **WAN down** | Internal calls fine; external = intercept tone | Island FSM; reconnect backoff; event on restore |
| **Anchor down, WAN up** | Same as above | Distinguishable in telemetry; fleet alert |
| **Anchor regional failover** | Brief external outage; calls in progress drop | Control channel re-establishes to secondary anchor endpoint (list provisioned) |
| **PoE brownout/loss** | Phones also PoE — site power design question | Clean shutdown, CDR spool flush |
| **Cert expired/revoked** | External calls refused | Local alarm state; LAN unaffected |
| **Device dies** | Site rollback: spare device, claim to tenant, config restores from fleet plane | RMA; state is server-side by design |
| **Congested customer uplink** | Playout buffer absorbs; worst case audible stall-burst | Buffer histograms flag the site; sell them better internet |

---

## 10. Open Decisions (ADR queue)
Each gets a real ADR before the milestone that depends on it. Current leanings noted, not decided.
* **ADR-001 — SIP stack on device:** Port a proven minimal stack vs. own UA implementation. Forces: commodity-handset interop (the long tail of Yealink quirks) vs. footprint and license. Leaning: smallest battle-tested stack that passes a 3-vendor handset matrix. Blocker for M2.
* **ADR-002 — PCM transport framing to anchor:** Raw chunked HTTP (3CX-shaped) vs. WS-framed audio in the native binding. Forces: binding symmetry, head-of-line behavior, proxy friendliness. Blocker for M5 only.
* **ADR-003 — Playout buffer adaptation algorithm:** Fixed+watermark vs. delay-percentile adaptive. Forces: the §3.4 instrumentation decides this empirically — ship simple, adapt on fleet data. Blocker for M3 exit.
* **ADR-004 — Config model:** Single signed text artifact vs. structured store with deltas. Forces: auditability (favors artifact) vs. fleet-push granularity. Blocker for M4.
* **ADR-005 — Native anchor technology:** FreeSWITCH vs. rtpengine+daemon vs. ground-up. Forces: time-to-replace-3CX vs. operational fit with Engage GCP. Blocker for M5.
* **ADR-006 — Island-mode external failover:** Confirmed out of scope v1, or hardened optional module. Forces: doctrine invariant #1 vs. market demand. Decide after first 10 tenants ask (or don't).

---

## 11. Milestone Roadmap
Each milestone has a demo and an exit criterion. No milestone depends on an undecided ADR.
* **M0 — Bench transport proof:** S3 ↔ 3CX CCAPI: control channel up, one PCM stream each way, audio audible both directions on a single call. Exit: loop a test call for 1 hour, zero stream stalls on LAN-local anchor. *(De-risks: the entire premise.)*
* **M1 — Media bridge:** One handset (RTP) bridged to one PSTN call via anchor. Playout buffer v1 with full instrumentation. Exit: MOS-credible audio over a WAN-realistic path (netem-impaired link), buffer histograms collected.
* **M2 — LAN PBX:** Registrar + dialplan + ext ↔ ext direct media + DTMF feature codes; 3-vendor handset matrix passes. Exit: an office of 5 handsets lives on it for a week. *(Depends: ADR-001.)*
* **M3 — Concurrency & admission:** N simultaneous external calls to spec figure; call admission control; overload behaves per §8. Exit: 8 concurrent calls, 24-hour soak, zero underrun-caused artifacts at P95. *(Depends: ADR-003.)*
* **M4 — Fleet plane v1:** CA, claim flow, ZTP, signed OTA with A/B + rollback, telemetry ingest. Exit: factory-reset device to first call < 5 min, untouched by an engineer. *(Depends: ADR-004.)*
* **M5 — Native anchor:** `engage-native` binding deployed in GCP; device swaps bindings by config. Exit: same M3 soak passes against the native anchor; 3CX demoted to "supported binding." *(Depends: ADR-002, ADR-005.)*
* **M6 — Pilot:** 3–5 friendly Engage tenants in production. Exit: 30 days, fleet telemetry clean, one honest postmortem written.

**Sequencing rationale:** M0 before everything because the PCM-over-CCAPI path is the only premise not already proven in this design; M5 deliberately late because 3CX is a perfectly good crutch while the edge is being hardened.

---

## 12. Relationship to pocket-dial
Shared lineage, different products, deliberate component reuse:

| Component | pocket-dial | DRAWBRIDGE |
| :--- | :--- | :--- |
| **VSC / DTMF programming layer** | Origin | Direct reuse |
| **SSH-only management posture** | Origin (post-redesign) | Direct reuse |
| **SIP UA / registrar** | Shared library target | Shared library target |
| **WAN trunk model** | SIP (open-source, BYO-carrier) | Anchor API (operator-bound) |
| **Governance** | Open source | Engage product (open-core decision TBD — candidate ADR-007) |

**Rule:** Keep the shared pieces in a common library from M2 onward; divergence later is cheap, convergence later is not.

---
*v0.1 — drafted for development guidance; every figure in §8 is an engineering estimate to be replaced by bench data starting at M0.*
