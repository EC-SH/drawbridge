# Third-Party Notices

Drawbridge — an ENGAGE product — incorporates the following upstream and
third-party components. Each remains under its own license, cited below.
See `LICENSE` for how these interact with the commercial license.

## Upstream foundation — SipServer (MIT)

Portions of `src/` derive from the SipServer project by BarGabriel
(https://github.com/BarGabriel/SipServer). The original license is retained
verbatim as required:

```
MIT License

Copyright (c) 2022 BarGabriel

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Runtime components

| Component | License | Use | Notes |
|---|---|---|---|
| wolfSSL `^5.8.2` | **GPLv2** (dual: commercial from wolfSSL Inc.) | SSH crypto — **display build + `PD_HOST_SSH` only** | **Commercial distribution of linked binaries requires a wolfSSL commercial license** — see LICENSE §2. The eth/wifi/lan8720 firmware does NOT link wolfSSL. |
| wolfSSH `^1.4.20` | **GPLv2** (dual) | SSH sysop terminal server (display build only) | Same obligation as wolfSSL |
| littlessh (in-tree, `components/littlessh`) | Apache-2.0 (this project) | SSH sysop terminal server — **eth/wifi/lan8720 builds** | From-scratch SSH-2.0 server; crypto via ESP-IDF's bundled mbedTLS (below). No GPL obligation. |
| mbedTLS (bundled in ESP-IDF) | Apache-2.0 | PSA crypto for littlessh (curve25519/ECDSA/AES-GCM) + esp-tls | Ships with ESP-IDF |
| ESP-IDF (v5.1–v6.0.1) | Apache-2.0 | Firmware SDK | https://github.com/espressif/esp-idf |
| espressif/cjson (cJSON) | MIT | WAN anchor call-control API JSON parsing | Dave Gamble & contributors |
| lvgl/lvgl `^8.3` | MIT | Touch-display dashboard | |
| espressif/mdns | Apache-2.0 | `*.local` discovery | |
| espressif/esp_websocket_client | Apache-2.0 | WAN anchor call-control WebSocket client | |
| espressif/esp_lcd_touch | Apache-2.0 | AXS15231B touch | |
| espressif/w5500, espressif/lan87xx | Apache-2.0 | Ethernet PHY drivers | |
| cxxopts (vendored `src/Helpers/cxxopts.hpp`) | MIT | Host CLI parsing | Jarryd Beck |

## Development-only components (not distributed in firmware)

| Component | License | Use |
|---|---|---|
| GoogleTest / GoogleMock | BSD-3-Clause | Unit test suite (FetchContent, host builds) |

If a component is added to `main/idf_component.yml`, the host CMake
FetchContent set, or vendored into `src/`, add it here in the same change.
