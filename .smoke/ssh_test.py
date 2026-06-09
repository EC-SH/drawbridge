#!/usr/bin/env python3
"""Scripted SSH smoke test for the Pocket-Dial wolfSSH console.

Usage: python ssh_test.py <host> [password]

The device is OPEN (any password) while unsecured; once an admin PIN is set the
password must be that PIN. Uses paramiko's low-level Transport so we don't touch
the known_hosts file, drives the interactive line shell, and prints what comes back.

Exit 0 if authenticated and at least one command produced output.
"""
import sys
import time

try:
    import paramiko
except ImportError:
    print("ERROR: paramiko not installed", file=sys.stderr)
    sys.exit(2)

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.159"
pw   = sys.argv[2] if len(sys.argv) > 2 else "pocketdial"

print("[ssh] connecting to %s:22 ..." % host)
sock_ok = False
try:
    t = paramiko.Transport((host, 22))
    t.banner_timeout = 20
    t.start_client(timeout=20)
    sock_ok = True
    hk = t.get_remote_server_key()
    print("[ssh] host key:", hk.get_name(), "(%d bits)" % hk.get_bits())
    t.auth_password(username="admin", password=pw)
    print("[ssh] authenticated:", t.is_authenticated())
    if not t.is_authenticated():
        print("[ssh] RESULT: AUTH FAILED")
        t.close(); sys.exit(1)

    chan = t.open_session(timeout=10)
    chan.get_pty()
    chan.invoke_shell()
    time.sleep(0.6)

    def drain():
        out = b""
        end = time.time() + 2.0
        while time.time() < end:
            if chan.recv_ready():
                out += chan.recv(8192)
                end = time.time() + 0.4
            else:
                time.sleep(0.1)
        return out.decode(errors="replace")

    print("---- banner ----")
    print(drain(), end="")
    saw_output = False
    for cmd in ["help", "show status"]:
        chan.send(cmd + "\r")
        time.sleep(0.4)
        resp = drain()
        if resp.strip():
            saw_output = True
        print("---- after '%s' ----" % cmd)
        print(resp, end="")

    chan.send("exit\r")
    time.sleep(0.3)
    t.close()
    print("\n[ssh] RESULT:", "OK" if saw_output else "CONNECTED (no command output)")
    sys.exit(0 if saw_output else 1)

except Exception as e:
    print("[ssh] ERROR (%s): %s" % ("post-connect" if sock_ok else "connect", e))
    print("[ssh] RESULT: FAIL")
    sys.exit(1)
