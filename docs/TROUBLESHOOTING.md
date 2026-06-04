# Troubleshooting Runbook

Symptom → likely cause → fix. Work top-down within each section. Where a fact comes from
the firmware or another doc, it is cited so you can verify.

Quick references: [SETUP_GUIDE.md](SETUP_GUIDE.md) ·
[PHONE_COMPATIBILITY.md](PHONE_COMPATIBILITY.md) · [HARDWARE_SELECTION.md](HARDWARE_SELECTION.md) ·
[API.md](API.md) · [OTA.md](OTA.md) · [THREAT_MODEL.md](THREAT_MODEL.md)

---

## Access-point not visible

**Symptom:** The `esp32-sipserver` Wi-Fi network does not appear in your client's Wi-Fi list.

| Cause | Fix |
| :--- | :--- |
| Device not powered / still booting | Confirm power (USB-C or PoE). Watch the serial monitor (`idf.py monitor`) for `wifi_init_softap finished. SSID:esp32-sipserver` (`main/esp_main.cpp`). |
| This is a **wired Ethernet** build | `SIP_TRANSPORT=eth` boards have **no SoftAP** — they join your wired LAN. Reach the dashboard at the device's LAN IP / `pocketdial.local` (see [HARDWARE_SELECTION.md](HARDWARE_SELECTION.md)). |
| Device is in **Station mode** | If Wi-Fi was configured to join an existing network (`/api/wifi/connect`), it is a client, not an AP. Factory-reset to return to AP/onboarding (see [Forgot the admin PIN](#forgot-the-admin-pin)). |
| Display build sitting in onboarding | The display variant's onboarding AP is **`My-Ap`**, not `esp32-sipserver` (`ONBOARDING_SSID`, `main/esp_main_display.cpp`). Look for `My-Ap`. |
| 2.4 GHz only | The ESP32 SoftAP is 2.4 GHz, channel 1. Ensure your client shows 2.4 GHz networks. |

---

## Captive portal won't open

**Symptom:** You joined `My-Ap` (display build) but no setup page appears.

| Cause | Fix |
| :--- | :--- |
| OS captive-portal prompt missed | Manually browse to `http://192.168.4.1/`. The device returns a `302` redirect to the portal for any off-host request ([API.md §3](API.md)). |
| The 5-minute decay window elapsed | The portal has a **300 s decay watchdog** (`CAPTIVE_DECAY_SECONDS`, `main/esp_main_display.cpp`): with no confirmed config it reboots into Standalone AP. Re-join **`esp32-sipserver`** and use `http://192.168.4.1`. |
| Browser cached HTTPS / HSTS | Use a fresh `http://` URL (not `https://`), or try a private window. The dashboard is HTTP only. |
| DNS not redirecting | The onboarding DNS responder answers all names with the device IP (port 53). If your client uses DNS-over-HTTPS, type the IP `192.168.4.1` directly. |

---

## Phone won't register (timeout or 401)

**Symptom:** The SIP client never reaches "registered", times out, or shows an error.

| Cause | Fix |
| :--- | :--- |
| Wrong server/port/transport | Server `192.168.4.1`, port `5060`, transport **UDP** (the engine is UDP-only). See [PHONE_COMPATIBILITY.md](PHONE_COMPATIBILITY.md). |
| Not on the device's network | Confirm the phone has a `192.168.4.x` lease (SoftAP) or can reach the device's LAN IP (wired). |
| TCP/TLS selected | Switch the client to **UDP**. There is no TCP/TLS listener. |
| Using extension `777`/`999` | These are reserved virtual extensions; pick another (e.g. `1001`). |
| `503 Service Unavailable` on REGISTER | The client pool is full. `allocateClient()` evicts the oldest *expired* binding, else returns `503`; the phone retries on its refresh timer. Raise the tier ([SCALING.md §4](SCALING.md)). |
| Client pruned after registering | The registrar prunes a client after ~15 s of silence if it ignores the `OPTIONS` keepalive sent every 5 s (`RequestsHandler.cpp`). Enable the phone's keep-alive / answer-OPTIONS option. |
| Rate-limited (packets dropped) | The SIP UDP path uses a per-source-IP token bucket (burst 40, 20 pkt/s sustained). A flooding or misconfigured client gets packets dropped; watch `packetsDropped` on `/api/status` ([ARCHITECTURE.md §5](ARCHITECTURE.md)). |

> [!NOTE]
> A `401` here is **not** SIP auth — pocket-dial has **no SIP digest authentication**
> today (any extension registers on the open link, see [THREAT_MODEL.md](THREAT_MODEL.md)
> S-3). A `401` you see is almost always the phone's own account dialog or, separately,
> the **HTTP admin** gate (`/api/admin/*`) — a different subsystem.

---

## One-way or no audio

**Symptom:** The call connects (rings, answers) but you hear nothing, or only one side
hears audio.

| Cause | Fix |
| :--- | :--- |
| **Codec mismatch (most common)** | pocket-dial does not transcode; it rewrites SDP to `0 8 101` (`enforceG711()`). If a phone offers only Opus/G.722/G.729, there is no common codec → no audio. **Restrict every client to G.711 µ-law + a-law** ([PHONE_COMPATIBILITY.md §1](PHONE_COMPATIBILITY.md)). |
| NAT/STUN/ICE enabled on the phone | Media is **peer-to-peer on one L2 segment**. NAT traversal rewrites the SDP connection address and breaks direct RTP. Turn STUN/ICE/rport **off**. |
| Client isolation on the AP | RTP is phone-to-phone; if SoftAP client isolation were enabled, stations couldn't reach each other and audio would fail (see [PROVISIONING.md §4.4](PROVISIONING.md) caveat). |
| Firewall between phones (wired) | On a wired LAN, ensure the segment allows station-to-station UDP for the RTP port range. |
| Verify with `777` | Dial **`777`** (echo): if echo works but a two-party call does not, the problem is between the two phones (NAT/isolation/firewall), not the codec. If `777` itself is silent, it is the codec/RTP on that one phone. |

---

## Calls drop unexpectedly

**Symptom:** Established calls hang up on their own.

| Cause | Fix |
| :--- | :--- |
| Keepalive prune | A phone that stops answering `OPTIONS` is pruned after ~15 s (`RequestsHandler.cpp`); its calls end. Keep the phone's keep-alive on and Wi-Fi signal adequate. |
| Wi-Fi association lost | On a SoftAP node, a weak link drops the station. Check RSSI / reduce range. |
| Session pool exhausted | New INVITEs get `503` when the session pool is full; existing calls are untouched. Raise the tier ([SCALING.md §4](SCALING.md)). |
| Admin force-disconnect | `POST /api/kill` de-registers an extension and tears down its calls ([API.md](API.md)). Check whether someone used the dashboard's kill control. |
| Spoofed BYE (open AP) | With no SIP auth on an open AP, a peer can inject a `BYE` (`THREAT_MODEL.md` D-2). The link-layer fix is WPA2 on the SoftAP. |

---

## Dashboard unreachable

**Symptom:** `http://192.168.4.1` (or `pocketdial.local`) does not load.

| Cause | Fix |
| :--- | :--- |
| Wrong scheme | Use **`http://`**, not `https://` — the dashboard is plain HTTP ([API.md §1](API.md)). |
| mDNS not resolving | Browse to the raw IP `192.168.4.1` (SoftAP) or the device's LAN IP (wired). |
| Not joined to the device network | Re-check Wi-Fi association / DHCP lease. |
| Slow-client / Slowloris timeout | The HTTP worker enforces a 5 s `SO_RCVTIMEO` and closes idle sockets ([ARCHITECTURE.md §4](ARCHITECTURE.md)); reload the page. |
| `413 Payload Too Large` | A request body over **16 KB** is rejected ([API.md §1](API.md)). Don't paste oversized Wi-Fi passwords. |
| `403 cross-origin request rejected` | A state-changing POST whose `Origin` host ≠ `Host` is blocked (CSRF guard). Use the dashboard directly, or send a matching `Origin` from CLI ([API.md §2](API.md)). |
| `401` on a control action | A PIN is provisioned and you have no valid `pd_session`. Log in via `/api/admin/login` first ([SETUP_GUIDE.md §3](SETUP_GUIDE.md)). |
| `429` on login | Brute-force lockout: 5 failed logins → 60 s cooldown (`AdminAuth`). Wait it out; it auto-clears. |

---

## Forgot the admin PIN

There is **no PIN recovery** — the PIN is stored only as a salted, iterated SHA-256 hash
(`AdminAuth.cpp`). The path back to a usable device is a **factory reset**, which clears
the admin credential and Wi-Fi config and reboots into onboarding/AP mode.

**If you still hold a valid session** (logged in elsewhere), POST a factory reset:

```bash
DEVICE=http://192.168.4.1
curl -s -b cookies.txt -H "Origin: $DEVICE" \
     -X POST --data "confirm=ERASE" \
     "$DEVICE/api/factory-reset"
# -> {"status":"ok","message":"Factory reset. Rebooting to captive-portal setup..."}
```

- The `confirm=ERASE` token is **required** — without it the endpoint returns
  `400 {"error":"factory reset requires confirm=ERASE"}` (`HttpServer::sendApiFactoryReset`).
- Factory reset erases `admin_salt`/`admin_hash` (clearing the PIN and all sessions) and
  the Wi-Fi keys (`wifi_mode`, `wifi_ssid`, `wifi_pass`, `decayed`), then reboots.
- The dashboard's **Factory Reset** button performs the same call (`index_html.h`).

**If you have no valid session** (PIN lost and not logged in anywhere): the gated
`/api/factory-reset` will return `401`. Recover physically — **re-flash over USB/JTAG**.
A full reflash returns the device to the unprovisioned/open state; note that
`idf.py erase-flash` wipes NVS entirely, and even a plain `idf.py flash` may leave NVS in
an inconsistent state across the partition migration, so **treat it as a clean slate and
re-onboard** ([OTA.md §5.1](OTA.md)). After reflashing, set a new PIN as the first step
([SETUP_GUIDE.md §3](SETUP_GUIDE.md)).

> [!TIP]
> Choose a PIN you will remember but that is still ≥6 alphanumeric chars
> ([THREAT_MODEL.md §5.2](THREAT_MODEL.md)). There is no "reset link."

---

## OTA update fails or rolls back

**Symptom:** A firmware push via `/api/ota/upload` errors, or the device reverts to the
previous firmware after rebooting. Full reference: [OTA.md](OTA.md).

| Code / behavior | Cause | Fix |
| :--- | :--- | :--- |
| `401` | A PIN is provisioned but the request has no/invalid `pd_session`. | Log in first (`/api/admin/login`), reuse the cookie jar ([OTA.md §3.2](OTA.md)). |
| `403` | Cross-origin rejected (CSRF guard). | Send a matching `Origin` header. |
| `411` | Missing/zero `Content-Length`. | Use `--data-binary @build/SipServer.bin` so curl sets the length. |
| `400` | Upload truncated / socket closed mid-stream / flash write failed. | Re-upload on a stable link; the boot partition is unchanged, the device keeps running the current image ([OTA.md §4.2](OTA.md)). |
| `422` | `esp_ota_end()` rejected the image (bad magic / corrupt / not a valid app). | Verify you uploaded the correct `build/SipServer.bin`; rebuild. |
| `500` | `esp_ota_begin` / `set_boot_partition` failed. | Check device logs; retry. |
| `501` | This is the **host/desktop** build — OTA is device-only. | Run OTA against real hardware ([OTA.md §3.4](OTA.md)). |
| **Boots the OLD image after reboot** | **Anti-rollback** restored the previous slot because the new image did not reach `markValid()` (it crashed/boot-looped during startup). This is the safety net working — no bricking. | Inspect serial logs for the boot-loop cause, fix the image, re-upload. The device confirms a healthy image only after a few seconds of stable operation ([OTA.md §4](OTA.md), `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`). |
| Power lost during write | The slot is partially written but never activated; `otadata` still points at the running slot, so the next boot is the old image. | Simply re-upload to retry ([OTA.md §4.2](OTA.md)). |

> [!IMPORTANT]
> OTA images are **unsigned** today and the upload is gated only by the admin PIN +
> same-origin check. **Always set a PIN** and **restrict OTA to the local link**
> ([OTA.md §6](OTA.md), [THREAT_MODEL.md](THREAT_MODEL.md) T-5).

---

## Display blank or garbled (Guition JC3248W535 variant)

**Symptom:** The 3.5" touch panel is dark, shows noise, or never renders the UI.

| Cause | Fix |
| :--- | :--- |
| Missing I2C pull-ups on the touch bus | Many JC3248W535 clones omit them; add **4.7 kΩ pull-ups** on `TOUCH_SDA` (GPIO 4) and `TOUCH_SCL` (GPIO 8) to 3.3 V, or the panel/touch init can time out and crash ([HARDWARE.md §8A](HARDWARE.md)). |
| Wrong build / not the display target | Build with `idf.py -D SIP_TRANSPORT=display build` ([README.md](../README.md#jc3248w535en-smart-display)). A Wi-Fi-only build does not drive the panel. |
| PSRAM not in Octal mode | The two 307.2 KB LVGL frame buffers need **8 MB Octal PSRAM @ 80 MHz** (`MALLOC_CAP_SPIRAM`). Octal PSRAM and `qio` flash mode are set via `sdkconfig.defaults`; a mismatched config exhausts internal SRAM ([HARDWARE.md §2](HARDWARE.md), README display note). |
| Backlight off | `TFT_BL` is GPIO 1, **active-high** (high = on). A dark-but-alive panel can be a backlight wiring issue ([HARDWARE.md §2](HARDWARE.md)). |
| Headless fallback engaged | If the display panel fails to initialize, the firmware is designed to fall back to headless operation (README "Robust Concurrency & Headless Fallback") — SIP and the HTTP dashboard still work; reach it over the network while you debug the panel. |

> [!NOTE]
> The display is optional to operation. Even with a dead panel, the device still runs the
> SIP registrar and serves the HTTP dashboard — manage it from a browser
> ([HARDWARE_SELECTION.md §3](HARDWARE_SELECTION.md)).

---

## Still stuck? Collect this before asking for help

- `GET /api/status` output (uptime, `packetsProcessed`, `packetsDropped`, `clients`,
  `sessions`) — [API.md](API.md).
- `GET /api/admin/status` (`provisioned` / `authenticated` booleans).
- Serial monitor log around the failure (`idf.py monitor`).
- Board model and `SIP_TRANSPORT` build used ([HARDWARE_SELECTION.md](HARDWARE_SELECTION.md)).
- The exact phone model/firmware and its codec/transport settings
  ([PHONE_COMPATIBILITY.md](PHONE_COMPATIBILITY.md)).
