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
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print(f"[patch_wolfssl] PATCHED {label}")
    else:
        print(f"[patch_wolfssl] OK (already patched) {label}")
    return True


def main():
    if len(sys.argv) < 2:
        print("usage: patch_wolfssl.py <managed_components_dir>", file=sys.stderr)
        return 2
    mc = sys.argv[1]
    wolfssl = os.path.join(mc, "wolfssl__wolfssl")
    # wolfSSH ships as a sibling managed component. The pty-req source fix (Fix 4) lives
    # in its tree; patch_file() no-ops cleanly if it is absent (e.g. a build that pulled
    # wolfSSL but not wolfSSH).
    wolfssh = os.path.join(mc, "wolfssl__wolfssh")
    if not os.path.isdir(wolfssl):
        # wolfSSL not fetched (e.g. non-display build) — nothing to do.
        print(f"[patch_wolfssl] wolfssl__wolfssl not present under {mc}; nothing to patch")
        return 0

    ok = True

    user_settings = os.path.join(wolfssl, "include", "user_settings.h")

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

    # Fix 4 (B2a): accept an interactive pty-req channel on ESP-IDF.
    #
    # Root cause (verified on hardware: ssh_probe_nopty.py works, ssh_test.py with
    # chan.get_pty() drops the channel "before any banner"):
    #   wolfSSH 1.4.20's DoChannelRequest() pty-req branch (wolfssh/src/internal.c,
    #   gated on WOLFSSH_TERM which is ALREADY enabled by the stock wolfSSL 5.8.2
    #   ESP_ENABLE_WOLFSSH block) handles the RFC-4254 6.2 terminal modes string as:
    #
    #       ssh->modes = (byte*)WMALLOC(modesSz, ssh->ctx->heap, DYNTYPE_STRING);
    #       if (ssh->modes == NULL)
    #           ret = WS_MEMORY_E;
    #
    #   paramiko (and OpenSSH/PuTTY in the common case) send an EMPTY modes string,
    #   i.e. modesSz == 0, so this is WMALLOC(0, ...). On ESP-IDF with heap poisoning
    #   OFF (the default), malloc(0) returns NULL — see the ESP-IDF "Heap Memory
    #   Allocation" guide. The NULL is then misread as OOM: ret = WS_MEMORY_E. That
    #   makes DoChannelRequest reply CHANNEL_FAILURE to the pty-req AND return an error
    #   up through DoReceive(), so wolfSSH_accept() fails and SshServer closes the
    #   socket before it can print the banner. A non-pty shell request carries no modes
    #   field, never hits this malloc, and works — exactly the observed split.
    #
    # Fix: treat a zero-length modes string as "no modes" (the correct semantics) and
    # skip the allocation/copy entirely. ssh->modes stays NULL, ssh->modesSz stays 0,
    # the terminal dimensions are still recorded on the session (ssh->widthChar /
    # ssh->heightRows), and the request is answered with CHANNEL_SUCCESS. Only the
    # zero-length path changes; a client that DOES send modes is unaffected. This is a
    # one-spot, behaviour-preserving guard applied idempotently to the (git-ignored)
    # managed component, mirroring the other source fixes here.
    internal_c = os.path.join(wolfssh, "src", "internal.c")
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

    # Fix 5 (B2a): GetStringRef() off-by-one rejected a 0-length string at buffer end.
    #
    # Root cause (pinpointed on hardware with a PTYDBG printf trace:
    #   "pty-req parsed ret=-1004 begin=45 len=45 modesSz=0" — i.e. all bytes consumed,
    #   yet WS_BUFFER_E): GetStringRef() guarded with `*idx < len` where the sibling
    #   GetString() uses `*idx <= len` and is commented "allows 0 length string". The
    #   pty-req "terminal modes" string (RFC-4254 6.2) is the LAST field of the request
    #   and is EMPTY for paramiko/OpenSSH/PuTTY. After reading its 4-byte length the
    #   cursor sits exactly at len, so `*idx < len` is false and GetStringRef returns
    #   WS_BUFFER_E -> DoChannelRequest fails the pty-req -> wolfSSH_accept() returns
    #   WS_FATAL_ERROR and SshServer closes the socket before the banner. A non-pty
    #   "shell" request has no trailing empty string and never hits this.
    #
    # Fix: `<= len`, matching GetString() and RFC-4254 (a 0-length string at the buffer
    # end is valid). Needed together with Fix 4: this lets the empty modes field parse;
    # Fix 4 then avoids the WMALLOC(0). Behaviour-preserving for non-empty strings.
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
