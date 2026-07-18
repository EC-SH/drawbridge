# DRAWBRIDGE Quick Start — Zero to First Call

> Field-tech guide for a pre-loaded unit. No toolchain, no source code, no prior SIP knowledge required. Estimated time: 10 minutes.

---

## What you need

- DRAWBRIDGE unit (LilyGO T-ETH-ELITE S3, firmware pre-flashed)
- PoE switch port (802.3af) **or** a PoE injector — the board is powered entirely over Ethernet
- Cat-5e/Cat-6 Ethernet cable
- Laptop or PC on the same LAN with an SSH client (PuTTY, Terminal, etc.)
- One or two SIP phones (hardware or softphone) on the same LAN

> **No USB power needed.** The board draws power from the PoE port. Do not connect both PoE and USB simultaneously unless you know what you are doing (see [HARDWARE.md](HARDWARE.md)).

---

## Step 1 — Power on

Plug the Ethernet cable into a **PoE-enabled** switch port or injector, then into the DRAWBRIDGE unit.

Wait approximately **15 seconds** for the firmware to boot and acquire a DHCP lease. SSH will become reachable once the network stack is up.

---

## Step 2 — Find the device

Try mDNS first:

```
ssh drawbridge.local
```

If your network supports mDNS/Bonjour (most home and small-office networks do), this resolves automatically. The hostname is `drawbridge.local` and is advertised before the provisioning gate — it works even on a brand-new unprovisioned unit.

**If mDNS does not resolve:** check your router or switch's DHCP lease table for a hostname starting with `drawbridge`, or a MAC registered to Espressif. Then connect directly:

```
ssh <ip-address>
```

**SSH details:**
- Port: 22
- Username: any value works, or omit it entirely — the firmware does not check the username
- Password: **none** until you set the admin PIN (SSH is open on a fresh unit)

You will land in the DRAWBRIDGE SSH TUI — an ANSI terminal interface. No shell prompt.

---

## Step 3 — First-run setup

On a fresh unit, the TUI opens directly to the **FIRST RUN** screen. It shows three numbered steps and their current status:

```
[1] Set the admin PIN .............. ○ none   (SSH is OPEN until set)
[2] Network ........................ 192.168.1.45   ·   STATION
[3] PSTN trunk ..................... ⊘ DOWN
```

**Complete step [1] before anything else.** Press `1` to open the PIN-entry modal.

- Minimum length: 4 characters. Recommended: 6 or more alphanumeric characters.
- You are prompted to enter the PIN twice to confirm.
- **There is no PIN recovery path.** If the PIN is lost, the unit must be reflashed. Write it down and store it securely.

Once the PIN is set, the status chip for step [1] changes to `● SET` and SSH logins from this point forward require the PIN as the password.

> **PIN constraint:** the PIN may not begin with `4887` — that digit string is reserved as
> the web dashboard's DTMF open-code (below).
>
> **Web dashboard goes dark at this point.** Setting the PIN also puts the HTTP dashboard
> into its dark-by-default mode: it stays reachable for a grace window so you can finish
> onboarding, then the listener closes. To reopen it later, dial `*4887` (spells HTTP) from
> a phone registered as the admin extension (default `1001`) — the dashboard becomes
> reachable at `http://<device-ip>/` for a bounded window (default 10 min); a logged-in
> session can extend it by an hour with the "Keep open (1h)" button. SSH is unaffected —
> it stays available at all times. Details: `docs/THREAT_MODEL.md` §5.6.

Press `2` to confirm the IP address and network mode (informational — no action needed for wired Ethernet).

Press `Enter` to proceed to the hub.

> Step [3] (PSTN trunk) connects DRAWBRIDGE to an upstream telephony provider for external calls. It is not required for internal extension-to-extension calls. Skip it now and configure it later via the TRUNK tab in [3] PBX CONFIG.

---

## Step 4 — Register your first SIP phone

DRAWBRIDGE uses an **open registrar** — you do not pre-create extensions. Any SIP phone that sends a REGISTER to the device's IP on port 5060 is accepted and appears as a live extension.

Configure the phone with these settings:

| Setting | Value |
| :--- | :--- |
| SIP server / registrar | DRAWBRIDGE IP address (e.g. `192.168.1.45`) |
| SIP domain / proxy | same as SIP server |
| Port | `5060` |
| Transport | **UDP** (no TCP, no TLS) |
| Username / extension | your choice — e.g. `1001` (avoid 700–799, 777, 999) |
| Password | any value or blank |
| Audio codecs | **G.711 only** — enable PCMU (µ-law) and PCMA (a-law); **disable everything else** |
| NAT / STUN / ICE / rport | **off** |

**The codec setting is the most common source of no-audio calls.** DRAWBRIDGE rewrites every call's codec list to G.711 only. If your phone is set to prefer Opus, G.722, or G.729, the negotiation will fail and you will hear nothing. Disable those codecs on every phone before placing a call.

Once registered, the phone's extension appears on the hub's client list. You can also confirm via the TUI: press `3` (PBX CONFIG), Extensions tab, and the live registered roster is shown.

For per-phone configuration steps (Yealink, Grandstream, Cisco, Polycom, Linphone, Zoiper, MicroSIP), see [PHONE_COMPATIBILITY.md](PHONE_COMPATIBILITY.md).

---

## Step 5 — Test with the echo extension

From the registered phone, dial **`777`**.

You should hear your own voice played back after a short delay. This confirms:
- The phone is registered
- SIP signaling is working
- RTP audio (codec, UDP path) is working

If you hear nothing, or the call does not connect, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

## Step 6 — Make your first extension-to-extension call

Register a second phone with extension `1002` (or any other number not in use).

From the first phone (1001), dial `1002`. The second phone rings. Answer it and verify two-way audio.

Note that **RTP audio flows directly between the two phones** — the DRAWBRIDGE unit only handles SIP signaling. Both phones must be able to reach each other's IP on UDP. On a single flat LAN this is always the case.

---

## Reserved extensions

Do not assign these as phone extensions:

| Extension | Purpose |
| :--- | :--- |
| `777` | Echo test — plays your audio back |
| `999` | All-page broadcast — calls every registered extension simultaneously |
| `700`–`799` | Call park orbits |

---

## Next steps

- **Full TUI reference:** [SYSOP_MANUAL.md](SYSOP_MANUAL.md) — every screen, every key, every option
- **Detailed first-time setup:** [SETUP_GUIDE.md](SETUP_GUIDE.md) — registrar modes, security hardening, site handoff checklist
- **Fleet and operator tasks:** [OPERATOR_ADMIN.md](OPERATOR_ADMIN.md) — ring groups, CDR, OTA updates, factory reset
- **Per-phone configuration:** [EXTENSION_SETUP.md](EXTENSION_SETUP.md) and [PHONE_COMPATIBILITY.md](PHONE_COMPATIBILITY.md)
- **Capacity and scaling:** [SCALING.md](SCALING.md)
- **Security posture:** [THREAT_MODEL.md](THREAT_MODEL.md)
- **Troubleshooting:** [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — registration failures, one-way audio, call drops

---

## Quick-start checklist

Use this to track your progress on-site.

- [ ] DRAWBRIDGE unit powered via PoE — Ethernet cable seated, switch port is 802.3af
- [ ] SSH reachable at `drawbridge.local` or `<ip>` on port 22
- [ ] First-run screen appeared in the TUI
- [ ] Admin PIN set (step [1] on first-run screen) — PIN written down and stored securely
- [ ] Network IP confirmed (step [2] on first-run screen)
- [ ] First phone configured: SIP server = device IP, port 5060, UDP, G.711 only, STUN/ICE off
- [ ] First phone shows as registered (hub client list or PBX CONFIG → Extensions)
- [ ] Echo test passed: dialed 777, heard own voice back
- [ ] Second phone configured and registered (extension 1002 or similar)
- [ ] Extension-to-extension call completed: 1001 → 1002, two-way audio confirmed
