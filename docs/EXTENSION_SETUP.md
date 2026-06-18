# DRAWBRIDGE — Extension Setup Guide

**Audience:** Installers and operators adding SIP phones to a running DRAWBRIDGE unit.

**Before you start:** The unit must be powered on and reachable. You need its LAN IP or
the `drawbridge.local` mDNS hostname (visible in [6] ABOUT if mDNS does not resolve).
SSH access and an admin PIN are required for Secure mode operations.

---

## Step 1: Point the Phone at DRAWBRIDGE

Extensions self-register — you do not pre-create them. In Open and Learn registrar modes,
any SIP phone that sends a REGISTER to the device is accepted and appears in the Extensions
list while it remains actively registered.

> The **[A]** add extension button in **[3] PBX CONFIG → Extensions** renders a form but
> does not persist entries. Extensions appear in the list only when a phone is registered —
> there is no persistent extension database to populate ahead of time.

**If you need Secure mode** (SIP digest authentication), the correct sequence is:

1. Set the registrar to **Learn** mode (SSH TUI **[4] SECURITY → [D] Devices → [M]**).
2. Configure the phone (see Step 2 below) and let it register — it appears in the
   Extensions list as **◐ LEARNED**.
3. With the extension highlighted, press **[S]** to assign a SIP digest secret.
4. Enter the same secret on the phone's SIP account password field.
5. Once all phones have secrets, switch registrar mode to **Secure**.

> Do not use extensions 777 or 999 — these are reserved for the echo test and all-page
> broadcast respectively.

---

## Step 2: Configure the Phone

All phones use the same core settings. The table below covers every required field.

### Universal Settings

| Field | Value |
|-------|-------|
| SIP Server / Registrar / Proxy | `drawbridge.local` — or the device's LAN IP if mDNS fails |
| SIP Port | `5060` |
| Transport | **UDP only** — TCP and TLS are not supported |
| Username / Extension | e.g., `1001` |
| Password / Auth secret | Blank (Open mode) — or the SIP secret assigned via TUI [S] (Secure mode) |
| Audio codecs | **G.711 µ-law (PCMU) and G.711 a-law (PCMA) only** |
| Registration expiry | Up to `3600` s |
| NAT / STUN / ICE / rport | **All off** — media is peer-to-peer on the LAN |

> **Codec is the most important setting.** DRAWBRIDGE does not transcode. If a phone
> offers only Opus, G.722, or G.729, the rewritten SDP will have no common codec and
> the call will have no audio. Disable every codec except PCMU and PCMA.

---

## Per-Phone Configuration

### Linphone (desktop or mobile)

1. Settings → Account → add account manually.
2. SIP address: `sip:1001@drawbridge.local` (or the device IP).
3. SIP proxy / transport: `drawbridge.local:5060`, **UDP**.
4. Disable outbound proxy, ICE, STUN, and TURN under network settings.
5. Audio codecs: enable **PCMU** and **PCMA**, disable Opus, G.722, and speex.

### Zoiper

1. Add account → manual configuration → SIP.
2. Domain / host: `drawbridge.local` (or device IP), username `1001`, transport **UDP**.
3. Codecs: keep only **G.711 u-law** and **G.711 a-law**.
4. Disable STUN and rport in network settings.

### MicroSIP (Windows)

1. Menu → Add Account.
2. SIP server: `drawbridge.local` (or device IP), Username `1001`,
   Domain `drawbridge.local`, transport **UDP**.
3. Codecs: select **PCMU** and **PCMA** only.

### Yealink (T2x / T4x series)

Configure via the phone's web UI (http://`<phone-ip>`):

1. Account → Register:
   - Server Host: `drawbridge.local` (or device IP)
   - Port: `5060`
   - Transport: **UDP**
   - Register Name / User Name: `1001`
   - Password: SIP secret (Secure mode) or leave blank (Open mode)
2. Codec: move **PCMU** and **PCMA** to the enabled list; remove Opus, G.722, and G.729.
3. NAT: NAT Traversal = Disabled, rport = Disabled, STUN = Disabled.

> DRAWBRIDGE automatically strips the SDP body from `180 Ringing` responses. This
> prevents early-media loops and ringing hangs on Yealink phones — no phone-side
> configuration is required.

### Grandstream (GXP / GRP series)

1. Account → General Settings:
   - SIP Server: `drawbridge.local` (or device IP)
   - SIP Transport: **UDP**
   - SIP User ID: `1001`
   - Authenticate ID: `1001`
   - Password: SIP secret (Secure mode) or leave blank (Open mode)
2. Audio codecs: preferred vocoder list = **PCMU** then **PCMA** only; clear the rest.
3. Disable STUN and "Use NAT IP".

### Cisco (SPA / MPP series)

1. Ext 1 → Proxy and Registration:
   - Proxy: `drawbridge.local` (or device IP)
   - Transport: **UDP**
2. Subscriber Information: User ID `1001`; password if using Secure mode.
3. Audio Configuration: preferred codec **G711u** / **G711a**; disable all others;
   turn off ICE and STUN.

### Polycom / Poly

1. Configure the SIP registrar address `drawbridge.local:5060`, transport **UDPOnly**,
   line address / auth user `1001`.
2. Password: SIP secret (Secure mode) or leave blank (Open mode).
3. Codec preferences: enable **G711_Mu** and **G711_A**, disable G.722 and Opus.

---

## Step 3: Verify Registration

Work through these checks in order. Each step confirms a narrower layer.

1. **Phone status:** the phone's display or status indicator shows "registered" (or
   equivalent — "Ready", a solid icon, etc.).

2. **TUI confirmation:** on DRAWBRIDGE, press **1** (SYSTEM MONITOR). The extension
   should appear as **● ONLINE** in the extension roster.

3. **Echo test:** from the registered phone, dial **777**. You should hear your own
   voice played back. This confirms:
   - Outbound SIP signaling works
   - The codec negotiated to G.711
   - RTP audio flows from the phone to the device and back

4. **Extension-to-extension call:** register a second phone on a different extension.
   Dial from phone 1 to phone 2 — confirm two-way audio. This confirms:
   - Inbound SIP signaling works on both phones
   - P2P RTP between the two phones flows correctly

5. **Page test:** dial **999** from any registered phone. All other registered phones
   should auto-answer and hear the page audio. This confirms the multicast page path.

---

## Common Problems

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| No audio on calls | Codec mismatch | Ensure only G.711 µ-law and a-law are enabled on the phone; disable Opus, G.722, G.729 |
| Registration timeout / "unable to register" | Wrong server address or port | Confirm the SIP server field is `drawbridge.local` or the correct device IP; port is `5060`; transport is UDP |
| `401 Unauthorized` in Secure mode | Missing or wrong SIP secret | Re-enter the secret on the phone; confirm it matches the secret assigned via **[S]** on the extension in the TUI |
| Extension drops after ~15 s | Phone not responding to OPTIONS keepalive | Enable SIP keep-alive in the phone's NAT or account settings (most phones have this on by default) |
| NAT/STUN errors in phone logs | NAT/STUN still enabled | Disable STUN, ICE, rport, and any NAT traversal settings on the phone |
| One-way audio | Incorrect local IP reported by phone | Disable NAT traversal; confirm the phone and DRAWBRIDGE are on the same L2 subnet |
| Ringing phone shows no active call in TUI | Extension not registered (phone re-registered to wrong address) | Verify SIP server field on phone points to DRAWBRIDGE, not a previous registrar |

---

## See Also

- [OPERATOR_ADMIN.md](OPERATOR_ADMIN.md) — managing extensions, ring groups, and security modes
- [LEARN_MODE.md](LEARN_MODE.md) — cutover procedure for migrating an existing phone fleet
- [API.md](API.md) — HTTP API for status and session queries
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — extended diagnostics for registration and audio failures
