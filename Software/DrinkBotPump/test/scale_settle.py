#!/usr/bin/env python3
"""Logs how the scale settles after a weight goes on and comes off.

Detects the object by itself, logs for 60 s, asks you to remove it, logs 30 s
more, then prints averages over time. Uses the one pump on USB, or --port.
"""
import argparse
import glob
import statistics
import sys
import time

import serial

DETECT_G = 10.0  # a change this big means the object went on or came off
LOADED_S = 60
UNLOADED_S = 30
WINDOWS = [(0, 1), (1, 2), (2, 3), (3, 5), (5, 10), (10, 20), (20, 40), (40, 60)]


def read_weight(s):
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


def log_for(s, seconds):
    rows = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        v = read_weight(s)
        if v is not None:
            rows.append((time.monotonic() - t0, v))
        print(f"\r  {time.monotonic() - t0:5.1f} s   {v if v is not None else 0:8.2f} g  ", end="", flush=True)
        time.sleep(0.1)
    print()
    return rows


def wait_for_change(s, reference):
    while True:
        v = read_weight(s)
        if v is not None and abs(v - reference) > DETECT_G:
            return
        time.sleep(0.05)


def table(rows, zero, windows):
    print("    window      average      from zero   std dev  readings")
    for a, b in windows:
        vals = [v for t, v in rows if a <= t < b]
        if vals:
            print(f"  {a:3d}-{b:3d} s  {statistics.mean(vals):9.2f} g  {statistics.mean(vals) - zero:9.2f} g  "
                  f"{statistics.pstdev(vals):6.2f}  {len(vals):5d}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port")
    args = ap.parse_args()
    ports = [args.port] if args.port else sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 1:
        sys.exit(f"Need exactly one pump on USB, found {ports}. Use --port.")
    s = serial.Serial(ports[0], 115200, timeout=0.02)
    time.sleep(0.4)

    print("Reading the empty scale for 5 s. Container on, nothing else, hands off…")
    empty = log_for(s, 5)
    zero = statistics.mean(v for _, v in empty)
    print(f"Empty reading: {zero:.2f} g\n")
    print(">>> Gently place the 100 g object now, and let go. Logging starts by itself.")
    wait_for_change(s, zero)
    print(f"Detected. HANDS OFF for {LOADED_S} s.")
    loaded = log_for(s, LOADED_S)
    final_load = statistics.mean(v for t, v in loaded if t >= 40)
    print("\n>>> Remove the object now, gently.")
    wait_for_change(s, final_load)
    print(f"Detected. HANDS OFF for {UNLOADED_S} s.")
    unloaded = log_for(s, UNLOADED_S)
    s.close()

    print("\nWith the object on:")
    table(loaded, zero, WINDOWS)
    print("\nAfter removing it:")
    table(unloaded, zero, [w for w in WINDOWS if w[1] <= 20] + [(20, UNLOADED_S)])
    print("\nDONE")


if __name__ == "__main__":
    main()
