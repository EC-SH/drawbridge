# drawbridge

> [!NOTE]
> **Latest release: [v1.2.0](../../releases/latest)** — hardware-verified on the JC3248W535 display board (boots, joins Wi-Fi, SIP + dashboard confirmed working). **Prebuilt, flashable firmware is attached to the release** — see [Install](#install). `main` carries newer in-development changes; pin to a release tag for production.

**A sovereign, zero-trust SIP gateway for the edge.** drawbridge transforms $10 microcontrollers into autonomous, mTLS-secured PBX anchors, decentralizing secure telecom infrastructure without exposing inbound WAN surfaces. Flash an ESP32-S3, connect softphones or physical IP phones to its network, and make direct VoIP calls instantly.

Now with **native touch display support** and **W5500 wired-Ethernet capability**:
* **Guition JC3248W535EN HMI Target**: Features a native **ESP-IDF v6.0.1 + LVGL 8.4.0** application driving a 3.5" IPS capacitive touchscreen with a retro CGA CRT-style switchboard dashboard. *(Note: This board represents the development milestone the ESP32 SIP logic was built on, and is not recommended for deployment).*
* **Wired-Ethernet & PoE Support**: Pre-configured transport targets for [Waveshare ESP32-S3-ETH](https://www.waveshare.com/wiki/ESP32-S3-ETH) (unverified, check pins) and [LilyGO T-ETH boards](https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series) (canonical), bridging drawbridge to professional wired and Power-over-Ethernet network switches.
* **SIP Virtual Extensions**: Features an inline **SIP Echo Test (`777`)** with zero-media-processing SDP loopback, and a **Parallel Broadcast / All-Page Intercom (`999`)** with target-URI rewritin[...]
* **Strict VoIP Interoperability**: Resolves early media loopback loops and ringing hangs on professional devices (like Yealink IP phones) by automatically stripping caller SDPs from all provisiona[...]


---

## DRAWBRIDGE (Commercial Product Line)

**pocket-dial** is the open-source signaling registrar foundation for **[DRAWBRIDGE](docs/DRAWBRIDGE.md)**—a LAN-sovereign edge PBX designed for high-security commercial environments. 

Unlike the open-source pocket-dial, **DRAWBRIDGE** enforces:
* **Zero Inbound WAN Surface**: No open inbound ports, no SIP trunk registrations, and no exposed RTP ranges on the WAN. The device initiates all external contacts.
* **mTLS Identity (No shared secrets)**: Device authentication to the operator's Media Anchor uses an operator-issued mTLS certificate.
* **B2BUA Media Bridging**: WAN/PSTN call legs are terminated and bridged locally (`RTP ⇄ playout buffer ⇄ raw PCM16 over chunked HTTPS GET/POST`) to secure the media path and route it to hosted infrastructure.

For the comprehensive architecture details, see the **[DRAWBRIDGE System Architecture Overview](docs/DRAWBRIDGE.md)**.

---

## Install

There are three ways to run pocket-dial. **Most people want Option 1.**

### 🔌 Option 1 — Flash prebuilt firmware to an ESP32-S3 (easiest, no toolchain)

Download the firmware for your board from the **[latest release](../../releases/latest)** and pick the matching variant:

| Variant | Board / use |
|---------|-------------|
| `*-display.*` | Guition JC3248W535 3.5" touch display (Not recommended for deployment) |
| `*-eth.*` | W5500 wired Ethernet / PoE ([Waveshare ESP32-S3-ETH](https://www.waveshare.com/wiki/ESP32-S3-ETH), [LilyGO T-ETH](https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series)) |
| `*-wifi.*` | Generic ESP32-S3 (Wi-Fi SoftAP) |

Flash with [esptool](https://github.com/espressif/esptool) (`pip install esptool`). Example for the **display** build — replace `COM3` with your serial port (`/dev/ttyUSB0` on Linux/macOS):

```bash
esptool --chip esp32s3 -p COM3 -b 460800 write_flash \
  0x0     bootloader-esp32s3-display.bin \
  0x8000  partition-table-esp32s3-display.bin \
  0x20000 SipServer-esp32s3-display.bin
```

(Swap `display` → `eth`/`wifi` for the other variants. Exact offsets are also in the release's `flasher_args-*.json`; verify downloads against `SHA256SUMS`.) Power-cycle the board and it boots the SIP registrar + dashboard. On first boot it raises an open Wi-Fi AP `esp32-sipserver`; connect a softphone to `192.168.4.1:5060`. See [Configuration](#configuration).

### 🛠 Option 2 — Build the firmware from source

For development or custom configs (ESP-IDF v6.0.1+ or Arduino). See **[Building](#building)** — e.g. `idf.py -D SIP_TRANSPORT=display flash monitor`.

### 💻 Option 3 — Run on a computer or Raspberry Pi (no ESP32 needed)

The same SIP engine runs as a desktop/server binary on Linux/macOS/Windows. Needs CMake + a C++17 compiler:

```bash
# One-liner: download the latest release source, build, and run
curl -fsSL https://raw.githubusercontent.com/EC-SH/drawbridge/main/install.sh | sh

# …or from a clone:
./quickstart.sh                 # Windows: quickstart.bat
./quickstart.sh --service       # install as a systemd service (Linux)
```

See [Desktop Mode](#desktop-mode) for manual CMake steps and CLI flags.

---

## Project Status & MVP Roadmap

> [!NOTE]
> **Current phase**: In Development — Commercial Softswitch Call-Control API Trunk Anchor & Performance Hardening.
>
> Following the v1.2.0 MVP release, we have integrated a WAN trunk anchor against a commercial softswitch's call-control API (terminating local SIP/RTP and bridging to mTLS HTTPS/WSS audio streams), resolved 27 concurrency/memory defects in the SIP engine, and implemented call setup/teardown latency optimizations.

### ✅ **Completed**
- Core SIP signaling (REGISTER, INVITE, ACK, BYE, CANCEL, OPTIONS)
- Peer-to-peer RTP media routing
- Wi-Fi AP mode with captive-portal onboarding **+ Wi-Fi STATION (client) mode**
- Web dashboard with **admin PIN authentication** and **OTA firmware updates**
- Virtual extensions (Echo `777`, Broadcast `999`)
- **Call Detail Records (`/api/cdr`)** and **Do-Not-Disturb (`/api/dnd`)**
- Concurrency & memory-safety hardening, input validation, per-IP rate limiting
- Configurable memory pools + hardware tiers ([SCALING.md](docs/SCALING.md))
- Desktop (Linux/Windows) mode + Arduino IDE sketch templates
- **End-to-end hardware validation on the JC3248W535** (Issue #44) ✅
- **Commercial softswitch call-control API trunk anchor**: Terminate the handset's RTP locally and bridge audio to an upstream softswitch over mTLS HTTPS/WSS (PCM16 @ 8kHz)
- **Inbound PSTN→extension calls (Mode 1)** via the trunk anchor (delayed-offer INVITE toward the extension; *hardware-verified pre-alpha — two-way audio, bridging, and DTMF working; soak/stability hardening in progress*)
- **SIP digest authentication** with per-extension secrets (`SipSecretStore`) and **open / learn / secure registrar modes** (`RegistrarMode`)
- **Call forwarding**: unconditional (CFU), on-busy (CFB), no-answer (CFNA)
- **Ring/hunt groups** (`/api/group`)
- **Blind transfer** via `REFER`
- **Star-codes**: `*60` (DND), `*80` (call-forward status readback), `*72`/`*73` (CFU enable/disable), `*69` (call return), `*11` (extension/IP readback)
- **SSH sysop terminal** — 80×24 ANSI TUI over SSH (wolfSSH on the display build; from-scratch littlessh backend on eth/wifi/lan8720)
- **SIP Engine Defect Resolution**: Fixed 27 concurrency, locking, memory, and code quality issues
- **Call Setup and Teardown Latency Optimizations**: Parsed status directly from WS payloads, implemented exponential back-off on retries, configured HTTP timeouts, and optimized loopback simulation delays (Issue #29)

### ⏳ **In Progress / Backlog**
- Arduino IDE platform-guard verification (Issue #41)
- Raise UDP RX mailbox + multi-IP load testing (Issues #78, #79)
- SD-card support (Issue #80)
- Zero-Touch Phone Auto-Provisioning (#35), Live SIP Tracer (#32), PCAP dump (#33)
- WS Event Task Stack Size optimization (#86)
- Desktop/Host Media Transport Support (#87)

**For detailed issue tracking and resolved items, see [ISSUES.md](ISSUES.md) and the [GitHub Issue Tracker](../../issues).**

---

## Developer Roadmap & Issue Tracker

> [!NOTE]
> We maintain an active architectural roadmap and issue tracker documenting concurrency challenges, task pinning, and socket-blocking mitigations. Review our planned performance updates in [ISSUES.md](ISSUES.md).

---

## Table of Contents

* [Install](#install)
* [Features](#features)
* [How It Works](#how-it-works)
* [Architecture](#architecture)
* [Project Structure](#project-structure)
* [Supported SIP Methods \& Responses](#supported-sip-methods--responses)
* [Virtual Extensions (`777` & `999`)](#virtual-extensions-777--999)
* [VoIP Interoperability & SDP Stripping](#voip-interoperability--sdp-stripping)
* [Automated Testing & Smoke Tools](#automated-testing--smoke-tools)
* [Building](#building)
  * [ESP32-S3 (ESP-IDF v6.0.1)](#esp32-s3-esp-idf-v53)
  * [JC3248W535EN Smart Display](#jc3248w535en-smart-display)
  * [W5500 Ethernet & PoE](#w5500-ethernet--poe)
  * [ESP32-S3 (Arduino IDE)](#esp32-s3-arduino-ide)
  * [Desktop (Linux / Windows)](#desktop-mode)
* [API Reference](#api-reference)
* [Configuration](#configuration)
* [License](#license)

---

## Features

* **Standalone AP Mode (ESP32-S3)** — Spawns an open Wi-Fi access point (`esp32-sipserver`), runs an internal DHCP server, and binds the SIP registrar to `192.168.4.1:5060`. Connect, register, a[...]
* **W5500 Ethernet / PoE Transport** — Direct network interface driver support for wired RJ45 and Power-over-Ethernet environments, featuring DHCP or static IP fallbacks.
* **Captive Portal Wi-Fi Onboarding** — When unconfigured, spawns a secure setup SoftAP (`My-Ap`) and displays a join QR code on screen. Intercepts HTTP traffic via a background DNS redirection [...]
* **SSH Sysop Terminal (ANSI TUI)** — The primary headless config surface: an 80×24 "sysop terminal" over SSH on port 22, driving live System Monitor, Network, PBX Config (extensions, ring groups, forwards/DND, IVR, TRUNK), Security, and Reports/CDR screens. SSH is open until an admin PIN is provisioned, then PIN-gated. The display build serves it via wolfSSH; the eth/wifi/lan8720 builds use the lighter in-tree **littlessh** backend (PSA/mbedTLS, no wolfSSL).
* **Smart Touchscreen Dashboard** — Native ESP-IDF v6.0.1 + LVGL 8.4.0 CGA-style HMI. Double-buffered in external OPI PSRAM (307.2 KB 16-bit color frames). Tap to cycle phosphorus theme colors (CGA B[...]
* **Active Keepalive OPTIONS & Pruning** — Periodically dispatches OPTIONS ping packets to all active clients. Automatically reaps dead bindings upon lease timeouts or silent periods to bound me[...]
* **Robust Concurrency & Headless Fallback** — Dedicated, core-isolated FreeRTOS tasks. SIP signaling executes on Core 1, and HTTP/Web dashboard operations execute on Core 0. If the display pane[...]

---

## How It Works

The device boots as its own isolated telecommunication hub. Media (RTP) flows directly **peer-to-peer (P2P)** between the endpoints over the local wireless or wired segment. The board brokers sign[...]

```
                   ┌──────────────────────┐
                   │  pocket-dial Server  │
                   │   (SIP Signaling)    │
                   └──────────────────────┘
                        ▲            ▲
              INVITE /  │            │  INVITE /
              200 OK    │            │  200 OK
                        ▼            ▼
              ┌───────────┐        ┌───────────┐
              │ Phone 102 │◀══════▶│ Phone 105 │
              │  (Caller) │   RTP  │  (Callee) │
              └───────────┘  Audio └───────────┘
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      SipServer                          │
│                                                         │
│  ┌───────────┐    ┌──────────────────┐    ┌──────────┐  │
│  │ UdpServer │───▶│ SipMessageFactory│───▶│Requests- │  │
│  │           │    │                  │    │Handler   │  │
│  │ (recv/    │    │ Parses raw UDP   │    │          │  │
│  │  send)    │◀───│ into SipMessage  │    │ Dispatch │  │
│  │           │    │ or SipSdpMessage │    │ table of │  │
│  │           │    └──────────────────┘    │ handlers │  │
│  └───────────┘                            │          │  │
│                                           │ Manages: │  │
│                   ┌──────────────────┐    │ _clients │  │
│                   │    SipClient     │◀───│_sessions │  │
│                   │ (number + addr)  │    └──────────┘  │
│                   └──────────────────┘          │       │
│                                                 ▼       │
│                   ┌──────────────────┐    ┌──────────┐  │
│                   │     Session      │◀───│ Outbox   │  │
│                   │ (callID, state,  │    │ Queue    │  │
│                   │  src, dest, P2P) │    │ (Lock-   │  │
│                   └──────────────────┘    │  free)   │  │
│                                           └──────────┘  │
└─────────────────────────────────────────────────────────┘
```

---

## Project Structure

```
drawbridge/
├── CMakeLists.txt              # Dual-mode build: desktop (CMake) or ESP-IDF
├── ISSUES.md                   # Roadmap & Architectural Concurrency Issues
├── CHANGELOG.md                # Detailed release notes and issue fixes
├── LICENSE                     # MIT
├── README.md                   # Developer Manual (You are here)
├── main.cpp                    # Desktop entry point (cxxopts CLI)
├── main/
│   ├── CMakeLists.txt          # ESP-IDF component builder (wifi/eth/display targets)
│   ├── esp_main.cpp            # ESP32-S3 entry point (Wi-Fi AP transport)
│   ├── esp_main_eth.cpp        # ESP32-S3 entry point (W5500 Ethernet transport)
│   ├── esp_main_eth_lan8720.cpp# Classic ESP32 entry point (internal EMAC + LAN8720 PHY)
│   ├── esp_main_display.cpp    # ESP32-S3 entry point (Guition AXS15231B touch display target)
│   ├── drivers/                # Low-level QSPI panel and touch screen driver layer
│   │   ├── esp_lcd_axs15231b.c # High-performance esp_lcd driver
│   │   └── esp_lcd_axs15231b.h
│   ├── ui/                     # Interactive LVGL 8.4.0 switchboard dashboard
│   │   ├── ui.cpp              # CGA interface, touch press router, log terminal
│   │   └── ui.h
│   └── wifi/                   # DNS/HTTP redirect captive portal wizard
│       ├── DnsServer.cpp
│       └── DnsServer.hpp
├── sketches/                   # Self-contained Arduino IDE sketches
│   ├── SipServer/              # Generic ESP32-S3 Wi-Fi SoftAP
│   ├── SipServerETH/           # Waveshare ESP32-S3-ETH (W5500)
│   ├── SipServer_T_ETH_Lite_W5500/   # LilyGO T-ETH-Lite W5500
│   ├── SipServer_T_POE_Pro_LAN8720/  # LilyGO T-PoE-Pro LAN8720
│   ├── SipServer_JC3248W535/   # Guition 3.5" IPS Smart Display [DEPRECATED]
│   ├── MinimalTest/            # Diagnostic: PSRAM / heap allocation verification
│   └── PinFuzzer/              # Diagnostic: GPIO scanning tool
├── components/                 # ESP-IDF components (firmware-only)
│   └── littlessh/              # From-scratch SSH-2.0 server on PSA/mbedTLS
│       ├── include/littlessh.h #   public C API (eth/wifi/lan8720 SSH backend)
│       └── src/littlessh.c     #   KEX (curve25519) · ECDSA host key · AES-GCM
├── src/                        # Core SIP Engine + Helpers (Cross-Platform C++)
│   ├── Helpers/
│   │   ├── UdpServer.cpp/.hpp  # Threaded, platform-abstracted UDP socket
│   │   ├── HttpServer.cpp      # select-based CGA web dashboard + REST API
│   │   ├── AdminAuth.cpp       # Salted/iterated admin-PIN auth
│   │   ├── SipDigest.cpp / SipSecretStore.cpp # SIP digest auth + per-ext secrets
│   │   ├── SshServer.cpp       # SSH façade + wolfSSH backend (display)
│   │   ├── SshServerLittlessh.cpp # littlessh backend glue (eth/wifi/lan8720)
│   │   ├── Tui.cpp/.hpp        # Transport-agnostic 80×24 ANSI sysop TUI
│   │   ├── OtaUpdater.cpp      # Dual-OTA image apply + rollback
│   │   └── ArpLookup.cpp / IDGen.hpp
│   └── SIP/
│       ├── SipMessage(.cpp/.hpp), SipSdpMessage, SipMessageFactory # parser/dispatch
│       ├── SipClient / Session  # registrar model + call state machine
│       ├── RequestsHandler.cpp # Signaling logic, PBX features, dashboard API
│       ├── SipServer.cpp       # Orchestrator
│       ├── RtpSender / RtpReceiver / PlayoutBuffer / MediaBridge # B2BUA media path
│       └── ThreeCxAnchorClient / LoopbackAnchorClient # WAN trunk anchor (commercial softswitch call-control API)
```

---

## Supported SIP Methods & Responses

| Method/Response | Enum Constant | Handler | Description |
|---|---|---|---|
| `REGISTER` | `SipMessageTypes::REGISTER` | `onRegister` | Adds or updates client binding in the registrar. |
| `INVITE` | `SipMessageTypes::INVITE` | `onInvite` | Initiates a call session. Intercepts `777` / `999`. |
| `ACK` | `SipMessageTypes::ACK` | `onAck` | Acknowledges session completion. |
| `BYE` | `SipMessageTypes::BYE` | `onBye` | Terminates an active call. |
| `CANCEL` | `SipMessageTypes::CANCEL` | `onCancel` | Aborts a pending call during ringing. |
| `100 Trying` | `SipMessageTypes::TRYING` | `onTrying` | Provisional: call is being routed. |
| `180 Ringing` | `SipMessageTypes::RINGING` | `onRinging` | Provisional: device alerting. Ringing SDP stripped. |
| `200 OK` | `SipMessageTypes::OK` | `onOk` | Final success. Closes signaling handshakes. |
| `404 Not Found` | `SipMessageTypes::NOT_FOUND` | — | Callee number is not currently registered. |
| `480 Temp Unavailable` | `SipMessageTypes::UNAVAILABLE` | `onUnavailable` | Callee is out of range or unregistered. |
| `486 Busy Here` | `SipMessageTypes::BUSY` | `onBusy` | Callee rejected the call. |
| `487 Request Terminated`| `SipMessageTypes::REQUEST_TERMINATED` | `onReqTerminated` | Call aborted via CANCEL. |

---

## Virtual Extensions (`777` & `999`)

pocket-dial includes two built-in virtual extensions to test network media paths and deploy emergency broadcast systems easily:

### 1. Echo Loopback Test (`777`)
* When an endpoint dials `777`, `RequestsHandler` intercepts the `INVITE` and immediately answers `200 OK` using **the caller's own SDP connection information**.
* This forces standard softphones and IP phones to open their RTP channels and stream their output packets back to their own receive ports.
* Resolves the need for server-side audio transcoding or DSP, producing a zero-latency hardware echo test.

### 2. Parallel Intercom / Emergency Broadcast (`999`)
* Dialing `999` invokes parallel forking. The server registers a broadcast session, saves the caller's `INVITE`, and cloning it, dispatches concurrent `INVITE` requests to **every registered exte[...]
* **Auto-Answer Injection**: Injects VoIP-standard auto-answer headers into the outgoing forks:
  * `Call-Info: <sip:any>;answer-after=0`
  * `Alert-Info: info=alert-autoanswer`
  * `Alert-Info: answer-after=0`
  * `Alert-Info: intercom=true`
  * `P-Auto-Answer: normal`
* **Target-URI Rewriting**: Rewrites the Request-URI and `To` headers dynamically to use the target's unique extension rather than `999` (essential for phone-side SIP validation rules).
* **Race Connection**: The first device to answer with `200 OK` is connected to the caller. The server forwards its SDP and immediately fires off a `CANCEL` to all losing ringing targets so their[...]

---

## VoIP Interoperability & SDP Stripping

During testing with strict, professional SIP terminals (like Yealink IP phones), standard softphone behaviors can introduce early media loops or codec failures. pocket-dial applies two core signa[...]

1. **Ringing SDP Stripper (`clearBody`)**: When forwarding a `180 Ringing` packet, any copy of the caller's SDP is erased via `ringing->clearBody()`. The body is removed and the `Content-Length` [...]
2. **Strict Codec Enforcement (`enforceG711`)**: Strips non-G.711 audio configurations (like HD G.722 or Opus codecs) from forked `INVITE` and `200 OK` SDP packages, locking the media session to [...]

---

## Automated Testing & Smoke Tools

The repo ships stdlib-only Python tooling for validating a running PBX — on the host build or on real hardware — without manual phone presses:

### `tests/load/sip_stress.py`
SIP load/stress tester. Registers virtual user-agents, places echo-test (`777`) calls against a registrar, and measures registration + call-setup latency, throughput, and the static-pool / rate-limit ceilings (also samples the HTTP `/api/status` counters).
```bash
python tests/load/sip_stress.py <registrar-ip> [port]
```

### `.smoke/sip_probe.py`
Quick liveness probe — sends a SIP `REGISTER` (falling back to `OPTIONS`) over UDP and reports any response. Exit 0 if the stack answers.
```bash
python .smoke/sip_probe.py <ip> [port]      # default port 5060
```

### `.smoke/ssh_test.py` · `.smoke/ssh_tui.py`
Scripted SSH smoke tests for the sysop console — works against either backend (wolfSSH on the display build, littlessh on the eth/wifi builds). SSH is open while the device is unsecured; the admin PIN becomes the password once set. `ssh_tui.py` additionally reconstructs the live 80×24 TUI through a `pyte` terminal emulator.
```bash
python .smoke/ssh_test.py <host> [password]
python .smoke/ssh_tui.py  <host> [password]   # renders the full ANSI TUI
```

### `tests/http/test_api.sh`
The single HTTP API smoke suite (happy-path → CSRF → input-validation → 404 → OTA → admin-auth), used by both CI and on-hardware runs.
```bash
bash tests/http/test_api.sh <ip:port>       # e.g. 127.0.0.1:8080
```

The `.smoke/` directory also holds hardware helpers: `capture.py` (bounded USB-CDC serial boot-log capture) and `gen_provision_nvs.py` (generates an NVS image to provision a board for headless SIP/SSH testing).

---

## Building

### ESP32-S3 (ESP-IDF v6.0.1)
Required for professional firmware development. Installs and registers the device under native ESP-IDF structures.

```bash
# Set compilation target chip
idf.py set-target esp32s3

# (Optional) Open configuration menu
idf.py menuconfig

# Compile, flash to the board, and monitor serial output
idf.py build flash monitor
```

---

### JC3248W535EN Smart Display
To compile the high-performance native touchscreen switchboard UI:

```bash
# Sourced ESP-IDF environment required

# 1. Select the ESP32-S3 target chip
idf.py set-target esp32s3

# 2. Build using the display transport compile flag
idf.py -D SIP_TRANSPORT=display build

# 3. Flash and monitor (replace COM3 with your actual serial port)
idf.py -p COM3 -D SIP_TRANSPORT=display flash monitor
```
> [!IMPORTANT]
> The target allocates two 307.2 KB 16-bit double buffers in external RAM. The mandatory configurations (`FlashMode=qio` and `OPI PSRAM`) are automatically handled via `sdkconfig.defaults`.

---

### W5500 Ethernet & PoE
To build the server utilizing high-speed wired Ethernet on a W5500 board (LilyGO
T-ETH-ELITE, Waveshare ESP32-S3-ETH, etc.):

```bash
# Build using the Ethernet transport compile flag.
# Defaults to the LilyGO T-ETH-ELITE S3 pin map.
idf.py -D SIP_TRANSPORT=eth build

# For the Waveshare ESP32-S3-ETH instead, select its pin map:
idf.py -D SIP_TRANSPORT=eth -D PD_ETH_BOARD=waveshare build

# Flash and monitor (replace COMx with your board's port)
idf.py -p COMx -D SIP_TRANSPORT=eth flash monitor
```

The boot log prints the active board and pins (`W5500 board: LilyGO T-ETH-ELITE S3 — SCLK=48 …`),
followed by `Ethernet link UP` and the DHCP `IP: …` once the cable is connected. See
[docs/ETH_SMOKE.md](docs/ETH_SMOKE.md) for the full bring-up / smoke-test runbook.

---

### ESP32-S3 (Arduino IDE)
If compiling within an Arduino framework:

1. Open Boards Manager and install the **ESP32 board package** by Espressif.
2. Open `sketches/SipServer/SipServer.ino` (or target `SipServerETH.ino` for wired PoE W5500 connections).
3. Set your target board to **ESP32S3 Dev Module** (or specific board model).
4. Configure **Flash Mode = QIO 80MHz** and **PSRAM = OPI PSRAM** (mandatory for display sketches).
5. Click **Upload**.

---

### Desktop Mode

#### 1. Linux
Useful for debugging SIP transactions or testing softphone connections lock-free without flash waiting periods:
```bash
mkdir build && cd build
cmake ..
make
```

#### 2. Windows
Open a Visual Studio Developer Command Prompt:
```powershell
mkdir build && cd build
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ..
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build . --config Release
```

#### Running the Desktop Binary:
The compiled executable requires the binding IP interface as an argument:
```bash
# Bind the SIP server to your local network interface IP
./build_desktop/Release/SipServer.exe --ip=192.168.12.225

# Optional: Override the default port (5060)
./build_desktop/Release/SipServer.exe --ip=192.168.12.225 --port=5080
```

---

## API Reference

### Core Classes

#### `SipServer`
Manages the lifetime of top-level sockets and binds the handler callbacks.
```cpp
SipServer(std::string ip, int port = 5060, int httpPort = 8080);
```

#### `UdpServer`
Handles socket execution loops, threading abstractions, and core task pinning.
```cpp
UdpServer(std::string ip, int port, OnNewMessageEvent event);
void startReceive(); // Spawns background thread
int send(sockaddr_in addr, const std::string& buffer);
```

#### `SipMessage` & `SipSdpMessage`
Parses headers and extracts SDP details through an O(n) non-mutating index-walking algorithm.
* **`clearBody()`**: Purges message bodies and rewrites content lengths.
* **`enforceG711()`**: Forces audio codecs list to `0 8 101`.
* **`addHeader(name, value)`**: Injects new SIP headers before the content boundary.

---

## License

Drawbridge is a **commercial product line** — see the [LICENSE](LICENSE) file. It is built on the MIT-licensed SipServer foundation (notice retained). The SSH sysop console uses one of two backends depending on the build: the **display** firmware links GPL-dual-licensed **wolfSSL/wolfSSH**, while the **eth / wifi / lan8720** firmware uses the in-tree **littlessh** backend (`components/littlessh`) built on ESP-IDF's bundled **mbedTLS** (Apache-2.0) — so the wired/PoE builds carry **no GPL SSH dependency**. All upstream licenses are cited in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
