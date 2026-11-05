#!/usr/bin/env python3
"""Live scale readout for every pump on USB, 5 readings/s. Ctrl-C to stop.

`weight` is (raw - offset) / factor. On an uncalibrated pump (factor 1) it is
the raw HX711 count, so pressing on a working load cell moves it by thousands.
"""
import glob
import sys
import time

import serial


def weight(s):
    s.reset_input_buffer()
    s.write(b"weight\n")
    end = time.monotonic() + 0.5
    buf = b""
    while time.monotonic() < end and b"R," not in buf:
        buf += s.read(64)
    for line in buf.decode(errors="replace").splitlines():
        if line.startswith("P,WEIGHT,"):
            return float(line.split(",")[2])
    return None


ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sys.exit("No pumps on USB")
links = []
for p in ports:
    s = serial.Serial(p, 115200, timeout=0.05)
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(b"gid\n")
    time.sleep(0.2)
    ident = s.read(64).decode(errors="replace").split("\n")[0].replace("P,ID,", "ID ")
    links.append((ident, s))
print("   ".join(f"{name:>14}" for name, _ in links))
try:
    while True:
        vals = [weight(s) for _, s in links]
        print("   ".join(f"{v:14.1f}" if v is not None else f"{'--':>14}" for v in vals))
        time.sleep(0.2)
except KeyboardInterrupt:
    pass
