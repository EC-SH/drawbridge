#!/usr/bin/env python3
"""Idempotently patch the wolfSSL ESP managed component for ESP-IDF v6.0.1.

The wolfssl/wolfssl 5.8.2 ESP-Component Registry release does not build cleanly
on ESP-IDF v6.0.1 out of the box (it targets the v5 toolchain/headers). Because
the component lives under the git-ignored managed_components/ tree (it is fetched
by the IDF component manager and regenerated on `idf.py update-dependencies`),
we cannot commit the fixes directly. Instead this script re-applies them at
CMake configure time. It is safe to run repeatedly (each edit is guarded).

Five fixes (root-caused during the B1 SSH spike and the B2a pty-req work; fixes
4 and 5 are documented inline at their patch sites):

  1. FORCE SOFTWARE CRYPTO (NO_ESP32_CRYPT) for the ESP32-S3 branch of
     include/user_settings.h. The Espressif HW-crypto port pulls in v5-only
     peripheral headers removed in IDF v6 (driver/periph_ctrl.h,
     hal/clk_gate_ll.h) and leaves wc_ShaXXX.ctx half-defined, breaking
     wolfcrypt/src/sha512.c. Software crypto avoids those headers entirely.

  2. ENABLE THE wolfSSH FEATURE BLOCK by defining ESP_ENABLE_WOLFSSH near the
     top of include/user_settings.h (it gates WOLFSSL_WOLFSSH, WOLFSSL_KEY_GEN,
     and the ECC/ED25519/AES/SHA set the SSH server needs). Defined in the
     shared user_settings.h so both the wolfssl and wolfssh components see it.

  3. RENAME the 'thread_local' enum constant in
     wolfcrypt/src/port/Espressif/esp_sdk_mem_lib.c. Under IDF v6.0.1's default
     -std=gnu23, 'thread_local' is a reserved C23 keyword and cannot be an enum
     identifier; the original name broke the whole enum.

Usage:  python patch_wolfssl.py <managed_components_dir>
Exit 0 on success (including "already patched"); non-zero on real failure.
"""
import sys
import os


def patch_file(path, replacements, label, marker):
    """Apply (old, new) replacements to `path`.

    Idempotent: if `marker` (a short string unique to the applied patch) is
    already present anywhere in the file, the patch is considered done and the
    file is left untouched. This is robust even if the surrounding text was
    hand-edited, as long as the marker is present."""
    if not os.path.isfile(path):
        print(f"[patch_wolfssl] SKIP {label}: not found at {path}")
        return True  # component layout may differ; don't hard-fail the build
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    # A git checkout on Windows (FetchContent) may be CRLF; the anchors are LF.
    # Match/patch on an LF-normalized copy and restore CRLF on write.
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    if marker in text:
        print(f"[patch_wolfssl] OK (already patched) {label}")
        return True
    original = text
    for old, new in replacements:
        if old not in text:
            print(f"[patch_wolfssl] WARN {label}: anchor not found, "
                  f"component may have changed:\n    {old[:70]!r}")
            return False
        text = text.replace(old, new, 1)
    if text != original:
        if crlf:
            text = text.replace("\n", "\r\n")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print(f"[patch_wolfssl] PATCHED {label}")
    else:
        print(f"[patch_wolfssl] OK (already patched) {label}")
    return True


def apply_ptyreq_fixes(internal_c):
    """Fixes 4 & 5 (B2a): make wolfSSH 1.4.20 accept an interactive pty-req.

    Shared by both layouts — the ESP managed component and the upstream git
    tree (host FetchContent build) carry the same two bugs. Full root-cause
    notes at the patch sites in main()'s original commit; in short:
      Fix 4: WMALLOC(0) for the (empty) RFC-4254 terminal-modes string returns
             NULL and was misread as OOM — skip the alloc when modesSz == 0.
      Fix 5: GetStringRef() used `*idx < len` (vs GetString()'s `<=`), rejecting
             a 0-length string whose length field sits exactly at buffer end —
             which is precisely the empty modes field OpenSSH/PuTTY/paramiko send.
    """
    ok = True
    ok &= patch_file(internal_c, [(
        '            if (ret == WS_SUCCESS) {\n'
        '                ssh->modes = (byte*)WMALLOC(modesSz,\n'
        '                        ssh->ctx->heap, DYNTYPE_STRING);\n'
        '                if (ssh->modes == NULL)\n'
        '                    ret = WS_MEMORY_E;\n'
        '            }\n'
        '            if (ret == WS_SUCCESS) {\n'
        '                ssh->modesSz = modesSz;\n'
        '                WMEMCPY(ssh->modes, modes, modesSz);\n',
        '            /* pocket-dial B2a: a 0-length modes string (paramiko/OpenSSH/PuTTY\n'
        '             * default) means "no modes". WMALLOC(0) is malloc(0), which returns\n'
        '             * NULL on ESP-IDF (heap poisoning off) and was being misread as OOM,\n'
        '             * failing the pty-req. Only allocate/copy when modesSz > 0. */\n'
        '            if (ret == WS_SUCCESS && modesSz > 0) {\n'
        '                ssh->modes = (byte*)WMALLOC(modesSz,\n'
        '                        ssh->ctx->heap, DYNTYPE_STRING);\n'
        '                if (ssh->modes == NULL)\n'
        '                    ret = WS_MEMORY_E;\n'
        '            }\n'
        '            if (ret == WS_SUCCESS) {\n'
        '                ssh->modesSz = modesSz;\n'
        '                if (modesSz > 0)\n'
        '                    WMEMCPY(ssh->modes, modes, modesSz);\n',
    )], "internal.c: pty-req zero-length modes guard",
        marker="pocket-dial B2a")
    ok &= patch_file(internal_c, [(
        '        if (*idx < len && *strSz <= len - *idx) {\n'
        '            *str = buf + *idx;\n'
        '            *idx += *strSz;\n'
        '            result = WS_SUCCESS;\n'
        '        }',
        '        /* pocket-dial B2a Fix5: <= len (was < len) so a 0-length string whose\n'
        '         * 4-byte length sits exactly at buf end decodes, mirroring GetString().\n'
        '         * The empty pty-req "modes" field (paramiko/OpenSSH/PuTTY) hits this. */\n'
        '        if (*idx <= len && *strSz <= len - *idx) {\n'
        '            *str = buf + *idx;\n'
        '            *idx += *strSz;\n'
        '            result = WS_SUCCESS;\n'
        '        }',
    )], "internal.c: GetStringRef 0-length string at buffer end",
        marker="pocket-dial B2a Fix5")
    return ok


def main():
    if len(sys.argv) < 2:
        print("usage: patch_wolfssl.py <managed_components_dir>", file=sys.stderr)
        return 2
    mc = sys.argv[1]
    # Two layouts are supported:
    #   * ESP-IDF managed components:  <dir>/wolfssl__wolfssl, <dir>/wolfssl__wolfssh
    #   * CMake FetchContent (_deps):  <dir>/wolfssl-src,      <dir>/wolfssh-src
    # The FetchContent host build only needs the wolfSSH pty-req fixes (4 & 5);
    # the ESP-only files (user_settings.h, esp_sdk_mem_lib.c) are absent there and
    # patch_file() no-ops on missing paths.
    wolfssl = os.path.join(mc, "wolfssl__wolfssl")
    wolfssh = os.path.join(mc, "wolfssl__wolfssh")
    esp_layout = True
    if not os.path.isdir(wolfssl) and os.path.isdir(os.path.join(mc, "wolfssl-src")):
        wolfssl = os.path.join(mc, "wolfssl-src")
        esp_layout = False
    if not os.path.isdir(wolfssh) and os.path.isdir(os.path.join(mc, "wolfssh-src")):
        wolfssh = os.path.join(mc, "wolfssh-src")
        esp_layout = False
    if not os.path.isdir(wolfssl) and not os.path.isdir(wolfssh):
        # Neither fetched (e.g. a non-display ESP build) — nothing to do.
        print(f"[patch_wolfssl] no wolfssl/wolfssh tree under {mc}; nothing to patch")
        return 0

    ok = True

    user_settings = os.path.join(wolfssl, "include", "user_settings.h")

    # Fixes 1/2/3/6 patch the ESP component's user_settings.h / Espressif port
    # files — they exist only in the managed-component layout. The upstream git
    # tree (FetchContent host build) configures via CMake instead and only needs
    # the wolfSSH pty-req fixes (4 & 5) below.
    if not esp_layout:
        internal_c = os.path.join(wolfssh, "src", "internal.c")
        ok &= apply_ptyreq_fixes(internal_c)
        return 0 if ok else 1

    # Fix 2: enable wolfSSH feature block (inserted right after sdkconfig.h include).
    ok &= patch_file(user_settings, [(
        '/* The Espressif project config file. See also sdkconfig.defaults */\n'
        '#include "sdkconfig.h"\n',
        '/* The Espressif project config file. See also sdkconfig.defaults */\n'
        '#include "sdkconfig.h"\n\n'
        '/* pocket-dial: activate the wolfSSH feature block (WOLFSSL_WOLFSSH,\n'
        ' * WOLFSSL_KEY_GEN, ECC/ED25519/AES/SHA needed by the SSH server). */\n'
        '#ifndef ESP_ENABLE_WOLFSSH\n'
        '    #define ESP_ENABLE_WOLFSSH\n'
        '#endif\n',
    )], "user_settings.h: ESP_ENABLE_WOLFSSH",
        marker="#ifndef ESP_ENABLE_WOLFSSH")

    # Fixes 4 & 5 (B2a): the wolfSSH pty-req fixes — shared with the host
    # FetchContent layout; see apply_ptyreq_fixes() for the root-cause notes.
    internal_c = os.path.join(wolfssh, "src", "internal.c")
    ok &= apply_ptyreq_fixes(internal_c)

    # Fix 1: force software crypto on the ESP32-S3 branch.
    ok &= patch_file(user_settings, [(
        '#elif defined(CONFIG_IDF_TARGET_ESP32S3)\n'
        '    #define WOLFSSL_ESP32\n'
        '    /* wolfSSL HW Acceleration supported on ESP32-S3. Uncomment to disable: */\n'
        '    /*  #define NO_ESP32_CRYPT                         */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_HASH            */\n'
        '    /* Note: There\'s no AES192 HW on the ESP32-S3; falls back to SW */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_AES             */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI         */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MP_MUL  */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MULMOD  */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_EXPTMOD */\n'
        '    /***** END CONFIG_IDF_TARGET_ESP32S3 *****/',
        '#elif defined(CONFIG_IDF_TARGET_ESP32S3)\n'
        '    #define WOLFSSL_ESP32\n'
        '    /* pocket-dial: FORCE SOFTWARE CRYPTO on ESP-IDF v6.0.1 (HW port uses\n'
        '     * v5-only headers removed in v6). NO_ESP32_CRYPT bypasses the entire port. */\n'
        '    #define NO_ESP32_CRYPT\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_HASH\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_AES\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MP_MUL\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MULMOD\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_EXPTMOD\n'
        '    /***** END CONFIG_IDF_TARGET_ESP32S3 *****/',
    )], "user_settings.h: NO_ESP32_CRYPT (ESP32-S3)",
        # Marker = the uncommented EXPTMOD line directly before the S3 end banner.
        # In the pristine file this line is commented out, so it is unique to the patch.
        marker='    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_EXPTMOD\n'
               '    /***** END CONFIG_IDF_TARGET_ESP32S3 *****/')

    # Fix 6: same software-crypto force for the CLASSIC ESP32 branch (the lan8720
    # transport builds for esp32, not esp32s3). Without this, esp32_sha.c includes
    # hal/clk_gate_ll.h (removed in IDF v6) and the build dies at
    # "fatal error: hal/clk_gate_ll.h: No such file or directory". Only the esp32 and
    # esp32s3 branches are patched because those are the only targets this project
    # builds (display/eth/wifi -> esp32s3, lan8720 -> esp32); the S2/C2/C3/C6 branches
    # would need the same treatment if a transport ever targets them.
    ok &= patch_file(user_settings, [(
        '    /* wolfSSL HW Acceleration supported on ESP32. Uncomment to disable: */\n'
        '    /*  #define NO_ESP32_CRYPT                 */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_HASH    */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_AES     */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MP_MUL  */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MULMOD  */\n'
        '    /*  #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_EXPTMOD */',
        '    /* wolfSSL HW Acceleration supported on ESP32. Uncomment to disable: */\n'
        '    /* pocket-dial: FORCE SOFTWARE CRYPTO on ESP-IDF v6.0.1 (classic-ESP32 HW\n'
        '     * port includes hal/clk_gate_ll.h, removed in v6 - see esp32_sha.c:61).\n'
        '     * Mirrors the ESP32-S3 fix; required for the lan8720 transport. */\n'
        '    #define NO_ESP32_CRYPT\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_HASH\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_AES\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MP_MUL\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_MULMOD\n'
        '    #define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI_EXPTMOD',
    )], "user_settings.h: NO_ESP32_CRYPT (classic ESP32)",
        marker="FORCE SOFTWARE CRYPTO on ESP-IDF v6.0.1 (classic-ESP32 HW")

    # Fix 3: rename the C23-reserved 'thread_local' enum constant.
    mem_lib = os.path.join(
        wolfssl, "wolfcrypt", "src", "port", "Espressif", "esp_sdk_mem_lib.c")
    ok &= patch_file(mem_lib, [
        ('    mem_map_io = 0,\n    thread_local,\n    data,',
         '    mem_map_io = 0,\n    thread_local_seg, /* pocket-dial: C23 keyword rename */\n    data,'),
        ('sdk_log_meminfo(thread_local,  _thread_local_start',
         'sdk_log_meminfo(thread_local_seg,  _thread_local_start'),
    ], "esp_sdk_mem_lib.c: thread_local -> thread_local_seg",
        marker="thread_local_seg")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
