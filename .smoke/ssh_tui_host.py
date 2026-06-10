#!/usr/bin/env python3
"""Render the sysop TUI from the HOST build's SSH listener (PD_HOST_SSH).

Same pyte-reconstruction approach as ssh_tui.py, but host/port come from argv
(the host exe defaults to port 2222) and it only walks banner -> hub -> about.

Usage: python ssh_tui_host.py [host] [port] [password]
"""
import sys, time
import paramiko
try:
    import pyte
except ImportError:
    print("ERROR: pip install pyte", file=sys.stderr); sys.exit(2)

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
OUT = open(".smoke/tui_render_host.txt", "w", encoding="utf-8")
def emit(s):
    print(s)
    OUT.write(s + "\n"); OUT.flush()

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 2222
pw   = sys.argv[3] if len(sys.argv) > 3 else "x"

t = paramiko.Transport((host, port)); t.banner_timeout = 20
t.start_client(timeout=20)
t.auth_password("sysop", pw)
print("[tui-host] authenticated:", t.is_authenticated())
if not t.is_authenticated():
    sys.exit(1)

chan = t.open_session(timeout=10)
chan.get_pty(term="xterm", width=80, height=24)
chan.invoke_shell()

screen = pyte.Screen(80, 24)
stream = pyte.ByteStream(screen)

def grab(label, settle=0.7):
    time.sleep(settle)
    data = b""
    end = time.time() + 1.5
    while time.time() < end:
        if chan.recv_ready():
            data += chan.recv(65536); end = time.time() + 0.4
        else:
            time.sleep(0.05)
    stream.feed(data)
    emit("\n+" + "-"*80 + "+")
    emit("| FRAME: %-71s |" % label[:71])
    emit("+" + "-"*80 + "+")
    for row in screen.display:
        emit("|" + row + "|")
    emit("+" + "-"*80 + "+ (%d bytes)" % len(data))
    return len(data)

n1 = grab("LOGIN BANNER")
chan.send(" ")
n2 = grab("MAIN HUB")
chan.send("6")
n3 = grab("[6] ABOUT")
chan.send("\x1b")
chan.send("L")
time.sleep(0.4)
t.close()
print("\n[tui-host] done; frames bytes:", n1, n2, n3)
sys.exit(0 if (n1 > 0 and n2 > 0) else 1)
