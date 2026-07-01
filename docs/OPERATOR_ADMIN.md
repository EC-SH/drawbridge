# DRAWBRIDGE — Operator Administration Guide

**Audience:** Operators and managed-service providers managing deployed DRAWBRIDGE units.
Assumes familiarity with VoIP fundamentals. For initial field installation, see [ONBOARDING.md](ONBOARDING.md).

---

## Connecting to the Management Interface

DRAWBRIDGE is managed exclusively over SSH. The unit gets a DHCP address on the LAN and
advertises itself as `drawbridge.local` via mDNS. If mDNS does not resolve, use the IP
address shown in the TUI's [6] ABOUT screen or from your DHCP server's lease table.

```
ssh <user>@drawbridge.local
```

The TUI presents a hub screen on login. Global navigation: press **1–6** from any screen
to jump directly to that section.

| Key | Section |
|-----|---------|
| `1` | SYSTEM MONITOR |
| `2` | NETWORK |
| `3` | PBX CONFIG |
| `4` | SECURITY |
| `5` | REPORTS/LOGS |
| `6` | ABOUT |

### Recovering SSH After It Was Disabled

If SSH was disabled from the TUI's [4] SECURITY screen, re-enable it from the web
dashboard (admin-session-gated, same-origin) — no physical access needed:

```bash
DEVICE=http://drawbridge.local   # or http://<device-ip>
JAR=cookies.txt
curl -s -c "$JAR" -H "Origin: $DEVICE" -X POST --data "pin=YOUR_PIN" "$DEVICE/api/admin/login"
curl -s -b "$JAR" -H "Origin: $DEVICE" -X POST "$DEVICE/api/ssh/enable"
```

The setting persists across reboots; the SSH listener restarts immediately.

---

## Extension Management

### Adding Extensions

Extensions self-register — you do not pre-create them. In Open and Learn registrar modes,
any SIP phone that sends a REGISTER to the device is accepted and appears in the Extensions
list under **[3] PBX CONFIG** while it remains actively registered.

> The **[A]** button in the Extensions tab renders an add-extension form but does not yet
> persist entries. Extensions appear while a phone is registered and disappear when the
> phone goes offline. There is no persistent extension database to populate in advance.

> Reserved extensions: **777** (echo test) and **999** (all-page broadcast) are built-in
> virtual extensions — do not assign them to physical phones.

### Removing Extensions

Extension deletion is not yet implemented in the TUI. To remove a device binding in
Secure mode, **forget** the device via [4] SECURITY → [D] Devices → select the device →
**F** (Forget). This removes the MAC-to-extension binding; the extension itself remains in
the pool.

### Per-Extension DND (Do Not Disturb)

DND can be toggled two ways:

- **From the phone:** dial `*60` to enable, `*80` to disable.
- **From the TUI:** [3] PBX CONFIG → Extensions tab → navigate to the extension →
  **Enter** to open the Forwards/DND editor → toggle DND.

### Call Forwarding

| Code | Function |
|------|----------|
| `*72<ext>` | Enable Call Forward Unconditional (CFU) to `<ext>` |
| `*73` | Disable CFU |

CFB (forward-on-busy) and CFNA (forward-on-no-answer) are configured per extension from
the TUI only: [3] PBX CONFIG → Extensions tab → select extension → **Enter** (Forwards/DND
editor).

To read back the current forward status, check the Forwards/DND editor in the TUI:
[3] PBX CONFIG → Extensions tab → select extension → **Enter**.

### Star Codes Reference

| Code | Function |
|------|----------|
| `*60` | DND on (selective call rejection for the dialing extension) |
| `*80` | DND off |
| `*72<ext>` | Call Forward Unconditional on, to `<ext>` |
| `*73` | Call Forward Unconditional off |
| `*69` | Speak the last caller's extension |
| `*11` | Echo test (line check — equivalent to dialing 777) |

---

## Ring Groups and Hunt Groups

### Creating a Ring Group

1. [3] PBX CONFIG → use **Tab** or the right arrow to navigate to the **Ring Groups** tab.
2. Press **A** to create a new group.
3. Enter a group name and select members from the checklist (Space to check/uncheck).
4. Choose a ring mode: **Ring All** (all members ring simultaneously; first to answer
   connects) or **Hunt** (members ring sequentially in the order they were checked).
5. Save with **Enter** or the Save button.

### Editing a Group

From the Ring Groups tab, navigate to the group with the arrow keys, then press **Enter**
to open the editor. Modify membership or ring mode, then save.

### Deleting a Group

From the Ring Groups tab, navigate to the group and press **D**. A guarded confirm
screen prevents accidental deletion — the safe default is "Keep, go back". Press
**y** to confirm deletion.

---

## Call Transfer

DRAWBRIDGE handles both blind and attended (consultative) transfer via SIP REFER (RFC 3515/3891). Transfer is initiated from the phone using the phone's built-in transfer key — no star code required.

**Blind transfer** — transfer without consulting the target:

1. While on a call, press the phone's **Transfer** key.
2. Dial the target extension.
3. Press **Transfer** again (or **Send**) to complete. The call is immediately handed off.

**Attended (consultative) transfer** — consult the target before completing:

1. While on call with A, press **Transfer**, then dial B.
2. Wait for B to answer. Consult with B while A is on hold.
3. Press **Transfer** again to connect A and B. You drop off; A and B are spliced directly.

The server sends `202 Accepted` on the REFER, then issues re-INVITEs to cross-connect the two parties (REFER+Replaces, RFC 3891). Transfer works with any phone that sends a REFER — consult [PHONE_COMPATIBILITY.md](PHONE_COMPATIBILITY.md) for phone-specific transfer key names.

> Transferring to a park orbit (700–799) parks the call instead of connecting a new party — see [Call Park](#call-park) below.

---

## Call Park

Park orbits are numbered **700–799**.

- **To park:** while on a call, transfer (blind or attended) to any orbit in the 700–799
  range (e.g., transfer to 700).
- **To retrieve:** any phone dials the orbit number the call was parked on.
- **Timeout:** if a parked call is not retrieved within the park timeout, it rings back to
  the extension that originally parked it.

No TUI configuration is required — park orbits are always active.

---

## Page Zones

DRAWBRIDGE reserves dial range **980–989** for scoped paging zones. Dialing a configured
zone extension forks an intercom (auto-answer) INVITE to every member in that zone,
using the same broadcast machinery as the 999 all-page. An unconfigured 98x extension
results in a 404.

> Zone configuration is not yet exposed in the SSH TUI. The engine supports up to ten
> zones (980–989) and persists membership to NVS, but the management interface for
> creating and editing zones has not shipped. Use the 999 all-page broadcast in the
> interim, or contact support for scripted provisioning.

---

## Registrar Security Modes

DRAWBRIDGE supports three registrar modes. The current mode is shown on the Devices screen
and can be changed at any time by an admin.

| Mode | Behavior | When to use |
|------|----------|-------------|
| **Open** | No authentication. Any phone that knows an extension can register. | Trusted isolated LAN only. Not for production on a shared link. |
| **Learn** | First REGISTER from an unknown MAC is accepted without credentials (trust-on-first-use). The MAC is recorded and bound to the extension. Already-secured devices are still digest-challenged. | Fleet cutover only. Open briefly, then move to Secure. |
| **Secure** | Every REGISTER is digest-challenged (RFC 2617, MD5). Only extensions with a set SIP secret and a matching adopted MAC can register. | Steady-state production. |

### Changing the Registrar Mode

[4] SECURITY → [D] Devices → **M** (mode chooser) → select mode → **Enter**.

Mode changes are logged. Moving from Secure to Open or Learn is an explicit admin action;
there is no silent downgrade.

### Setting a Per-Extension SIP Secret

Two paths:

- [3] PBX CONFIG → Extensions tab → select extension → **S** (assign/rotate secret)
- [4] SECURITY → [D] Devices → select device row → **A** (assign secret)

Secrets are stored as the digest HA1 (MD5 of `extension:realm:secret`). The plain-text
secret is never stored or displayed after entry.

### Promoting a Learned Device to Secured

After a device has been adopted in Learn mode and a SIP secret has been assigned to its
extension, navigate to [4] SECURITY → [D] Devices, select the device row, and press **S**.
The device is now locked to its adopted MAC; a different MAC claiming the same extension
will be rejected.

### Forgetting a Device

To remove a MAC-to-extension binding (e.g., hardware replacement, rogue device):
[4] SECURITY → [D] Devices → select device → **F** → confirm.

After forgetting, the extension can be re-adopted in Learn mode or remain unclaimed.

For the full Learn-mode cutover procedure, see [LEARN_MODE.md](LEARN_MODE.md).

---

## OTA Firmware Updates

OTA updates are performed via the HTTP API. The TUI does not currently include an OTA
workflow.

### Prerequisites

- Admin PIN set and a valid session cookie (`pd_session`).
- The update image: `build/SipServer.bin` (built with the same `SIP_TRANSPORT` as the
  running firmware).
- Network access to the device on its LAN IP.

### Update Procedure

```bash
DEVICE=http://drawbridge.local   # or http://<device-ip>
JAR=cookies.txt

# 1. Authenticate
curl -s -c "$JAR" \
     -H "Origin: $DEVICE" \
     -X POST --data "pin=YOUR_PIN" \
     "$DEVICE/api/admin/login"

# 2. Check current OTA state (optional)
curl -s "$DEVICE/api/ota/status"

# 3. Upload the new image
curl -s -b "$JAR" \
     -H "Origin: $DEVICE" \
     -H "Content-Type: application/octet-stream" \
     -X POST --data-binary @build/SipServer.bin \
     "$DEVICE/api/ota/upload"

# 4. Reboot into the new image
curl -s -b "$JAR" \
     -H "Origin: $DEVICE" \
     -X POST "$DEVICE/api/ota/reboot"
```

### Anti-Rollback

The device uses a dual-partition A/B scheme. After an OTA reboot, the new image must
reach a healthy running state before it marks itself valid. If the new image crashes or
boot-loops before reaching steady state, the bootloader automatically restores the
previous image on the next reset. The device cannot be bricked by a bad OTA image.

### OTA Status Codes

| HTTP code | Meaning |
|-----------|---------|
| `200` | Image staged; POST `/api/ota/reboot` to activate. |
| `401` | PIN provisioned but no/invalid session cookie. |
| `403` | Cross-origin request rejected (CSRF guard). |
| `411` | Missing `Content-Length`. |
| `400` | Upload truncated or flash write failed. |
| `422` | Image corrupt, bad magic, or signature verification failed. |
| `500` | Flash initialization failed. |

For full OTA documentation including image signing (issue #47), see [OTA.md](OTA.md).

---

## Factory Reset

[4] SECURITY → **X** → two sequential confirmations required.

Factory reset clears: admin PIN, SIP secrets, all NVS configuration, trunk credentials,
and registrar device bindings. The device reboots into first-run state.

**There is no recovery from a factory reset.** All configuration is lost. A lost PIN also
requires a factory reset — PINs are not recoverable.

---

## CDR (Call Detail Records)

### Viewing CDRs

- **TUI:** [5] REPORTS/LOGS → Recent Calls (the default tab on entry).
- **HTTP API:** `GET /api/cdr`

CDRs are held in an in-memory ring buffer (32 records by default). The buffer wraps
oldest-first. Reboots clear all CDR data.

### API Response Format

`GET /api/cdr` returns a JSON array, newest call first. No authentication required.

```json
[
  {
    "caller": "1001",
    "callee": "1002",
    "startMs": 1234567890123,
    "ageSec": 142,
    "duration": 67,
    "result": "answered"
  }
]
```

| Field | Type | Description |
|-------|------|-------------|
| `caller` | string | Calling extension |
| `callee` | string | Called extension |
| `startMs` | integer | Steady-clock epoch (ms) when the call started — same basis as device uptime, not wall clock |
| `ageSec` | integer | Seconds since the call started |
| `duration` | integer | Talk time in seconds (0 if the call never connected) |
| `result` | string | `answered`, `busy`, `cancelled`, `unavailable`, or `failed` |

---

## Fleet Management

Each DRAWBRIDGE unit is independent with no centralized management plane.

### Identifying Units

Each unit is identified by:
- mDNS hostname: `drawbridge.local` (unique on its local subnet)
- MAC address: visible in [6] ABOUT and in `GET /api/status` → `telemetry`
- LAN IP: shown in [2] NETWORK and [6] ABOUT

Record each unit's LAN IP and MAC at provisioning time. For multi-site deployments, label
the device's physical port with its MAC.

### Scripted Fleet OTA

The HTTP API supports scripted updates. Iterate over your fleet IP list, authenticate,
upload, and reboot each unit sequentially. Use `GET /api/ota/status` to confirm each
device's running partition before and after the update.

```bash
# Confirm post-reboot slot
curl -s "http://<device-ip>/api/ota/status" | python3 -c \
  "import sys,json; d=json.load(sys.stdin); print(d['running'], d['pendingVerify'])"
```

Wait for `pendingVerify: false` before updating the next unit; `true` means the new image
has not yet confirmed a healthy boot.

### Headless NVS Provisioning

For zero-touch deployment of a new unit, use the provisioning script to inject NVS
credentials before first boot:

```bash
python3 .smoke/gen_provision_nvs.py --help
```

See PROVISIONING.md for the full provisioning workflow.

---

## See Also

- [ONBOARDING.md](ONBOARDING.md) — first-boot setup and admin PIN provisioning
- [LEARN_MODE.md](LEARN_MODE.md) — fleet cutover runbook (Open → Learn → Secure)
- [OTA.md](OTA.md) — OTA partition layout, signing, and rollback
- [THREAT_MODEL.md](THREAT_MODEL.md) — security posture and threat analysis
- [API.md](API.md) — full HTTP API reference
