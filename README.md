# DRAWBRIDGE

> Standalone SIP PBX on a single PoE Ethernet board. One cable, zero configuration, deployed in under 10 minutes.

[![License](https://img.shields.io/badge/license-Commercial-blue.svg)](LICENSE)
[![Built on pocket-dial](https://img.shields.io/badge/built%20on-pocket--dial%20(MIT)-green.svg)](THIRD_PARTY_NOTICES.md)

DRAWBRIDGE turns a [LilyGO T-ETH-ELITE S3](https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series)
into a fully self-contained office phone system — SIP registrar, PSTN trunk anchor, and SSH
config interface in a single device. Plug in a PoE Ethernet cable. Phones register. Calls work.
No router changes, no cloud dependency, no server.

Built on the MIT-licensed **pocket-dial** open-source SIP registrar engine.

---

## What it does

- Extension-to-extension SIP calls with peer-to-peer RTP (no server transcoding)
- Outbound and inbound PSTN via WAN anchor (commercial softswitch call-control API) — up to **4 concurrent PSTN calls**, self-healing
- Call park on orbits 700–799, ring/hunt groups, call forwarding (CFU/CFB/CFNA), DND
- Attended and blind transfer (REFER/REFER+Replaces)
- BLF/presence via SUBSCRIBE/NOTIFY
- SSH TUI config interface — 80×24 ANSI terminal, port 22, primary config surface
- Web dashboard — HTTP status, CDR, and OTA firmware updates. Reachable by default
  once provisioned; an optional Security-panel toggle locks it back down so it's only
  reachable after `*4887` (spells HTTP on the keypad) from the registered admin
  extension (default `1001`), a fresh provisioning grace period, or a logged-in
  operator's 1-hour keep-alive
- SIP digest authentication with open/learn/secure registrar modes
- OTA updates with anti-rollback partition protection
- Star codes: `*60`/`*80` DND on/off, `*72`/`*73` call forward, `*69` call return, `*11` echo test
- Virtual extensions: `777` echo test, `999` all-page broadcast
- CDR (call detail records)
- RFC 4028 session timers, RFC 3311 UPDATE, RFC 3515/3891 REFER+Replaces

---

## Hardware

**Commercial reference board: [LilyGO T-ETH-ELITE S3](https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series)**
ESP32-S3, W5500 wired Ethernet, 802.3af PoE — one RJ45 carries power and data. Up to ~48
concurrent calls, 100+ registered extensions.

Other supported boards (development and evaluation only — not for deployment):

| Board | Transport | Notes |
|---|---|---|
| Guition JC3248W535EN | Wi-Fi SoftAP | 3.5" touch display; development milestone board |
| Generic ESP32-S3 | Wi-Fi SoftAP | Limited to ~16 phones by SoftAP station cap |
| Waveshare ESP32-S3-ETH | W5500 Ethernet | Unverified pin map |

See [docs/HARDWARE.md](docs/HARDWARE.md) and [docs/HARDWARE_SELECTION.md](docs/HARDWARE_SELECTION.md)
for pinouts, PoE wiring, and board-tier capacity details.

---

## Install (prebuilt firmware — the deployment path)

No toolchain required. Download the firmware from the
[latest release](../../releases/latest).

### 1. Pick your firmware variant

| Filename pattern | Board |
|---|---|
| `*-eth.*` | LilyGO T-ETH-ELITE (commercial reference) |
| `*-display.*` | Guition JC3248W535 (development board) |
| `*-wifi.*` | Generic ESP32-S3 |

### 2. Verify the download

```powershell
# Windows
Get-FileHash drawbridge-eth-<version>.bin -Algorithm SHA256
```

```bash
# Linux / macOS
sha256sum drawbridge-eth-<version>.bin
```

Compare the output against `SHA256SUMS` in the release.

### 3. Flash

Install [esptool](https://github.com/espressif/esptool) (`pip install esptool`), then:

```bash
esptool.py --chip esp32s3 -p COM3 write_flash 0x0 drawbridge-eth-<version>.bin
```

Replace `COM3` with your serial port (`/dev/ttyUSB0` on Linux/macOS).

### 4. First boot

Connect the PoE Ethernet cable. The board obtains a DHCP address and advertises
`drawbridge.local` on the LAN. Open the web dashboard to finish setup — it's
reachable by default once provisioned, no SSH required:

```
http://drawbridge.local
```

SSH is still available (`ssh drawbridge.local`) for the full SSH TUI config surface,
but it's no longer a required first step. If you've locked the dashboard down via the
Security panel's HTTP-lock toggle, dial `*4887` from the admin extension to reopen it.

See [docs/FLASHING.md](docs/FLASHING.md) for first-flash detail and OTA update procedure.

---

## Documentation

| Document | Description |
|---|---|
| [docs/FLASHING.md](docs/FLASHING.md) | First-flash and OTA update procedures |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Board pinouts, PoE wiring, BOM |
| [docs/HARDWARE_SELECTION.md](docs/HARDWARE_SELECTION.md) | Board tier comparison and capacity |
| [docs/PHONE_COMPATIBILITY.md](docs/PHONE_COMPATIBILITY.md) | Tested SIP phones and softphone clients |
| [docs/LEARN_MODE.md](docs/LEARN_MODE.md) | Registrar open/learn/secure security modes |
| [docs/SCALING.md](docs/SCALING.md) | Capacity configuration and pool sizing |
| [docs/OTA.md](docs/OTA.md) | OTA update technical detail and anti-rollback |
| [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) | Security posture |
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | 10-minute first deployment (field tech guide) |
| [docs/SYSOP_MANUAL.md](docs/SYSOP_MANUAL.md) | SSH TUI complete screen-by-screen reference |
| [docs/OPERATOR_ADMIN.md](docs/OPERATOR_ADMIN.md) | Extension management, ring groups, CDR, OTA |
| [docs/EXTENSION_SETUP.md](docs/EXTENSION_SETUP.md) | Per-phone SIP registration guide |
| [docs/SETUP_GUIDE.md](docs/SETUP_GUIDE.md) | Detailed first-time configuration (eth board) |

---

## Building from source

For contributors and integrators. Operators should use the prebuilt firmware above.

**Requirement:** ESP-IDF v6.0.1. The `SIP_TRANSPORT` variable selects the firmware variant.

```bash
# Commercial reference board (T-ETH-ELITE, default)
idf.py -D SIP_TRANSPORT=eth build
idf.py -p COM4 -D SIP_TRANSPORT=eth flash

# Development display board
idf.py -D SIP_TRANSPORT=display build

# Generic Wi-Fi
idf.py build
```

### Host build (CI and dev loop)

The host binary is a development and CI artifact — it is not a product.

```bash
cmake -B build -S . && cmake --build build
ctest --test-dir build/tests --output-on-failure
```

When `IDF_PATH` is unset, CMake builds the host binary and GoogleTest suite. When `IDF_PATH`
is set, it delegates to ESP-IDF. Unset `IDF_PATH` for host work.

---

## Project structure

```
drawbridge/
├── CMakeLists.txt              # Dual-mode build: host (CMake) or ESP-IDF
├── main/                       # ESP-IDF component — per-transport entry points
│   ├── esp_main_eth.cpp        # T-ETH-ELITE (W5500, PoE)
│   ├── esp_main.cpp            # Generic ESP32-S3 (Wi-Fi SoftAP)
│   ├── esp_main_display.cpp    # Guition JC3248W535 (touch display)
│   ├── esp_main_eth_lan8720.cpp# Classic ESP32 (internal EMAC + LAN8720)
│   ├── drivers/                # AXS15231B QSPI panel driver
│   ├── ui/                     # LVGL CGA dashboard (display build)
│   └── wifi/                   # Captive-portal DNS redirect
├── src/
│   ├── SIP/                    # SIP engine — parser, state machines, PBX features
│   └── Helpers/                # UDP socket, HTTP dashboard, SSH TUI, OTA, auth
├── components/
│   └── littlessh/              # From-scratch SSH-2.0 server (eth/wifi/lan8720 builds)
├── tests/                      # GoogleTest suite + HTTP smoke scripts
└── .github/workflows/ci.yml    # CI: cppcheck, GoogleTest, firmware matrix
```

---

## License

DRAWBRIDGE is a commercial product — see [LICENSE](LICENSE).

Built on the MIT-licensed pocket-dial SIP registrar engine (notice retained). SSH backends:
the display build links wolfSSL/wolfSSH (GPL-dual-licensed); the eth/wifi/lan8720 builds use
the in-tree littlessh backend on mbedTLS (Apache-2.0) — no GPL dependency in the wired/PoE
builds. Full third-party notices in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
