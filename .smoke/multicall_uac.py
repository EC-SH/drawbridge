#!/usr/bin/env python3
"""Multi-call SIP UAC — drives N concurrent outbound calls through drawbridge to the
upstream MoH test target (dial 9807) so the WAN anchor's concurrent-call capacity can be
measured (#100, "8 simultaneous calls").

Each call: REGISTER a unique extension -> INVITE the dest with an SDP offer -> ACK the
200 OK -> stream µ-law silence RTP while counting the MoH RTP coming back -> BYE. Run N
of these concurrently and report how many reached two-way media. Pure stdlib (no deps).

Usage:
    python multicall_uac.py <board_ip> <n_calls> [dest=9807] [hold_sec=8] [ext_base=191]

Watch the board's /api/status (tlsSocketsEst, mediaActive, freeHeap) + COM5 serial while
this runs to find the real concurrent-call edge.
"""
import socket, sys, threading, time, hashlib, random

def now_ms(): return int(time.time() * 1000)

def local_ip_for(target):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target, 5060)); return s.getsockname()[0]
    except OSError:
        return "0.0.0.0"
    finally:
        s.close()

def rtp_silence(seq, ts, ssrc):
    # RTP header (V=2, PT=0/PCMU) + 160 bytes of µ-law silence (0xFF)
    hdr = bytes([0x80, 0x00, (seq >> 8) & 0xFF, seq & 0xFF,
                 (ts >> 24) & 0xFF, (ts >> 16) & 0xFF, (ts >> 8) & 0xFF, ts & 0xFF,
                 (ssrc >> 24) & 0xFF, (ssrc >> 16) & 0xFF, (ssrc >> 8) & 0xFF, ssrc & 0xFF])
    return hdr + (b'\xff' * 160)

class Call:
    def __init__(self, board_ip, ext, dest, hold_sec, local_ip):
        self.board_ip, self.ext, self.dest = board_ip, ext, dest
        self.hold_sec, self.local_ip = hold_sec, local_ip
        self.result = "init"; self.rtp_rx = 0

    def _sip(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((self.local_ip, 0)); s.settimeout(5.0)
        return s, s.getsockname()[1]

    def _beep_ok(self, sip, invite):
        # Reply 486 Busy Here to the board's register-beep INVITE so it stops retransmitting and
        # doesn't pollute this socket while we wait for the REGISTER 200. Echo the request's routing
        # headers (Via/From/To/Call-ID/CSeq) back verbatim per RFC 3261.
        try:
            lines = invite.decode(errors="replace").split("\r\n")
            keep = []
            for h in lines:
                lk = h.lower()
                if lk.startswith(("via:", "from:", "call-id:", "cseq:")):
                    keep.append(h)
                elif lk.startswith("to:"):
                    keep.append(h if "tag=" in h else h + ";tag=uacbeep%d" % random.randint(1000, 9999))
            resp = ("SIP/2.0 486 Busy Here\r\n" + "\r\n".join(keep) + "\r\nContent-Length: 0\r\n\r\n").encode()
            sip.sendto(resp, (self.board_ip, 5060))
        except Exception:
            pass

    def run(self):
        try:
            self._run()
        except Exception as e:
            self.result = "EXC:%s" % e

    def _run(self):
        sip, sip_port = self._sip()
        rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); rtp.bind((self.local_ip, 0)); rtp.settimeout(0.05)
        rtp_port = rtp.getsockname()[1]
        tag = "uac%d-%d" % (self.ext, random.randint(1000, 9999))
        callid = "mc-%d-%d@%s" % (self.ext, now_ms(), self.local_ip)
        contact = "<sip:%d@%s:%d>" % (self.ext, self.local_ip, sip_port)

        # 1. REGISTER (Open/Learn registrar -> 200, no digest). The board sends a "register beep"
        # INVITE back to a freshly-registered contact, which can arrive before (or instead of) the
        # REGISTER 200 on this socket; and under a concurrent ramp a single REGISTER/200 can be
        # dropped. So: loop recvfrom looking specifically for the REGISTER 200 (ignore the beep
        # INVITE — answer it 200 so it doesn't retransmit), and retry the whole REGISTER a few times.
        registered = False
        for attempt in range(4):
            reg = ("REGISTER sip:%s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%d;branch=z9hG4bK%d\r\n"
                   "Max-Forwards: 70\r\nFrom: <sip:%d@%s>;tag=%s\r\nTo: <sip:%d@%s>\r\n"
                   "Call-ID: reg-%s\r\nCSeq: %d REGISTER\r\nContact: %s\r\nExpires: 120\r\nContent-Length: 0\r\n\r\n"
                   % (self.board_ip, self.local_ip, sip_port, random.randint(1, 1 << 30),
                      self.ext, self.board_ip, tag, self.ext, self.board_ip, callid, attempt + 1, contact)).encode()
            sip.sendto(reg, (self.board_ip, 5060))
            deadline = time.time() + 2.5
            while time.time() < deadline:
                try:
                    data, _ = sip.recvfrom(4096)
                except socket.timeout:
                    break
                first = data.split(b"\r\n", 1)[0]
                if b"REGISTER" in data and b" 200 " in first:
                    registered = True
                    break
                if data.startswith(b"INVITE"):
                    # register-beep: ACK-less 200 so the board stops retransmitting, then keep waiting
                    self._beep_ok(sip, data)
            if registered:
                break
        if not registered:
            self.result = "REG-TIMEOUT"; return

        # 2. INVITE dest with SDP offer (our RTP port)
        sdp = ("v=0\r\no=uac %d %d IN IP4 %s\r\ns=mc\r\nc=IN IP4 %s\r\nt=0 0\r\n"
               "m=audio %d RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n"
               % (now_ms(), now_ms(), self.local_ip, self.local_ip, rtp_port))
        branch = "z9hG4bK%d" % random.randint(1, 1 << 30)
        inv = ("INVITE sip:%s@%s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%d;branch=%s\r\n"
               "Max-Forwards: 70\r\nFrom: <sip:%d@%s>;tag=%s\r\nTo: <sip:%s@%s>\r\n"
               "Call-ID: %s\r\nCSeq: 1 INVITE\r\nContact: %s\r\n"
               "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s"
               % (self.dest, self.board_ip, self.local_ip, sip_port, branch, self.ext, self.board_ip,
                  tag, self.dest, self.board_ip, callid, contact, len(sdp), sdp)).encode()
        sip.sendto(inv, (self.board_ip, 5060))

        # 3. Await 200 OK (skip 100/180); grab To-tag + remote SDP RTP endpoint
        to_tag = None; remote_rtp = None; deadline = time.time() + 20
        while time.time() < deadline:
            try:
                data, _ = sip.recvfrom(8192)
            except socket.timeout:
                continue
            line = data.split(b"\r\n", 1)[0].decode(errors="replace")
            if " 200 " in line:
                txt = data.decode(errors="replace")
                for h in txt.split("\r\n"):
                    if h.lower().startswith("to:") and "tag=" in h:
                        to_tag = h.split("tag=", 1)[1].strip()
                cip = None; cport = None
                for h in txt.split("\r\n"):
                    if h.startswith("c=IN IP4"):
                        cip = h.split()[-1]
                    if h.startswith("m=audio"):
                        cport = int(h.split()[1])
                if cip and cport:
                    remote_rtp = (cip, cport)
                break
            if " 486 " in line or " 503 " in line or " 603 " in line or " 480 " in line:
                self.result = "REJECT:" + line; return
        if not to_tag or not remote_rtp:
            self.result = "NO-200"; return

        # 4. ACK
        ack = ("ACK sip:%s@%s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%d;branch=z9hG4bK%d\r\n"
               "Max-Forwards: 70\r\nFrom: <sip:%d@%s>;tag=%s\r\nTo: <sip:%s@%s>;tag=%s\r\n"
               "Call-ID: %s\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n"
               % (self.dest, self.board_ip, self.local_ip, sip_port, random.randint(1, 1 << 30),
                  self.ext, self.board_ip, tag, self.dest, self.board_ip, to_tag, callid)).encode()
        sip.sendto(ack, (self.board_ip, 5060))

        # 5. Pump silence RTP + count MoH RTP for hold_sec
        ssrc = random.randint(1, 1 << 30); seq = random.randint(1, 1000); ts = 0
        end = time.time() + self.hold_sec; nexttx = time.time()
        while time.time() < end:
            if time.time() >= nexttx:
                try: rtp.sendto(rtp_silence(seq, ts, ssrc), remote_rtp)
                except OSError: pass
                seq = (seq + 1) & 0xFFFF; ts = (ts + 160) & 0xFFFFFFFF; nexttx += 0.02
            try:
                d, _ = rtp.recvfrom(2048)
                if len(d) >= 12: self.rtp_rx += 1
            except socket.timeout:
                pass
        self.result = "OK rtp_rx=%d" % self.rtp_rx

        # 6. BYE
        bye = ("BYE sip:%s@%s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%d;branch=z9hG4bK%d\r\n"
               "Max-Forwards: 70\r\nFrom: <sip:%d@%s>;tag=%s\r\nTo: <sip:%s@%s>;tag=%s\r\n"
               "Call-ID: %s\r\nCSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n"
               % (self.dest, self.board_ip, self.local_ip, sip_port, random.randint(1, 1 << 30),
                  self.ext, self.board_ip, tag, self.dest, self.board_ip, to_tag, callid)).encode()
        sip.sendto(bye, (self.board_ip, 5060))
        time.sleep(0.3); sip.close(); rtp.close()

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    board = sys.argv[1]; n = int(sys.argv[2])
    dest = sys.argv[3] if len(sys.argv) > 3 else "9807"
    hold = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    base = int(sys.argv[5]) if len(sys.argv) > 5 else 191
    lip = local_ip_for(board)
    print("[uac] %s -> %s, %d concurrent calls to %s, hold %ds, ext %d.." % (lip, board, n, dest, hold, base))
    calls = [Call(board, base + i, dest, hold, lip) for i in range(n)]
    threads = [threading.Thread(target=c.run) for c in calls]
    for t in threads: t.start(); time.sleep(0.25)   # 250ms stagger so setups don't thundering-herd
    for t in threads: t.join()
    ok = sum(1 for c in calls if c.result.startswith("OK"))
    print("\n[uac] ==== RESULTS ====")
    for c in calls: print("  ext %d: %s" % (c.ext, c.result))
    print("[uac] %d/%d calls reached media (rtp flowing both ways)" % (ok, n))
    sys.exit(0 if ok == n else 1)

if __name__ == "__main__":
    main()
