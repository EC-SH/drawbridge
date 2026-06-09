#!/usr/bin/env python3
"""One-off diagnostic: does the wolfSSH console answer WITHOUT a pty-req?

If the banner/commands come through here but not in ssh_test.py (which calls
get_pty()), the server is rejecting the pty-req channel request — the thing the
ANSI TUI will need. Usage: python ssh_probe_nopty.py <host> [password]
"""
import sys, time
import paramiko

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.159"
pw   = sys.argv[2] if len(sys.argv) > 2 else "pocketdial"

t = paramiko.Transport((host, 22)); t.banner_timeout = 20
t.start_client(timeout=20)
print("[probe] host key:", t.get_remote_server_key().get_name())
t.auth_password(username="admin", password=pw)
print("[probe] authenticated:", t.is_authenticated())

chan = t.open_session(timeout=10)
# NOTE: deliberately NO get_pty() — just a shell channel
chan.invoke_shell()
time.sleep(0.8)

def drain():
    out = b""; end = time.time() + 2.0
    while time.time() < end:
        if chan.recv_ready():
            out += chan.recv(8192); end = time.time() + 0.4
        else:
            time.sleep(0.1)
    return out.decode(errors="replace")

print("---- banner (no pty) ----"); print(drain(), end="")
saw = False
for cmd in ["help", "show status"]:
    chan.send(cmd + "\r"); time.sleep(0.4)
    r = drain()
    if r.strip(): saw = True
    print("---- after '%s' ----" % cmd); print(r, end="")
chan.send("exit\r"); time.sleep(0.3); t.close()
print("\n[probe] RESULT:", "SHELL WORKS WITHOUT PTY" if saw else "still no output")
sys.exit(0 if saw else 1)
