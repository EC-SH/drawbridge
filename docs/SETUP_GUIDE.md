# DRAWBRIDGE Setup Guide

This guide covers the first-time configuration of a DRAWBRIDGE unit after firmware is
already on the board. It assumes **wired PoE Ethernet** on a LilyGO T-ETH-ELITE S3 — the
commercial reference hardware. There is no Wi-Fi or SoftAP on this board.

If you are looking for the abbreviated 10-minute path, see [QUICKSTART.md](QUICKSTART.md).
For firmware installation from scratch, see [FLASHING.md](FLASHING.md) or
[OTA.md](OTA.md) for over-the-air updates on an already-running unit.

**What you will do:**

1. [Power on and connect over Ethernet](#1-power-on-and-connect)
2. [Open the SSH TUI — the primary config surface](#2-access-the-ssh-tui)
3. [Set the admin PIN — mandatory first step](#3-set-the-admin-pin)
4. [Choose a registrar security mode](#4-configure-the-registrar-mode)
5. [Add extensions](#5-add-extensions)
6. [Register phones](#6-register-phones)
7. [Test calls](#7-test-calls)
8. [Site-handoff checklist](#8-site-handoff-checklist)

---

## 1. Power on and connect

1. Plug the board's Ethernet port into an **802.3af PoE switch port** or a **PoE injector**
   connected to your LAN. The board draws power from the Ethernet cable — no separate power
   supply is required.
2. The device obtains a DHCP address from your LAN automatically.
3. Wait approximately 15 seconds for boot to complete.
4. The device advertises itself via mDNS as **`drawbridge.local`**.

> If mDNS does not resolve on your network, find the device's IP address from your DHCP
> server's lease table, or check TUI [6] ABOUT after connecting on the same LAN.
> Use that IP address wherever this guide shows `drawbridge.local`.

The SIP registrar listens on **UDP port 5060**. The SSH config surface is on **TCP port 22**.
A web status page is available at `http://drawbridge.local` (port 80) — but note it is
**dark by default once the admin PIN is set**: the HTTP listener is closed except within a
bounded window opened by dialing `*4887` from the admin extension, a fresh provisioning
grace period, or a logged-in session's keep-alive (see THREAT_MODEL.md §5.6). SSH is
always available.

### Network placement note

Deploy the board on a **trusted, physically controlled LAN segment**. DRAWBRIDGE is not
designed for direct Internet exposure. SIP signaling and RTP media are cleartext on the
wire; physical switch-port discipline and LAN segmentation (e.g., a dedicated voice VLAN)
are the appropriate controls for wired deployments. If the phones and the DRAWBRIDGE unit
are on the same L2 segment, RTP flows peer-to-peer between phones — the device handles
signaling only for LAN extension-to-extension calls.

---

## 2. Access the SSH TUI

The SSH terminal is the **primary configuration surface** for DRAWBRIDGE. All security
settings, extension management, and system configuration are done here.

Connect from any host on the same LAN:

```
ssh sysop@drawbridge.local
```

If mDNS does not resolve:

```
ssh sysop@<device-ip>
```

> Any SSH username is accepted — the username field is not validated until the
> owner/sysop privilege model is fully implemented. `sysop` is the conventional label.

**On first boot: no password is required.** The TUI opens immediately to the **FirstRun**
setup screen. This is by design — DRAWBRIDGE is open for initial provisioning until you
set an admin PIN. Anyone who can reach the device on the LAN can connect unauthenticated
until the PIN is set. **Set the PIN before doing anything else.**

After the PIN is set, the SSH password is the admin PIN.

> The web dashboard at `http://drawbridge.local` is available for read-only status and OTA
> updates, but it is not the configuration path for field deployments. On a provisioned
> device it is dark by default — open it with `*4887` from the admin extension when needed.

---

## 3. Set the admin PIN

> **Do this first.** Until a PIN is provisioned, anyone on the LAN can reach the SSH TUI
> and change device configuration. A PIN engages the authentication gate immediately.

### From the FirstRun screen

The FirstRun screen prompts you to set a PIN on first boot. Follow the on-screen
instructions.

### From the TUI at any time

Navigate to **[4] SECURITY → [P] Set PIN** and follow the prompt.

**PIN requirements:**

| Property | Requirement |
| :--- | :--- |
| Minimum length | 4 characters |
| Recommended | 6 or more alphanumeric characters |
| Recovery | None — a lost PIN requires a factory reset (reflash) |

After the PIN is set:
- SSH authentication uses the PIN as the password.
- The PIN protects all configuration actions in the TUI.
- A 4-digit numeric PIN is technically accepted but trivially guessable. Use at least 6
  alphanumeric characters and record it in the site's secure credentials log.

---

## 4. Configure the registrar mode

The registrar mode controls how phones are permitted to register. This is a security
decision — choose deliberately.

Navigate to **[4] SECURITY → [D] Registrar → [M] Mode** to select a mode.

### The three modes

| Mode | Behavior | When to use |
| :--- | :--- | :--- |
| **Open** (default) | Any phone that knows an extension number can register without credentials. | Isolated bench testing only. Not for production on a shared LAN. |
| **Learn** | The first device to claim an extension is adopted automatically (trust-on-first-use, keyed by MAC address). Subsequent registrations from a different MAC are rejected once the extension is secured. | During initial phone onboarding. Run this window short, then move to Secure. |
| **Secure** | Every registration is SIP digest-challenged (RFC 2617). Only extensions with a configured secret can register. Each extension is locked to the MAC address that claimed it. | Steady-state production. The target for any completed deployment. |

### Recommended workflow for a new deployment

1. Set registrar mode to **Learn** while registering phones.
2. Confirm the adopted roster in **[4] SECURITY** — verify each extension shows the correct
   MAC address.
3. Assign a per-extension SIP secret to each extension via **[3] PBX CONFIG → Extensions → [S]**.
4. Flip mode to **Secure** once all phones are registered and secrets are set.

The Learn mode window is a deliberate, time-limited weakening of the registrar. On a
wired segment with physical access controls, this risk is manageable — but close the
window promptly. See [LEARN_MODE.md](LEARN_MODE.md) for the full cutover runbook,
including MAC-lock behavior, ARP resolution caveats, and rollback procedures.

> **Mode transitions require admin authentication and are not automatic.** There is no
> silent downgrade — moving from Secure back to Learn or Open is an explicit admin action.

---

## 5. Add extensions

In **Open** and **Learn** mode, phones self-register: configure the phone with any
extension number and the SIP registrar accepts it directly. No pre-provisioning step
is required.

> The [A] Add extension form in the TUI ([3] PBX CONFIG → Extensions) renders a
> layout for future use but does not persist entries yet — extensions appear in the
> list only while a phone is actively registered.

### Setting per-extension SIP secrets (required for Secure mode)

To use **Secure** registrar mode, each extension needs a SIP digest secret:

1. Leave the registrar in **Learn** mode temporarily.
2. Register the phone — it appears in the TUI extensions list while connected.
3. With the extension highlighted, press **[S]** to assign a secret (never echoed).
4. Enter the same secret on the phone's SIP account password field.
5. Once all phones have secrets set, switch registrar mode to **Secure**.

The device stores an HA1 hash of the secret — the actual secret text is not recoverable
from the device, but the stored HA1 is equivalent to a credential. Record all secrets in
the site's secure credentials log.

**Reserved extensions — do not use as phone extensions:**

| Extension | Purpose |
| :--- | :--- |
| 700–799 | Call park orbits |
| 777 | Echo test (loopback) |
| 98x | Zone paging |
| 999 | All-page broadcast |

---

## 6. Register phones

Universal SIP account settings for all phones:

| Setting | Value | Notes |
| :--- | :--- | :--- |
| SIP server / registrar | `drawbridge.local` or the device IP | Use the IP if mDNS does not resolve |
| Port | `5060` | UDP only |
| Transport | **UDP** | |
| Extension / username | e.g. `1001` | Any number except reserved extensions (777, 999, 700–799, 98x) |
| Password / secret | SIP secret set via TUI [S] | Blank if using Open mode |
| Codec | **G.711 µ-law (PCMU) and a-law (PCMA) only** | Disable Opus, G.722, G.729 |
| Codec payload | PCMU = 0, PCMA = 8, telephone-event = 101 | |
| Registration expiry | Up to 3600 seconds | Higher values are capped to 3600 |
| NAT / STUN / ICE | **All OFF** | The device is on the same LAN — no NAT traversal needed |

> **Codec requirement:** DRAWBRIDGE rewrites all SDP to `0 8 101` via `enforceG711()`.
> Phones that negotiate Opus or G.722 will have their codec list overridden. Disable
> non-G.711 codecs on each phone to avoid confusion during call setup.

After the phone registers, confirm it appears in the extensions list under **[3] PBX CONFIG**
or in the web dashboard at `http://drawbridge.local`.

The registrar sends an OPTIONS keepalive to each registered phone every 5 seconds and
prunes extensions that do not respond after approximately 15 seconds. A phone that does
not answer OPTIONS keepalives may disappear from the roster.

For per-model configuration walkthroughs and known quirks, see
[PHONE_COMPATIBILITY.md](PHONE_COMPATIBILITY.md).

---

## 7. Test calls

Verify the system before leaving the site. At minimum, run the echo test and one
extension-to-extension call.

### Echo test — dial 777

Dial **777** from any registered phone. The call answers immediately and echoes your
voice back to you using your own phone's RTP connection information. This tests
audio path, codec negotiation, and RTP connectivity without requiring a second phone.

- You should hear your own voice returned with a short delay.
- If the call answers but you hear nothing, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md)
  for one-way or no-audio diagnosis.
- If the call does not connect, confirm the phone is registered in **[3] PBX CONFIG**.

### All-page broadcast — dial 999

With at least two phones registered, dial **999** from one phone. All other registered
extensions ring simultaneously with auto-answer headers. The first extension to answer is
bridged to the caller; the rest stop ringing.

### Extension-to-extension call

Dial from one registered extension to another (for example, `1001` to `1002`).

- The callee phone rings; answer it.
- Two-way audio should be audible on both ends.
- RTP media flows **directly between the two phones** — the DRAWBRIDGE device only brokers
  the SIP signaling for LAN calls. The call appears in the active sessions list in
  **[3] PBX CONFIG** and on the web dashboard.

---

## 8. Site-handoff checklist

Complete and sign off on this list before leaving the site. Keep a copy in the site's
installation record.

**System**
- [ ] Firmware version confirmed — TUI **[6] ABOUT**
- [ ] Device IP noted; static DHCP binding configured on the switch for this MAC address
      (prevents IP changes on reboot)
- [ ] Unit is on a trusted, physically controlled LAN segment or voice VLAN

**Security**
- [ ] Admin PIN set — minimum 6 alphanumeric characters
- [ ] PIN recorded in the site's secure credentials log (not on a sticky note near the device)
- [ ] Registrar mode configured — Open / Learn / Secure (circle one)
- [ ] If Secure: per-extension secrets set and confirmed on each handset

**Phones**
- [ ] All phones registered and visible in TUI [3] PBX CONFIG → Extensions
- [ ] All phones show as registered in TUI [3] PBX CONFIG or web dashboard
- [ ] Echo test (777) passed on at least one extension — two-way audio confirmed
- [ ] Extension-to-extension call passed — two-way audio confirmed

**Optional / if applicable**
- [ ] PSTN trunk configured and verified (outbound and inbound calls tested)
- [ ] Ring groups configured
- [ ] Call park orbits tested (700–799)

---

## Next steps

- [PHONE_COMPATIBILITY.md](PHONE_COMPATIBILITY.md) — per-model phone configuration walkthroughs
- [LEARN_MODE.md](LEARN_MODE.md) — full cutover runbook for migrating an existing phone fleet
- [QUICKSTART.md](QUICKSTART.md) — abbreviated first-boot path (10 minutes)
- [OTA.md](OTA.md) — firmware updates after initial deployment
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — audio, registration, and connectivity problems
- [THREAT_MODEL.md](THREAT_MODEL.md) — security posture, hardening roadmap, and risk analysis
