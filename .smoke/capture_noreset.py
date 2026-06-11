#!/usr/bin/env python3
"""Capture ESP32-S3 serial WITHOUT pulsing a reset.

Unlike capture.py, this never touches DTR/RTS, so the running firmware (and any
live WebSocket/SIP state) is preserved. Use it to watch events on an
already-booted board — e.g. a live call — rather than a fresh boot.

Usage: python capture_noreset.py [COM5] [seconds] [outfile]
"""
import sys, time

try:
    import serial
except ImportError:
    print("ERROR: pyserial not available", file=sys.stderr)
    sys.exit(2)

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 120
out  = sys.argv[3] if len(sys.argv) > 3 else "call_log.txt"

try:
    # dsrdtr/rtscts left default-off; do NOT assign ser.dtr/ser.rts so the
    # adapter doesn't assert a reset on open.
    ser = serial.Serial(port, 115200, timeout=0.2)
except Exception as e:
    print("ERROR opening %s: %s" % (port, e), file=sys.stderr)
    sys.exit(3)

deadline = time.time() + secs
total = 0
with open(out, "wb") as f:
    while time.time() < deadline:
        try:
            data = ser.read(4096)
        except Exception as e:
            sys.stdout.write("\n[capture] read error: %s\n" % e)
            break
        if data:
            total += len(data)
            f.write(data); f.flush()
            try:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
            except Exception:
                pass
try:
    ser.close()
except Exception:
    pass
sys.stdout.write("\n[capture] done: %d bytes in %ds -> %s\n" % (total, secs, out))
