# Over-The-Air (OTA) Firmware Updates

Phase-1 production hardening adds OTA firmware updates to pocket-dial on the
ESP32-S3. This document covers the partition layout, how to build and push an
update, the rollback strategy, the one-time migration from the old single-app
layout, and the security posture.

> TL;DR: dual-OTA (`ota_0` / `ota_1`) on the 16 MB flash, an admin-gated
> streaming upload endpoint (`POST /api/ota/upload`), explicit reboot
> (`POST /api/ota/reboot`), and mark-valid-on-healthy-boot rollback. An
> **application-layer ECDSA-P256 image signature** (the `X-OTA-Signature` header,
> verified before the slot is made bootable) is wired in — **off by default**,
> enabled per-build with a provisioned key (§6, issue #47). The **durable** fix
> against physical reflash remains Secure Boot v2 + flash encryption (see
> [THREAT_MODEL.md](THREAT_MODEL.md)).

---

## 1. Partition layout

### 1.1 The new table (`partitions.csv`)

```
# Name,     Type, SubType,  Offset,    Size
nvs,        data, nvs,      0x9000,    0x6000
otadata,    data, ota,      0xf000,    0x2000
phy_init,   data, phy,      0x11000,   0x1000
ota_0,      app,  ota_0,    0x20000,   0x600000
ota_1,      app,  ota_1,    0x620000,  0x600000
```

### 1.2 Rationale

| Partition | Offset | Size | Why |
|-----------|--------|------|-----|
| `nvs`     | `0x9000`  | `0x6000` (24 KB) | **Byte-identical to the previous single-`factory` layout.** Holds WiFi creds + the admin PIN/salt/hash. Kept at the same offset/size so the data stays layout-compatible across the migration. **Do not move or resize.** |
| `otadata` | `0xf000`  | `0x2000` (8 KB)  | Two 4 KB sectors (the required size). Records which slot is active and the per-slot rollback/validation state. |
| `phy_init`| `0x11000` | `0x1000` (4 KB)  | RF calibration blob (unchanged role; shifted up to make room for `otadata`). |
| `ota_0`   | `0x20000` | `0x600000` (6 MB)| First app slot. |
| `ota_1`   | `0x620000`| `0x600000` (6 MB)| Second app slot (A/B partner). |

Arithmetic / alignment checks:

- `nvs` ends at `0x9000 + 0x6000 = 0xf000` → `otadata` starts there. ✅
- `otadata` ends at `0xf000 + 0x2000 = 0x11000` → `phy_init` starts there. ✅
- App partitions **must be 64 KB-aligned** on the ESP32-S3 (MMU flash-mapping
  granularity). `0x20000` and `0x620000` are both multiples of `0x10000`. ✅
- `ota_1` ends at `0x620000 + 0x600000 = 0xC20000`, well within the 16 MB
  (`0x1000000`) device — about **3.875 MB of flash left free** at the top for
  future partitions (e.g. a SPIFFS/LittleFS data partition or a coredump
  partition). ✅
- Each 6 MB slot is ~4× the current ~1.5 MB display image — generous headroom
  for UI growth without re-partitioning (which would force another full
  reflash).

`sdkconfig.defaults` already points the build at this file:

```ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

### 1.3 Rollback Kconfig

Added to `sdkconfig.defaults`:

```ini
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

This is the **single canonical symbol** for the anti-rollback / app-validation
workflow in ESP-IDF v5.x. (There is no separate `CONFIG_APP_ROLLBACK_ENABLE` in
mainline IDF — that name does not exist as a Kconfig option, so it is
intentionally not added.) With this enabled, a freshly activated OTA image boots
in the `PENDING_VERIFY` state and is **rolled back to the previous slot on the
next reset unless the running app confirms a healthy boot** (see §4).

---

## 2. Building

OTA is an on-device feature. The ESP-IDF build produces the image you upload.

```bash
# (one-time) point at your ESP-IDF v5.x install
. $IDF_PATH/export.sh

# Pick the transport you ship; the display build is the large one (~1.5 MB).
idf.py -D SIP_TRANSPORT=display set-target esp32s3
idf.py -D SIP_TRANSPORT=display build

# The OTA image is the application binary:
#   build/SipServer.bin
```

The desktop/host build (`cmake -B build -S . && cmake --build build`) compiles
the same `OtaUpdater` and HTTP endpoints, but with **stubs** — there is no flash
to write, so the host cannot perform a real update (see §3.4).

---

## 3. Pushing an update

### 3.1 Endpoints

| Method & path           | Auth                                   | Behaviour |
|-------------------------|----------------------------------------|-----------|
| `POST /api/ota/upload`  | same-origin **and** (provisioned ⇒ session) | **Streaming.** Writes the request body into the inactive slot, validates, and stages it for boot. |
| `GET  /api/ota/status`  | none (read-only, no secrets)           | JSON: running / boot / next partition, pending-verify flag, `otaSupported`. |
| `POST /api/ota/reboot`  | same-origin **and** (provisioned ⇒ session) | Reboots into the staged image (device) / no-op simulation (host). |

The two mutating endpoints use the **exact same gate** as the existing mutating
endpoints (`/api/kill`, `/api/wifi/*`, `/api/factory-reset`): the request must
be same-origin, and once an admin PIN is provisioned it must also carry a valid
`pd_session` cookie. `/api/ota/upload` is **not** subject to the 16 KB request
body cap — it is intercepted before the buffered path and streamed (a firmware
image is >1.5 MB).

### 3.2 Curl walk-through (PIN provisioned)

```bash
DEVICE=http://192.168.4.1          # or http://pocketdial.local
JAR=cookies.txt

# 1) Log in with the admin PIN to obtain a pd_session cookie.
#    (Send Origin so the same-origin check passes from a script.)
curl -s -c "$JAR" \
     -H "Origin: $DEVICE" \
     -X POST --data "pin=YOUR_PIN" \
     "$DEVICE/api/admin/login"
# -> {"status":"ok","authenticated":true}

# 2) Inspect current OTA state (optional).
curl -s "$DEVICE/api/ota/status"
# -> {"running":"ota_0","boot":"ota_0","next":"ota_1","pendingVerify":false,"otaSupported":true,"error":""}

# 3) Stream the new firmware into the inactive slot.
#    Content-Length is set automatically by --data-binary @file.
curl -s -b "$JAR" \
     -H "Origin: $DEVICE" \
     -H "Content-Type: application/octet-stream" \
     -X POST --data-binary @build/SipServer.bin \
     "$DEVICE/api/ota/upload"
# -> {"status":"ok","bytes":1543210,"rebootRequired":true,"nextPartition":"ota_1",
#     "message":"image staged; POST /api/ota/reboot to boot it"}

# 4) Reboot into the new image.
curl -s -b "$JAR" \
     -H "Origin: $DEVICE" \
     -X POST "$DEVICE/api/ota/reboot"
# -> {"status":"ok","message":"rebooting into the new image..."}
```

If **no PIN is set yet** (fresh/unprovisioned device) you can skip step 1 — the
upload only requires same-origin in that state. **Set a PIN first in
production**; an open AP with an ungated OTA endpoint is a remote-compromise
risk (see §5).

### 3.3 Upload response codes

| Code | Meaning |
|------|---------|
| `200` | Image written, validated, and staged. `rebootRequired:true`. |
| `401` | PIN is provisioned but no/invalid `pd_session` cookie. |
| `403` | Cross-origin request rejected (CSRF guard). |
| `411` | Missing or zero `Content-Length` (the stream size is required). |
| `400` | Upload truncated / socket closed early, or a flash write failed mid-stream. |
| `422` | `esp_ota_end()` rejected the image (bad magic / corrupt / not a valid app), **or** the `X-OTA-Signature` failed to verify / was required and missing (issue #47). |
| `500` | `esp_ota_begin` / `esp_ota_set_boot_partition` failed. |
| `501` | **Host build only** — OTA is not available off-device. |

### 3.4 Host (desktop) build behaviour

The host binary is for development and CI smoke tests; it has no flash.

- `POST /api/ota/upload` — drains the request body (bounded by `Content-Length`
  and the existing 5 s per-socket receive timeout, so it never hangs) and
  returns **`501 {"error":"OTA only available on device"}`**. We deliberately
  return 501 rather than a simulated `200` so a real update can never be
  confused with the host stub in tooling/CI.
- `GET /api/ota/status` — returns valid JSON with placeholder partition labels
  (`"running":"host"`, …) and `"otaSupported":false`.
- `POST /api/ota/reboot` — returns `200 {"status":"ok","simulated":true,…}` and
  **does not exit the process** (the smoke-test harness keeps running).

---

## 4. Rollback strategy & failure handling

**Strategy: mark-valid-on-healthy-boot.** It is the simplest robust scheme and
is what `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is built for.

1. `POST /api/ota/upload` writes the image to the inactive slot and, on success,
   calls `esp_ota_set_boot_partition()` — the slot is now the boot choice but is
   marked `PENDING_VERIFY`.
2. `POST /api/ota/reboot` restarts the device into that slot.
3. On the **next** boot the bootloader sees `PENDING_VERIFY`. The application
   must affirm it is healthy by calling
   `esp_ota_mark_app_valid_cancel_rollback()` (exposed as
   `OtaUpdater::markValid()`), which flips the slot to `VALID`.
4. If the new image **crashes or boot-loops before** calling `markValid()`, the
   bootloader **automatically rolls back** to the previously valid slot on the
   subsequent reset. No bricking.

### 4.1 Integration in firmware `app_main` (wired in)

The application confirms a healthy boot from each firmware entry point's
`http_server_task` (`main/esp_main.cpp`, `esp_main_eth.cpp`, and
`esp_main_display.cpp`). After the SIP engine and HTTP dashboard are up, the
task waits a few seconds of stable operation, then confirms the image:

```cpp
#include "OtaUpdater.hpp"
// ... in the dashboard task's steady-state loop, after the servers are up:
if (OtaUpdater::isPendingVerify()) {
    OtaUpdater::markValid();   // confirm this image; cancel the pending rollback
}
```

The "few seconds of healthy operation" gate means a freshly OTA'd image that
crashes or boot-loops during startup never reaches `markValid()`, so the
bootloader rolls it back to the previous slot on the next reset. The check is
cheap and idempotent (`isPendingVerify()` is `false` on a normal boot), so it is
a no-op except immediately after an OTA.

> **Health-gate scope:** the confirmation runs in the normal operating path
> (the path a configured device follows after an OTA reboot). The display
> build's first-run *captive-onboarding* path does not confirm — that path is
> not reachable as a post-OTA boot of a configured device. A stricter health
> signal (e.g. confirm only after the first successful SIP REGISTER) is a
> possible future refinement.

### 4.2 Other failure modes

| Failure | Behaviour |
|---------|-----------|
| Upload truncated (Wi-Fi drop mid-stream) | `esp_ota_write` stream ends short → handler `abort()`s the session, returns `400`. The boot partition is unchanged; the device keeps running the current image. |
| Corrupt image | `esp_ota_end()` returns `ESP_ERR_OTA_VALIDATE_FAILED` → `422`. Slot not activated. |
| Power loss during write | The slot is partially written but never activated; `otadata` still points at the running slot. Next boot is the old image. Re-upload to retry. |
| New image boot-loops | Anti-rollback restores the previous slot — the image is only confirmed after a healthy boot reaches `markValid()` (§4.1). |

---

## 5. Migration from the single-`factory` layout

The previous table had one `factory` app partition at `0x10000` size `0x400000`.
Moving to dual-OTA **changes the partition table itself**, which the running
firmware cannot rewrite from inside an OTA. Therefore:

> **A one-time, full reflash over USB/JTAG is required to migrate a device from
> the old single-`factory` layout to the new dual-OTA layout.** After that,
> updates are OTA.

```bash
. $IDF_PATH/export.sh
idf.py -D SIP_TRANSPORT=display set-target esp32s3
idf.py -D SIP_TRANSPORT=display build
idf.py -p /dev/ttyUSB0 flash      # writes bootloader + new partition table + app to ota_0
```

### 5.1 NVS / re-onboarding caveat

`nvs` is kept at the **identical** offset (`0x9000`) and size (`0x6000`), so its
contents are *layout-preserved*. **However**, a full migration flash often erases
the whole chip depending on the method used:

- `idf.py flash` writes only the bootloader, partition table, and app — it does
  **not** explicitly erase `nvs`, so creds *may* survive.
- `idf.py erase-flash` (or a factory programming jig) **wipes everything**,
  including `nvs`.

Because the safe assumption differs per tool and the partition table offsets for
everything *after* `nvs` have shifted, **treat the migration as a clean slate**:

> After migrating a device to the dual-OTA layout, **re-onboard it** — reconnect
> WiFi and **re-set the admin PIN**. Do not assume the old WiFi password or PIN
> carried over.

---

## 6. Security

The controls on the upload path are:

- the **admin PIN / session** gate (once provisioned),
- the **same-origin / CSRF** check, and
- (issue #47) an **application-layer image signature check** — an ECDSA-P256
  signature over the streamed image's SHA-256, verified against a build-time OTA
  public key before the new slot is ever made bootable.

Without the signature check, anyone who (a) is on the local link and (b) holds the
admin PIN (or hits the first-run, pre-PIN window) could flash arbitrary firmware —
threats **T-5 (firmware/OTA tampering)** and **E-1** in
[THREAT_MODEL.md](THREAT_MODEL.md).

### 6.1 Application-layer signature verification (issue #47)

As the image streams into the inactive slot, `OtaUpdater` folds every byte into a
SHA-256 (PSA Crypto on-device). After `esp_ota_end()` validates the image format and
**before** the slot is activated, the upload handler calls
`OtaUpdater::verifySignature()` with the signature carried in the
**`X-OTA-Signature`** request header. Policy:

| Situation | Result |
|-----------|--------|
| Signature present and **verifies** | image staged (`200`) |
| Signature present and **fails** | `422`, image written to the slot but **never made bootable** |
| Signature absent, enforcement **ON** (`PD_OTA_REQUIRE_SIGNATURE`) | `422` — signature is mandatory |
| Signature absent, enforcement **OFF** (default) | accepted with a loud warning log |

Enforcement is **off by default** so first-run onboarding, the host/CI smoke path,
and units that have not yet been issued a signing key are not bricked. Turn it on
(and provision a key) for a production build.

**Key-provisioning hook (the human decision):** the trusted public key is compiled
in via two build macros on the `main` component (e.g. in `main/CMakeLists.txt`):

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PD_OTA_REQUIRE_SIGNATURE
    PD_OTA_PUBLIC_KEY_HEX="04ab...ef")   # hex of the 65-byte uncompressed P-256 point
```

Generate the keypair, derive the raw public point, and sign an image:

```bash
# 1) Keypair (KEEP ota_signing.pem OFFLINE / in an HSM — it is the master key).
openssl ecparam -name prime256v1 -genkey -noout -out ota_signing.pem
# 2) Public point as hex (the "pub:" block, 65 bytes incl. the 0x04 prefix).
openssl ec -in ota_signing.pem -text -noout
# 3) Sign the image's SHA-256 (DER), then convert DER -> raw r||s (64 bytes).
openssl dgst -sha256 -sign ota_signing.pem -out img.der.sig build/SipServer.bin
#    der2raw: extract r and s (32 bytes each) from the DER ECDSA-Sig-Value and
#    concatenate; e.g. with a tiny python/openssl asn1parse step. The header value
#    is base64(raw r||s).
curl ... -H "X-OTA-Signature: $(base64 -w0 img.raw.sig)" --data-binary @build/SipServer.bin .../api/ota/upload
```

> **What still needs a human key-management decision** (not code): where the OTA
> private key lives (HSM/offline), the per-fleet vs per-device key policy, how the
> public key is provisioned into the firmware at build time, and the key-rotation
> procedure. The P-256 key chosen here can be **promoted to the Secure Boot v2
> signing key**, so this app-layer check is a stepping stone, not a detour.

**Operational guidance:**

- **Always set an admin PIN in production.** An open AP with an ungated OTA
  endpoint is a remote persistent-compromise vector even with signing.
- **Restrict OTA to the local link** (the device is a LAN appliance; do not expose
  `/api/ota/*` to the internet).
- **Enable signature enforcement** (`PD_OTA_REQUIRE_SIGNATURE` + a provisioned key)
  for any fleet you do not physically control.

### 6.2 Roadmap (durable fix — see [THREAT_MODEL.md](THREAT_MODEL.md) §roadmap, P2)

The app-layer check above stops an **unsigned/forged image being staged over the
network**, but it does NOT stop a **physical** attacker reflashing over UART/JTAG,
because the running app does the checking. The durable fix moves the check into
hardware (commented hooks are in `sdkconfig.defaults`):

- **Secure Boot v2** — the bootloader verifies the app's ECDSA-P256 signature, so
  only images signed with your private key will **boot** (closes physical reflash).
- **Flash encryption** — protects NVS (WiFi password, admin hash, HA1) and the app
  against a physical flash read (threats T-4 / I-3 / I-6, issue #58).
- **`CONFIG_SECURE_SIGNED_ON_UPDATE`** — makes `esp_ota_*` itself enforce the
  appended signature on write, a second enforcement layer alongside this one.

These require burning eFuses (a one-way operation) and a secured factory flow, so
they stay **opt-in**; the partition layout, rollback workflow, and the app-layer
signature path shipped here are forward-compatible with all three.

---

## 7. File map (what this change touched)

| File | Change |
|------|--------|
| `partitions.csv` | Single `factory` → dual-OTA (`otadata` + `ota_0` + `ota_1`); `nvs` unchanged. |
| `sdkconfig.defaults` | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. |
| `src/Helpers/OtaUpdater.{hpp,cpp}` | Portable wrapper over the ESP-IDF `app_update` API; host stubs. |
| `src/Helpers/HttpServer.{hpp,cpp}` | Streaming `/api/ota/upload` interception + `/api/ota/status` + `/api/ota/reboot`. |
| `main/CMakeLists.txt` | Added `OtaUpdater.cpp` to `SRCS` and `app_update` to `REQUIRES`. |
| `docs/OTA.md` | This document. |
