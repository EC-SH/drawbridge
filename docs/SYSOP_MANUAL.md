# DRAWBRIDGE Sysop Manual

**Audience:** Field technicians and on-site administrators  
**Product:** DRAWBRIDGE — self-contained SIP PBX on a single PoE Ethernet board  
**Interface:** SSH TUI (primary configuration surface)

---

## Contents

1. [What DRAWBRIDGE Is](#1-what-drawbridge-is)
2. [Connecting via SSH](#2-connecting-via-ssh)
3. [Privilege Model](#3-privilege-model)
4. [TUI Layout](#4-tui-layout)
5. [Quick Reference — Global Keys](#5-quick-reference--global-keys)
6. [Status Chip Lexicon](#6-status-chip-lexicon)
7. [Screen Reference](#7-screen-reference)
   - [MONITOR — Live Call Wallboard](#71-monitor--live-call-wallboard)
   - [NETWORK](#72-network)
   - [PBX CONFIG](#73-pbx-config)
   - [SECURITY](#74-security)
   - [REPORTS](#75-reports)
   - [ABOUT](#76-about)
8. [First-Run Setup](#8-first-run-setup)
9. [PIN Management](#9-pin-management)
10. [Registrar Modes and Device Adoption](#10-registrar-modes-and-device-adoption)
11. [Star Codes](#11-star-codes)
12. [Firmware Updates (OTA)](#12-firmware-updates-ota)
13. [Security Considerations](#13-security-considerations)
14. [Troubleshooting](#14-troubleshooting)

---

## 1. What DRAWBRIDGE Is

DRAWBRIDGE is a self-contained SIP PBX that runs on a single ESP32-S3 board with PoE Ethernet (LilyGO T-ETH-ELITE). Plug in one Ethernet cable and it is a functioning PBX. No router, no external SIP trunk, and no server are required for internal extension-to-extension calls.

**What the device does:**

- Registers SIP desk phones and softphones as extensions
- Routes calls between registered extensions (peer-to-peer RTP audio)
- Provides PBX features: call hold, call park, ring groups, paging, BLF, call forwarding, DND, call park, and CDR
- Optionally connects outbound PSTN calls through a configured WAN trunk

**The SSH TUI is the primary configuration interface.** There is no physical screen or web UI configuration panel. All PBX configuration is done through the terminal.

---

## 2. Connecting via SSH

**Prerequisites:** Any SSH client (OpenSSH terminal, PuTTY, or similar).

**Connection details:**

| Parameter | Value |
|-----------|-------|
| Host | `drawbridge.local` or the device's DHCP IP address |
| Port | 22 |
| Username | `sysop` |
| Password | Admin PIN (once set); no password required until a PIN is provisioned |

**From a terminal:**

```
ssh sysop@drawbridge.local
```

**From PuTTY:** Enter `drawbridge.local` as the host name, port 22, and connection type SSH.

**Terminal requirements:** 80 columns × 24 rows minimum. The TUI scales up automatically with a larger terminal (up to 132 × 50). Use a terminal emulator with UTF-8 support and a full color palette for the best experience. Older terminals or `TERM=dumb` clients fall back to ASCII box-drawing and no color automatically.

**Name resolution:** `drawbridge.local` requires mDNS (Bonjour/Avahi) support on the client machine. If name resolution fails, find the device's IP address from your DHCP server and connect by IP.

---

## 3. Privilege Model

DRAWBRIDGE has a single authenticated role: **sysop**.

| Username | Access level |
|----------|--------------|
| `sysop` | Full TUI access — all configuration screens, PIN change, SSH toggle, factory reset |

Authentication is the admin PIN. Until a PIN is set (first run), the SSH session opens without a password. Once a PIN is provisioned, every new SSH connection requires it.

**There is no separate owner / recovery username today.** All admin operations, including factory reset, are available to the `sysop` session. Destructive operations (factory reset, forget device) require an explicit in-screen confirmation before they execute.

---

## 4. TUI Layout

The TUI is an 80×24 ANSI terminal with a fixed three-zone spine. The body region scales if your terminal is larger.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Row 1   TITLE BAR — hostname · IP · uptime clock · live stats              │
├─────────────────────────────────────────────────────────────────────────────┤
│ Row 4   ─────────────────────────────────────────────────────────────────── │
│         BODY — active screen content (18 rows at 80×24)                    │
│ Row 21  ─────────────────────────────────────────────────────────────────── │
├─────────────────────────────────────────────────────────────────────────────┤
│ Row 24  FOOTER — context-sensitive key hints                               │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Title bar (row 1):** Displays the device hostname, IP address, and an uptime clock (HH:MM:SS of uptime since last boot — not wall clock time). Live statistics update approximately once per second.

**Footer (row 24):** Key hints change per screen. Always shows the keys that work on the current screen.

**Themes:** Two color themes are available. Press `T` from any screen to toggle.

| Theme | Palette |
|-------|---------|
| BRASS (default) | Amber text on dark background |
| PHOSPHOR | Green text on dark background |

---

## 5. Quick Reference — Global Keys

These keys work from any screen.

| Key | Action |
|-----|--------|
| `1` | Go to MONITOR |
| `2` | Go to NETWORK |
| `3` | Go to PBX CONFIG |
| `4` | Go to SECURITY |
| `5` | Go to REPORTS |
| `6` | Go to ABOUT |
| `T` | Toggle theme (BRASS ↔ PHOSPHOR) |
| `?` | Context help for the current screen |
| `L` | Logout |
| `Esc` | Cancel modal / return to previous screen |

---

## 6. Status Chip Lexicon

Every status indicator in the TUI uses a glyph plus a text label plus color. Color is never used alone, so the screen is readable on monochrome terminals and by operators who have color vision differences.

| Chip | Meaning |
|------|---------|
| `● ONLINE` (green) | Extension registered and reachable |
| `○ UNREACH` (dim) | Extension known but not currently registered |
| `◐ RINGING` (amber) | A call is currently ringing this extension |
| `◆ ACTIVE` (bright) | Call in progress on this channel |
| `⊘ DND` (red) | Do Not Disturb is enabled |
| `◆ SECURED` (amber) | Device has MAC lock and SIP digest authentication enforced |
| `◐ LEARNED` (dim) | Device MAC was learned in Learn mode; not yet promoted to Secured |
| `· none` | No credential or no state (unset field) |
| `● SET` (green) | A value is provisioned (admin PIN, trunk secret) |
| `○ none` (dim) | A value is not yet provisioned |
| `● CONNECTED` (green) | Trunk is connected to the upstream call-control endpoint |
| `○ DISCONNECTED` (dim) | Trunk is not connected |

---

## 7. Screen Reference

### 7.1 MONITOR — Live Call Wallboard

**Key:** `1` from anywhere  
**Purpose:** Real-time view of all active calls and registered extensions.

This is the landing screen after login.

#### Call Matrix

Up to 8 channel rows. Each row shows:

| Column | Description |
|--------|-------------|
| CH | Channel number (1–8) |
| EXT | Calling extension |
| DEST | Call destination, prefixed with `→` |
| DUR | Call duration (MM:SS) |
| CODEC | Audio codec (PCMU = G.711 µ-law) |
| STATE | Status chip: `◆ ACTIVE`, `◐ RINGING`, `○ IDLE` |

Idle channel slots show empty EXT and DEST fields with `○ IDLE`.

#### Extension Roster

Lists all registered and known extensions with their current state chip (see Section 6).

#### Vitals Bar

Bottom of the monitor body:

| Indicator | Meaning |
|-----------|---------|
| MEM | Heap memory used % |
| CALLS | Active calls / maximum (e.g. 2/8) |
| UP | Uptime HH:MM:SS |

#### Keys on MONITOR

| Key | Action |
|-----|--------|
| `F` | Freeze / unfreeze live refresh (useful for reading the screen or capturing state) |
| `1`–`6`, `T`, `?`, `L` | Global keys |

The monitor refreshes at approximately 1 Hz. Press `F` to pause updates; press `F` again to resume.

---

### 7.2 NETWORK

**Key:** `2` from anywhere  
**Purpose:** View network configuration and switch WiFi mode.

Displays:

| Field | Description |
|-------|-------------|
| Interface | `eth0` (Ethernet) or `wlan0` (WiFi) |
| IP address | Current IP |
| MAC address | Device MAC |
| Mode | DHCP or STATIC |
| SSID | Joined or hosted network name (WiFi builds only) |
| Uptime | Network interface uptime |

#### Keys on NETWORK

| Key | Action |
|-----|--------|
| `M` | Guarded WiFi mode switch (AP ↔ Station) — requires confirmation; device reboots |

The `[M]` mode switch presents a confirmation screen before taking effect. A reboot is required for the change to apply.

---

### 7.3 PBX CONFIG

**Key:** `3` from anywhere  
**Purpose:** Configure extensions, ring groups, call forwarding, IVR, features, and the PSTN trunk.

PBX CONFIG is a tabbed panel. Navigate tabs with `Tab` or the left/right arrow keys.

| Tab | Purpose |
|-----|---------|
| Extensions | View registered extensions and manage credentials |
| Ring Groups | Create and manage ring/hunt groups |
| Forwards/DND | Per-extension call forwarding and Do Not Disturb |
| IVR | Digit routing (pending — shown as stub) |
| Features | System-wide feature settings |
| TRUNK | PSTN trunk configuration |

Use `↑`/`↓` to navigate rows within a tab. `Enter` opens an editor for the selected item.

---

#### Extensions Tab

Lists all currently-registered extensions with state, DND status, and credential status.

**What appears here:** Extensions are shown when SIP phones register with the device. The list reflects live registrations. An extension disappears from the list if its phone goes offline and re-registration lapses.

| Column | Description |
|--------|-------------|
| EXT | Extension number |
| STATE | `● ONLINE` / `○ UNREACH` |
| DND | `⊘ DND` if enabled, blank if not |
| CRED | `◆ SECURED` if SIP digest credentials are assigned; `· none` if not |

**Keys on Extensions tab:**

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate extension list |
| `S` | Assign or rotate a SIP digest secret for the selected extension |
| `Space` | Toggle DND on the selected extension |
| `Enter` | Open the Forwards/DND editor for the selected extension |

**Note on adding extensions:** DRAWBRIDGE's registrar is open by default — any phone that knows an extension number can register. Phones appear in this list when they register. To pre-provision extension credentials or restrict registration to known devices, use the Registrar mode and device adoption workflow described in Section 10.

---

#### Ring Groups Tab

Lists ring and hunt groups.

| Column | Description |
|--------|-------------|
| NAME | Group name / number |
| MODE | Ring All or One at a time (Hunt) |
| MEMBERS | Number of member extensions |

**Ring All:** Forks the incoming call to all member extensions simultaneously. The first to answer wins.

**Hunt (One at a time):** Tries each member extension in the configured order. Moves to the next extension on no-answer.

**Keys on Ring Groups tab:**

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate group list |
| `A` | Create a new ring group |
| `E` | Edit the selected group (members and mode) |
| `D` | Delete the selected group (requires confirmation) |

**Creating or editing a group:**

1. Press `A` (create) or `E` (edit) to open the group editor.
2. Enter a group name.
3. Select mode: Ring All or One at a time.
4. Check members from the extension roster using `Space`.
5. For Hunt mode, the pick order determines the sequence.
6. Press `Enter` on the Apply button to save, or `Esc` to cancel.

A group must have at least one member to be saved. If a member extension goes offline, the group shows a warning count of members that are not reachable.

---

#### Forwards/DND Tab

Shows per-extension forward settings.

| Column | Description |
|--------|-------------|
| EXT | Extension number |
| STATE | Registration state |
| DND | DND status chip |
| CFU | Call Forward Unconditional target (blank if unset) |
| CFB | Call Forward Busy target (blank if unset) |
| CFNA | Call Forward No Answer target (blank if unset) |

**Keys on Forwards/DND tab:**

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate extension list |
| `Space` | Toggle DND for the selected extension |
| `Enter` | Open the forward editor for the selected extension |

**Forward editor:**

The editor shows DND toggle and three forward targets: CFU, CFB, and CFNA.

| Forward type | When it applies |
|-------------|-----------------|
| CFU (Call Forward Unconditional) | All incoming calls, regardless of state |
| CFB (Call Forward Busy) | Incoming calls when the extension is already on a call |
| CFNA (Call Forward No Answer) | Incoming calls that are not answered within the no-answer timeout |

Forward targets are extension numbers. Leaving a target blank clears that forward. The target picker shows registered extensions and their current state. Ring groups cannot be used as forward targets at this time.

---

#### IVR Tab

IVR (Interactive Voice Response) digit routing. This tab is present but the IVR backend is not yet implemented. Configuration shown here is for layout reference only and will have no effect until IVR is available.

---

#### Features Tab

System-wide PBX feature configuration. Content varies with firmware version.

---

#### TRUNK Tab

PSTN trunk configuration. The trunk connects DRAWBRIDGE to an upstream call-control endpoint for outbound calls over the public phone network.

Displays:

| Field | Description |
|-------|-------------|
| Base URL | The call-control API endpoint |
| Client ID | API client identifier |
| Source DN | The directory number the device originates calls as |
| Secret | `◆ SET` if a client secret is stored; `· none` if not |
| Mode | Live or Loopback (loopback = no actual PSTN calls, for testing) |
| State | `● CONNECTED` / `○ DISCONNECTED` |

**Keys on TRUNK tab:**

| Key | Action |
|-----|--------|
| `E` | Open the trunk editor |

**Trunk editor:**

The editor has five fields:

| Field | Notes |
|-------|-------|
| Base URL | Full HTTPS base URL of the call-control endpoint |
| Client ID | API client identifier string |
| Secret | Never echoed (shows bullets only). Leave blank to keep the existing stored secret. |
| Source DN | Originating directory number |
| Mode | Toggle between Live (active PSTN) and Loopback (test mode, no PSTN) |

Press `Enter` on Apply to save. Changes take effect on next connection attempt (typically within seconds). A reboot is not required.

**The trunk secret is never echoed.** Once set, it is stored on the device and shown only as `◆ SET`. To replace it, enter a new value in the Secret field. To leave it unchanged, leave the Secret field empty.

---

### 7.4 SECURITY

**Key:** `4` from anywhere  
**Purpose:** Admin PIN management, SSH access control, and device registrar management.

Displays:

| Field | Description |
|-------|-------------|
| Admin PIN | `● SET` if provisioned; `○ none` if not |
| SSH Access | Enabled or disabled |
| Session | Current login: username, peer IP, session start time (uptime-based) |

**Keys on SECURITY:**

| Key | Action |
|-----|--------|
| `P` | Change admin PIN |
| `K` | Toggle SSH access (enable / disable) |
| `D` | Open REGISTRAR · DEVICES screen |
| `X` | Factory reset (requires two-step confirmation) |

**Disabling SSH access ([K]):** Disabling SSH closes the port and logs out any active session. To re-enable SSH without physical access, use the web dashboard escape hatch: from a browser (or curl) on the LAN, log in to the dashboard and issue `POST /api/ssh/enable` (admin-session-gated, same-origin):

```bash
curl -X POST -H "Cookie: pd_session=<token>" http://<device-ip>/api/ssh/enable
```

The setting persists across reboots. If the dashboard is also unreachable, physical serial access or a provisioning reset remains the fallback.

**Factory reset ([X]):** Erases all NVS configuration: admin PIN, WiFi credentials, trunk credentials, registered extension secrets, and device adoption records. The device reboots into first-run state. This requires two separate confirmations (type `Y` twice) to prevent accidental execution. There is no undo.

---

#### [P] Change PIN Modal

Three fields: Current PIN, New PIN, Confirm New PIN.

- PIN characters are never echoed (displayed as bullets).
- Minimum 4 characters; 6 or more alphanumeric characters recommended.
- If the Current PIN field is wrong, the modal reports an error and does not apply the change.
- If New PIN and Confirm New PIN do not match, the change is rejected.

---

#### [4]/[D] REGISTRAR · DEVICES

Shows the current registrar admission mode and the list of adopted devices.

**Registrar mode:**

| Mode | Behavior |
|------|----------|
| Open | No SIP authentication. Any phone that knows an extension number can register. Default mode. |
| Learn | Trust-on-first-use adoption. Unknown devices are accepted on first REGISTER and recorded by MAC address. Already-secured devices continue to require digest auth. Use only during a controlled adoption window. |
| Secure | Every REGISTER is SIP digest-challenged. Only extensions with assigned secrets and matching adopted MACs can register. Target production mode. |

**Device roster columns:**

| Column | Description |
|--------|-------------|
| MAC | Device MAC address (12 hex characters) |
| EXT | Extension number this device registered as |
| STATE | `● ONLINE` / `◐ LEARNED` / `◆ SECURED` |

**Keys on REGISTRAR · DEVICES:**

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate device list |
| `M` | Change registrar mode (Open / Learn / Secure) |
| `A` | Assign or rotate a SIP secret for the selected device's extension |
| `S` | Promote the selected device to Secured (enables MAC lock + digest enforcement) |
| `F` | Forget the selected device (removes adoption record — guarded confirm) |
| `Esc` | Return to SECURITY |

For the full device adoption workflow, see Section 10.

---

### 7.5 REPORTS

**Key:** `5` from anywhere  
**Purpose:** Call detail records and event log.

Two views, toggled with `Tab`:

**Recent Calls (default):** CDR ring showing the most recent calls, newest first.

| Column | Description |
|--------|-------------|
| CALLER | Originating extension |
| CALLEE | Destination extension or group |
| DUR | Call duration (0:00 if never connected) |
| RESULT | `● ANSWERED` / `○ BUSY` / `◯ CANCELLED` / `✕ FAILED` |

**Event Log:** Tail of the system event log.

**Keys on REPORTS:**

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate CDR rows |
| `Enter` | Open CDR detail modal for the selected call |
| `Tab` | Switch between Recent Calls and Event Log |

**CDR detail modal:** Shows full call information — caller, callee, codec (PCMU), Call-ID, start time (uptime-based HH:MM:SS), and duration. Press any key or `Esc` to close.

**Note on timestamps:** DRAWBRIDGE has no real-time clock. All times displayed are relative to uptime (HH:MM:SS since last boot). Timestamps reset after a reboot.

---

### 7.6 ABOUT

**Key:** `6` from anywhere  
**Purpose:** System information. All values are read-only.

Displays:

| Field | Description |
|-------|-------------|
| Firmware version | Build version string |
| Build date | Firmware build date |
| Hostname | Configured hostname |
| IP address | Current IP |
| MAC address | Device MAC |
| PSRAM | External PSRAM size and usage |
| Heap | Internal heap used % |
| Pool configuration | Call/session pool sizes (max calls, max extensions, etc.) |

---

## 8. First-Run Setup

A device with no admin PIN set routes to the **FirstRun** screen instead of the normal hub after login.

**FirstRun checklist:**

1. **Set admin PIN** — Opens the Change PIN modal. Complete this first. Until a PIN is set, any SSH client that can reach the device can connect without a password.
2. **Configure network** — Links to the NETWORK screen. Verify the device has an IP and can be reached.
3. **Add extensions** — Phones will appear in the Extensions tab when they register. Point phones at the device's IP as their SIP registrar.
4. **Configure PSTN trunk** — Optional. Only required if you need outbound PSTN calls. Links to the TRUNK tab in PBX CONFIG.

Complete each step in order. You can return to the checklist if you need to resume setup. Once a PIN is set, subsequent logins go directly to the MONITOR screen.

---

## 9. PIN Management

| Topic | Detail |
|-------|--------|
| Initial state | No password required until a PIN is set |
| After PIN is set | Every SSH login requires the PIN as password |
| Minimum length | 4 characters |
| Recommended | 6 or more alphanumeric characters |
| Brute-force lockout | 5 failed logins → 60-second lockout (counter resets after lockout clears or on successful login) |
| Forgotten PIN | No recovery path. Factory reset required (Section 7.4 `[X]`). Erases all configuration. |

**Set the PIN immediately on first login.** An unprovisioned device with SSH enabled accepts any connection without authentication.

**Changing the PIN:** Press `P` from the SECURITY screen. Enter the current PIN, the new PIN, and confirm the new PIN. The change takes effect immediately. The next SSH login will require the new PIN.

---

## 10. Registrar Modes and Device Adoption

DRAWBRIDGE supports three registrar modes controlling how SIP phones authenticate to register.

### Mode Summary

| Mode | Registration requirement | Use case |
|------|------------------------|----------|
| **Open** | None — any phone registers freely | Initial setup, isolated test environments |
| **Learn** | Trust-on-first-use — first device to claim an extension is recorded by MAC | Controlled fleet cutover from another PBX |
| **Secure** | SIP digest authentication + MAC lock | Production deployments |

Change the mode from SECURITY → REGISTRAR · DEVICES → `[M]`.

### Moving to Secure: Fleet Cutover Procedure

Use this when migrating phones from an existing PBX or when securing a new deployment.

**Prerequisites:**
- DRAWBRIDGE is on the same network segment (L2) as the phones (required for MAC resolution)
- Admin PIN is set
- You have a list of the expected extensions and their assigned phones

**Step 1 — Set registrar to Learn**

From SECURITY → REGISTRAR · DEVICES, press `[M]` and select Learn. The device is now in the adoption window.

**Step 2 — Let phones register (TOFU adoption)**

As each phone re-registers, DRAWBRIDGE records its MAC address and extension. Each device appears in the roster as `◐ LEARNED`. Phones continue to work on their existing credentials during this step.

If a phone has not re-registered, reboot it or trigger a re-register from the phone's menu to speed up adoption.

**Step 3 — Verify the roster**

Review every row in the device roster. Confirm that each MAC address matches the physical phone you expect for that extension. If an unexpected MAC appears, investigate before proceeding — it may indicate a duplicate device or an unauthorized registration attempt.

If a MAC shows blank, wait one registration cycle (typically 60–300 seconds depending on phone configuration). The MAC resolves from the ARP table, which may lag by one registration cycle on first contact.

**Step 4 — Assign secrets**

For each adopted device, select it in the roster and press `[A]` to assign a SIP digest secret. Enter a strong password. The device stores this credential for digest authentication.

Then type the same secret into the matching phone's SIP account configuration (usually in the phone's web UI under SIP account credentials).

**Step 5 — Promote each device to Secured**

Once a phone has been assigned a secret and is authenticating correctly, select its row and press `[S]` to promote it to Secured. A Secured device shows `◆ SECURED`.

A Secured extension is MAC-locked: only the original MAC address can register as that extension. A different MAC attempting to register the extension is rejected.

**Step 6 — Switch to Secure mode**

When all expected devices show `◆ SECURED` and are registering cleanly, change the registrar mode to Secure using `[M]`. New devices can no longer register without first being adopted.

### Important Limits of MAC-Based Security

The MAC lock is derived from the network ARP table. It prevents accidental conflicts and raises the bar against casual impersonation on a trusted LAN. It is not a cryptographic device identity guarantee. MAC addresses can be spoofed on a hostile network. The most effective additional control is enabling WPA2 on the SoftAP (if using WiFi) and keeping the device on a trusted wired segment.

### Recovering from a MAC Change

If a phone's hardware is replaced or its MAC address changes (some phones randomize MACs), the Secured extension will reject the new MAC. To re-adopt:

1. Press `[F]` on the old device entry to forget it.
2. Temporarily set the registrar back to Learn mode.
3. Allow the phone to re-register (TOFU adoption with the new MAC).
4. Re-assign the secret with `[A]`.
5. Promote to Secured with `[S]`.
6. Return the registrar to Secure mode.

### Resetting a Device Entry

Press `[F]` on any device row to forget it. This removes the MAC↔extension binding and the stored credential. The extension becomes unclaimed. The operation requires confirmation.

---

## 11. Star Codes

Dial these codes from any registered SIP phone. They are single-step (no PIN required).

| Code | Function |
|------|----------|
| `*60` | Enable Do Not Disturb for your extension |
| `*69` | Call back the last caller |
| `*72 <ext>` | Set Call Forward Unconditional to `<ext>` |
| `*73` | Clear Call Forward Unconditional |
| `*80` | Disable Do Not Disturb for your extension |
| `*11` | Echo test — reroutes the active call to the echo loopback (equivalent to dialing 777) |
| `777` | Echo test — hear your own voice (confirms audio path) |
| `999` | All-page broadcast — rings all registered extensions simultaneously |
| `700`–`799` | Call park orbits — park a call on orbit N, retrieve from any phone by dialing N (or `**N`) |
| `98x` | Page zone x — rings the phones assigned to zone x |

**Transferring a call:**

Use your phone's **Transfer** key — no star code needed. DRAWBRIDGE supports both transfer modes via SIP REFER (RFC 3515/3891):

| Mode | Steps |
|------|-------|
| **Blind transfer** | Press Transfer, dial target, press Transfer/Send. Call handed off immediately. |
| **Attended transfer** | Press Transfer, dial target, wait for answer, consult, press Transfer again. The two parties connect directly; you drop off. |

**Parking a call:**

1. While on a call, transfer to any orbit number (700–799).
2. The call is parked. Tell the intended recipient the orbit number.
3. The recipient dials the orbit number — bare (`700`) or `**`-prefixed (`**700`) — from
   their phone to retrieve the call.
4. Unattended parked calls ring back to the parking extension after a timeout. The ring-back
   caller ID identifies the orbit — the phone shows `Orbit 700` / `**700`, so the operator
   can retrieve straight from the call log by dialing `**700`, or answer to connect directly.

---

## 12. Firmware Updates (OTA)

Firmware updates are delivered over the HTTP API, not through the TUI. The device uses a dual-partition (A/B) layout that allows rollback to the previous firmware if the new image fails to boot.

**Update procedure (requires admin PIN and a computer on the same network):**

1. Obtain the firmware image (`.bin` file) from your vendor.
2. Log in through the HTTP API to get a session cookie:
   ```
   curl -c cookies.txt -H "Origin: http://<device-ip>" \
        -X POST --data "pin=<your-pin>" \
        http://<device-ip>/api/admin/login
   ```
3. Upload the new firmware:
   ```
   curl -b cookies.txt -H "Origin: http://<device-ip>" \
        -H "Content-Type: application/octet-stream" \
        -X POST --data-binary @SipServer.bin \
        http://<device-ip>/api/ota/upload
   ```
4. Trigger reboot into the new image:
   ```
   curl -b cookies.txt -H "Origin: http://<device-ip>" \
        -X POST http://<device-ip>/api/ota/reboot
   ```

The device reboots. If the new image boots and operates normally, it is confirmed automatically within a few seconds. If the new image crashes or boot-loops, the bootloader rolls back to the previous firmware on the next reset.

**Check current firmware status:**
```
curl http://<device-ip>/api/ota/status
```

**OTA is not available on the first-generation single-partition layout.** Devices must be migrated to the dual-OTA partition layout via a one-time USB/JTAG flash before OTA updates become available. After migration, re-onboard the device (set the admin PIN and reconfigure WiFi).

---

## 13. Security Considerations

### Set the Admin PIN First

An unprovisioned DRAWBRIDGE with SSH enabled accepts connections from anyone who can reach it on the network without requiring a password. Set the admin PIN as the first action after first login.

### WPA2 on the SoftAP

If your deployment uses the WiFi SoftAP mode, the default configuration is an open (unauthenticated) access point. This means anyone in radio range can join the network and observe SIP signaling and RTP audio in plaintext.

Enabling WPA2 on the SoftAP is the single highest-leverage security improvement for WiFi deployments. It encrypts the radio link, gates who can associate, and protects both call audio and SIP signaling. Contact your vendor or consult the build documentation to enable WPA2 (`WIFI_AUTH_WPA2_PSK`).

### Physical Security

DRAWBRIDGE stores credentials in flash (NVS). Without Secure Boot v2 and flash encryption enabled (advanced build options), physical access to the board allows reading stored credentials. For deployments where physical access is not controlled, consult the build documentation on enabling flash encryption and Secure Boot.

### SIP Digest Auth (Learn and Secure Modes)

In Open registrar mode, any phone on the network can register as any extension. Switch to Secure mode after completing the adoption procedure (Section 10) to require SIP digest authentication.

### SSH Brute Force

After 5 failed PIN attempts, the login gate imposes a 60-second lockout. The lockout clears automatically after 60 seconds or on the next successful login.

### Session Expiry

SSH sessions do not have an automatic idle timeout. Log out with `L` when stepping away from an active session.

---

## 14. Troubleshooting

| Symptom | Likely cause | Action |
|---------|-------------|--------|
| Cannot resolve `drawbridge.local` | mDNS not supported on client | Connect by IP address instead |
| SSH connection refused | SSH disabled in SECURITY, or device not fully booted | Wait 30 seconds and retry; check SECURITY → SSH access state |
| PIN accepted but session drops immediately | Terminal dimensions too small | Resize terminal to at least 80×24 and reconnect |
| Extension does not appear in Extensions tab | Phone is not registered | Check phone SIP server setting points to DRAWBRIDGE IP; check phone status |
| Extension shows `○ UNREACH` | Phone registered but is now offline or re-registration lapsed | Check phone network connectivity and SIP keepalive interval |
| Phone shows `401 Unauthorized` after adopting in Learn mode | Secret assigned in TUI but not yet entered on phone | Enter the matching secret in the phone's SIP account settings |
| Phone shows `403 Forbidden` after MAC change | Extension is Secured; new MAC is rejected | Follow the MAC recovery procedure in Section 10 |
| Unexpected device in REGISTRAR · DEVICES roster | Unknown phone registered during Learn window | Forget the device with `[F]`; investigate who owns the MAC before proceeding |
| Parked call does not ring back | Park orbit 700–799 only; verify the correct orbit was dialed | Check the dialed orbit matches what was given to the parking extension |
| No audio on a call | RTP path problem or codec mismatch | Run the echo test (`777`) to verify local audio; check codec settings on phone (G.711 µ-law / PCMU required) |
| OTA upload returns 401 | Admin PIN is set but session cookie is missing | Log in first to get a session cookie (see Section 12) |
| OTA upload returns 422 | Image file is corrupt or wrong board | Verify the `.bin` file is complete and built for the ESP32-S3 |
| After factory reset, device does not boot | Unrelated; factory reset only erases NVS | Wait for full reboot cycle (15–30 seconds); reconnect SSH |
| TUI shows wrong uptime | Uptime resets on every reboot; there is no RTC | Expected behavior — uptime is relative, not wall clock |
