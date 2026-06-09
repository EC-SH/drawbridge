#!/usr/bin/env python3
"""Render the pocket-dial ANSI sysop TUI over SSH through a real terminal emulator.

Drives banner -> hub -> [T] theme -> [?] help -> Esc -> [3] placeholder -> [L] logout,
feeding the raw byte stream into a pyte 80x24 screen and printing each reconstructed
frame exactly as a terminal user would see it (cursor positioning resolved, layout intact).

Usage: python ssh_tui.py <host> [password]
"""
import sys, time
import paramiko
try:
    import pyte
except ImportError:
    print("ERROR: pip install pyte", file=sys.stderr); sys.exit(2)

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
OUT = open(".smoke/tui_render.txt", "w", encoding="utf-8")
def emit(s):
    print(s)
    OUT.write(s + "\n"); OUT.flush()

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.159"
pw   = sys.argv[2] if len(sys.argv) > 2 else "x"

t = paramiko.Transport((host, 22)); t.banner_timeout = 20
t.start_client(timeout=20)
t.auth_password("admin", pw)
print("[tui] authenticated:", t.is_authenticated())
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

grab("LOGIN BANNER")
chan.send(" ");      grab("MAIN HUB")
# [3] PBX CONFIG - walk the tabs (cancel all modals; no NVS mutation)
chan.send("3");      grab("[3] PBX - Extensions tab (footer: no [D]Del)")
chan.send("\t");     grab("PBX - Ring Groups tab (no [F]; footer fits)")
chan.send("\t");     grab("PBX - Forwards/DND tab")
chan.send("\r");     grab("Forward editor modal ('Forwards · ext' - top border square?)")
chan.send(" ");      grab("Forward target picker (extensions only - NO ring groups)")
chan.send("\x1b");   chan.send("\x1b")
chan.send("\t");     grab("PBX - IVR tab (footer: no [T])")
chan.send("\t");     grab("PBX - Features star-code card")
chan.send("\x1b")
chan.send("L")
time.sleep(0.4)
t.close()
print("\n[tui] done")
