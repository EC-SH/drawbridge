#!/usr/bin/env python3
"""Capture ESP32-S3 USB-CDC serial boot log for a bounded time.

Usage: python capture.py [COM4] [seconds] [outfile]

Opens the port, pulses a best-effort reset (DTR/RTS — harmless on native USB
if it no-ops), then streams everything to stdout AND the output file until the
time budget elapses. Never blocks indefinitely.
"""
import sys, time

try:
    import serial  # pyserial (ships with the IDF python env via esptool)
except ImportError:
    print("ERROR: pyserial not available in this interpreter", file=sys.stderr)
    sys.exit(2)

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 25
out  = sys.argv[3] if len(sys.argv) > 3 else "boot_log.txt"

try:
    ser = serial.Serial(port, 115200, timeout=0.2)
except Exception as e:
    print("ERROR opening %s: %s" % (port, e), file=sys.stderr)
    sys.exit(3)

# Best-effort classic reset: EN low via RTS, IO0 high via DTR (run, not download).
try:
    ser.dtr = False
    ser.rts = True
    time.sleep(0.12)
    ser.rts = False
    time.sleep(0.05)
except Exception:
    pass  # native USB-CDC may ignore line controls — we still capture running logs

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
