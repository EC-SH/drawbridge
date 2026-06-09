#!/usr/bin/env python3
"""Render the digest-auth / Learn-mode TUI screens over SSH through a real
terminal emulator (pyte), to hardware-verify the width fixes from the code review:

  * [4] SECURITY  ............ footer must show [?] Help (was truncated)
  * [4]/[D] REGISTRAR DEVICES  footer [?] Help; mode block + blurb + roster fit;
                               result row reserved (18-row body, 80x24 frame)
  * [D]/[M] mode chooser ..... the LEARN blurb must fit at the 6-col indent
  * [3] PBX Extensions tab ... SEC column 9-wide so "* SECURED" doesn't bleed DND
  * SIP secret modal ......... the HA1 note is split across two <=53-col lines

Usage: python .smoke/ssh_tui_digest.py [host] [pin]
"""
import sys, time
import paramiko
import pyte

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
OUT = open(".smoke/tui_render_digest.txt", "w", encoding="utf-8")
def emit(s):
    print(s); OUT.write(s + "\n"); OUT.flush()

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.159"
pins = [sys.argv[2]] if len(sys.argv) > 2 else ["1234", "x", "admin"]

def connect():
    last = None
    deadline = time.time() + 75   # board may still be rebooting after a flash
    while time.time() < deadline:
        for pw in pins:
            try:
                t = paramiko.Transport((host, 22))
                t.banner_timeout = 20
                t.start_client(timeout=20)
                t.auth_password("admin", pw)
                if t.is_authenticated():
                    emit("[tui] authenticated with pin=%r" % pw)
                    return t
                t.close()
            except Exception as e:
                last = e
                try: t.close()
                except Exception: pass
        time.sleep(3)
    emit("[tui] could not connect/auth: %r" % last)
    sys.exit(1)

t = connect()
chan = t.open_session(timeout=10)
chan.get_pty(term="xterm", width=80, height=24)
chan.invoke_shell()

screen = pyte.Screen(80, 24)
stream = pyte.ByteStream(screen)

def grab(label, settle=0.8):
    time.sleep(settle)
    data = b""
    end = time.time() + 1.8
    while time.time() < end:
        if chan.recv_ready():
            data += chan.recv(65536); end = time.time() + 0.5
        else:
            time.sleep(0.05)
    stream.feed(data)
    emit("\n+" + "-"*80 + "+")
    emit("| FRAME: %-71s |" % label[:71])
    emit("+" + "-"*80 + "+")
    for row in screen.display:
        emit("|" + row + "|")
    emit("+" + "-"*80 + "+ (%d bytes)" % len(data))

ESC = "\x1b"
grab("LOGIN BANNER", settle=1.2)
chan.send(" ");   grab("MAIN HUB")
chan.send("4");   grab("[4] SECURITY  (check footer: [?] Help present)")
chan.send("d");   grab("[4]/[D] REGISTRAR / DEVICES  (mode block, roster, footer)")
chan.send("m");   grab("[D]/[M] MODE CHOOSER  (LEARN blurb must fit)")
chan.send(ESC);   grab("back -> DEVICES")
chan.send(ESC);   grab("back -> SECURITY")
chan.send(ESC);   grab("back -> HUB")
chan.send("3");   grab("[3] PBX CONFIG - Extensions tab  (SEC column 9-wide)")
chan.send("\x1b[B"); grab("Extensions: Down (select first ext)")  # arrow-down
chan.send("s");   grab("SIP SECRET MODAL  (HA1 note split across 2 lines)")
chan.send(ESC)
chan.send("L")    # logout
emit("\n[tui] done")
try: t.close()
except Exception: pass
