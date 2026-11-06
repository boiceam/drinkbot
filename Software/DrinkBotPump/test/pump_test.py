#!/usr/bin/env python3
"""Interactive hardware test for DrinkBotPump firmware 0.6.0 (see TEST_PLAN.md).

Two pumps on USB (A = reference / bridge, B = unit under test), wired to each
other over RS485. Automatic checks run on their own; the script asks you to
look at or do something whenever a check needs a human.

Power rule: the pump supply is sized for ONE running pump. Before any motion
command the script checks every other pump is IDLE and refuses otherwise.

Needs pyserial (pip install pyserial).
"""

import argparse
import datetime
import glob
import os
import re
import statistics
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: python3 -m pip install pyserial")

EXPECTED_VERSION = "0.6.0"
BRIDGE_TIMEOUT_S = 3.0
MAX_MOTION_MS = 120000

GREEN, RED, YELLOW, CYAN, BOLD, DIM, RESET = (
    "\033[32m", "\033[31m", "\033[33m", "\033[36m", "\033[1m", "\033[2m", "\033[0m")


class Abort(Exception):
    pass


class PowerRule(Exception):
    """A motion command while another pump is running (never allowed)."""


MOTION = ("dispense", "prime", "purge")


# ---------------------------------------------------------------- serial link

class Pump:
    """One pump on a USB serial port."""

    all = []

    def __init__(self, port, label):
        Pump.all.append(self)
        self.port = port
        self.label = label
        self.id = None
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def reopen(self, port):
        self.close()
        self.port = port
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def write_raw(self, data: bytes):
        self.ser.write(data)
        self.ser.flush()

    def read_line(self, deadline):
        buf = b""
        while time.monotonic() < deadline:
            c = self.ser.read(1)
            if not c:
                continue
            if c == b"\n":
                return buf.decode(errors="replace").rstrip("\r")
            buf += c
        return None if not buf else buf.decode(errors="replace")

    def drain(self, wait=0.0):
        """Return whatever arrives within `wait` seconds (unsolicited output)."""
        end = time.monotonic() + wait
        data = b""
        while True:
            chunk = self.ser.read(256)
            data += chunk
            if not chunk and time.monotonic() >= end:
                break
        return data.decode(errors="replace")

    def motion_target(self, text):
        """The pump a motion command would start, 'any' if unknown, else None."""
        t = text.split()
        if t and t[0] in MOTION:
            return self
        if len(t) >= 3 and t[0] == "bridge" and t[2] in MOTION:
            tid = int(t[1]) if t[1].isdigit() else None
            return next((p for p in Pump.all if p.id == tid), "any")
        return None

    def raw_status(self):
        self.ser.reset_input_buffer()
        self.write_raw(b"status\n")
        deadline = time.monotonic() + 1.0
        while (line := self.read_line(deadline)) is not None:
            m = re.fullmatch(r"P,STATUS,(\w+),\d+", line)
            if m:
                self.read_line(deadline)  # the R,OK
                return m.group(1)
        return None

    def cmd(self, text, timeout=3.0):
        """Send a command, collect lines up to the final R,... or $[R,...]."""
        target = self.motion_target(text)
        if target is not None:
            for p in Pump.all:
                if p is not target and p.ser.is_open:
                    st = p.raw_status()
                    if st != "IDLE":
                        raise PowerRule(f"refused `{text}` on {self.label}: pump {p.label} is {st}")
        self.ser.reset_input_buffer()
        t0 = time.monotonic()
        self.write_raw((text + "\n").encode())
        lines = []
        deadline = t0 + timeout
        while True:
            line = self.read_line(deadline)
            if line is None:
                break
            if line == "":
                continue
            lines.append(line)
            if line.startswith("R,") or line.startswith("$[R,"):
                break
        return lines, time.monotonic() - t0


def final(lines):
    return lines[-1] if lines else "(no reply)"


def packet_payload(line):
    """'$[P,VER,0.4.0|ABCD1234]' -> ('P,VER,0.4.0', crc_ok)."""
    m = re.fullmatch(r"\$\[(.*)\|([0-9A-Fa-f]{1,8})\]", line)
    if not m:
        return None, False
    payload, crc = m.group(1), int(m.group(2), 16)
    calc = zlib.crc32(("$[" + payload + "|").encode()) & 0xFFFFFFFF
    return payload, calc == crc


def find_ports():
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))


# ---------------------------------------------------------------- reporting

class Report:
    def __init__(self):
        self.rows = []  # (test, check, status, detail)
        self.test = ""
        self.started = datetime.datetime.now()

    def record(self, check, status, detail=""):
        self.rows.append((self.test, check, status, detail))
        color = {"PASS": GREEN, "FAIL": RED, "SKIP": YELLOW}[status]
        print(f"   {color}{status}{RESET}  {check}" + (f"  {DIM}{detail}{RESET}" if detail else ""))

    def check(self, cond, check, detail=""):
        self.record(check, "PASS" if cond else "FAIL", detail)
        return cond

    def counts(self):
        c = {"PASS": 0, "FAIL": 0, "SKIP": 0}
        for r in self.rows:
            c[r[2]] += 1
        return c

    def write(self, path, header):
        with open(path, "w") as f:
            f.write(f"# DrinkBotPump test report\n\n{header}\n\n")
            c = self.counts()
            f.write(f"**Result:** {c['PASS']} pass, {c['FAIL']} fail, {c['SKIP']} skip\n\n")
            f.write("| Test | Check | Result | Detail |\n|---|---|---|---|\n")
            for t, chk, st, d in self.rows:
                d = d.replace("|", "\\|").replace("\n", " ")
                f.write(f"| {t} | {chk} | {st} | {d} |\n")


R = Report()


def heading(code, title):
    R.test = code
    print(f"\n{BOLD}{CYAN}== {code}: {title} =={RESET}")


def say(text):
    print(f"   {text}")


def ask(question):
    """Human check. y = pass, n = fail (asks what happened), s = skip, q = quit."""
    while True:
        ans = input(f"   {BOLD}? {question} [y/n/s/q] {RESET}").strip().lower()
        if ans in ("y", "yes"):
            R.record(question, "PASS")
            return True
        if ans in ("n", "no"):
            note = input("     What did you see? ").strip()
            R.record(question, "FAIL", note)
            return False
        if ans in ("s", "skip"):
            R.record(question, "SKIP")
            return None
        if ans in ("q", "quit"):
            raise Abort()


def do(instruction):
    """Ask the user to do something; Enter continues, s skips."""
    ans = input(f"   {BOLD}> {instruction}  [Enter when done, s to skip, q to quit] {RESET}").strip().lower()
    if ans == "q":
        raise Abort()
    return ans != "s"


def ask_number(prompt, default=None):
    while True:
        raw = input(f"   {BOLD}? {prompt}{f' [{default}]' if default is not None else ''}: {RESET}").strip()
        if raw == "" and default is not None:
            return default
        if raw.lower() in ("s", "skip"):
            return None
        if raw.lower() == "q":
            raise Abort()
        try:
            return float(raw)
        except ValueError:
            print("     Enter a number, s to skip or q to quit.")


# ---------------------------------------------------------------- helpers

def expect(pump, text, want, check=None, timeout=3.0):
    lines, dt = pump.cmd(text, timeout)
    got = final(lines)
    ok = got == want
    R.check(ok, check or f"{pump.label}: `{text}` → {want}", f"got {' / '.join(lines) or '(no reply)'}")
    return lines


def status(pump):
    lines, _ = pump.cmd("status", 1.0)
    for line in lines:
        m = re.fullmatch(r"P,STATUS,(IDLE|FWD|REV),(\d+)", line)
        if m:
            return m.group(1), int(m.group(2))
    return None, None


def leds_off(*pumps):
    for p in pumps:
        p.cmd("ledmode 1 0")
        p.cmd("ledtime 0")


def show_color(pump, r, g, b, brightness=60):
    pump.cmd("ledtime 0")
    pump.cmd(f"ledcolor {r} {g} {b}")
    pump.cmd(f"ledmode 1 {brightness}")


def bridge(a, target, text, timeout=BRIDGE_TIMEOUT_S + 1.5):
    """Bridge a command; returns (payloads, all_crc_ok, final_line, seconds)."""
    lines, dt = a.cmd(f"bridge {target} {text}", timeout)
    payloads, crc_ok = [], True
    for line in lines:
        if line.startswith("$["):
            p, ok = packet_payload(line)
            crc_ok &= ok
            payloads.append(p)
    return payloads, crc_ok, final(lines), dt, lines


def wait_idle(pump, limit_s):
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit_s:
        st, _ = status(pump)
        if st == "IDLE":
            return time.monotonic() - t0
        time.sleep(0.03)
    return None


def stop_all(*pumps):
    for p in pumps:
        try:
            p.cmd("stop", 1.0)
        except Exception:
            pass


# ---------------------------------------------------------------- tests

def t0_identify(a, b):
    heading("T0", "Identify pumps and firmware version")
    for p in (a, b):
        lines, _ = p.cmd("version")
        ver = next((l.split(",", 2)[2] for l in lines if l.startswith("P,VER,")), None)
        if not R.check(ver == EXPECTED_VERSION and final(lines) == "R,OK",
                       f"{p.label} ({p.port}) runs {EXPECTED_VERSION}", f"got {' / '.join(lines)}"):
            say(f"{RED}{p.label} must be flashed with {EXPECTED_VERSION} (Debug Port = Disabled) before testing.{RESET}")
            raise Abort()
    show_color(a, 0, 255, 0)
    show_color(b, 0, 0, 255)
    say("Pump A's LEDs are now GREEN, pump B's are BLUE.")
    if not ask("Do you see one pump green and the other blue?"):
        raise Abort()
    do("Label them: green = A (reference), blue = B (unit under test)")
    leds_off(a, b)


def t1_clean_usb(a, b):
    heading("T1", "No unsolicited USB output (Debug Port = Disabled)")
    for p in (a, b):
        stray = p.drain(2.5)
        R.check(stray == "", f"{p.label}: silent for 2.5 s while idle", repr(stray[:80]) if stray else "")


def t2_ids(a, b, quick=False):
    heading("T2", "Device ID")
    for p in (a, b):
        lines, _ = p.cmd("gid")
        m = next((re.fullmatch(r"P,ID,(\d+)", l) for l in lines if l.startswith("P,ID,")), None)
        p.id = int(m.group(1)) if m else None
        say(f"{p.label} currently has ID {p.id}")
    if not quick:
        for bad in ("0", "255", "300", "abc"):
            expect(b, f"sid {bad}", "R,ERR")
        expect(b, "sid", "R,INVLD", "B: `sid` without a value → R,INVLD")
    for p, default in ((a, a.id if a.id and 1 <= a.id <= 254 else 1),
                       (b, b.id if b.id and 1 <= b.id <= 254 and b.id != a.id else None)):
        if p is b and default is None:
            default = (a.id or 1) + 1
        while True:
            want = ask_number(f"ID for pump {p.label} (1-254)", default)
            if want is None:
                want = default
            want = int(want)
            if 1 <= want <= 254 and (p is a or want != a.id):
                break
            print("     Must be 1-254 and different from A.")
        expect(p, f"sid {want}", "R,OK")
        expect(p, "scfg", "R,OK")
        lines, _ = p.cmd("gid")
        R.check(f"P,ID,{want}" in lines, f"{p.label}: gid → P,ID,{want}", " / ".join(lines))
        p.id = want
    say(f"A = ID {a.id}, B = ID {b.id}. Write these on the boards.")


def t3_parser(a, b):
    heading("T3", "Command parser")
    expect(b, "frobnicate", "R,INVLD")
    b.write_raw(b"\n")
    stray = b.drain(0.5)
    R.check(stray == "", "B: blank line → no reply", repr(stray))
    b.ser.reset_input_buffer()
    b.write_raw(b"gid\r\n")
    lines = []
    deadline = time.monotonic() + 1.0
    while (l := b.read_line(deadline)) is not None:
        lines.append(l)
        if l.startswith("R,"):
            break
    R.check(lines[-1:] == ["R,OK"] and f"P,ID,{b.id}" in lines, "B: CRLF line ending accepted", " / ".join(lines))
    expect(b, "dispense", "R,INVLD", "B: `dispense` without a value → R,INVLD")
    b.write_raw(b"x" * 400 + b"\n")
    b.drain(0.5)
    expect(b, "version", "R,OK", "B: still answers after a 400-char line")


def t4_limits(a, b):
    heading("T4", "Motion limits")
    for c in ("dispense 0", "prime 0", "purge 0", f"dispense {MAX_MOTION_MS + 1}",
              f"prime {MAX_MOTION_MS + 1}", f"purge {MAX_MOTION_MS + 1}", "dispense abc", "dispense -5"):
        expect(b, c, "R,ERR")
    st, _ = status(b)
    R.check(st == "IDLE", "B: still IDLE after rejected commands", str(st))


def t5_forward(a, b):
    heading("T5", "Forward run and timing")
    do("Put B's intake in water and the outlet over a cup if you want to watch flow (optional)")
    lines, _ = b.cmd("dispense 3000")
    t_ok = time.monotonic()
    R.check(final(lines) == "R,OK", "B: `dispense 3000` → R,OK", " / ".join(lines))
    st, rem = status(b)
    R.check(st == "FWD" and rem is not None and 2700 <= rem <= 3000, "B: status FWD with ≤3000 ms left", f"{st} {rem}")
    dt = wait_idle(b, 5)
    run_ms = (time.monotonic() - t_ok) * 1000 if dt is not None else None
    R.check(run_ms is not None and abs(run_ms - 3000) <= 100, "B: run ended after 3000 ±100 ms",
            f"{run_ms:.0f} ms" if run_ms else "never went IDLE")
    st, rem = status(b)
    R.check(st == "IDLE" and rem == 0, "B: status IDLE,0 afterwards", f"{st} {rem}")
    ask("Did B's motor run for about 3 s, pumping FORWARD (out of the outlet)?")


def t6_reverse(a, b):
    heading("T6", "Reverse, busy rejection, stop")
    expect(b, "purge 3000", "R,OK")
    st, _ = status(b)
    R.check(st == "REV", "B: status REV during purge", str(st))
    expect(b, "prime 1000", "R,ERR", "B: second motion while running → R,ERR")
    expect(b, "dispense 1000", "R,ERR", "B: dispense while running → R,ERR")
    wait_idle(b, 5)
    ask("Did B's motor run for about 3 s in REVERSE (back toward the intake)?")
    expect(b, "prime 10000", "R,OK")
    time.sleep(1.0)
    lines, _ = b.cmd("stop")
    R.check(final(lines) == "R,OK", "B: `stop` → R,OK", " / ".join(lines))
    st, rem = status(b)
    R.check(st == "IDLE" and rem == 0, "B: IDLE immediately after stop", f"{st} {rem}")
    expect(b, "stop", "R,OK", "B: `stop` while idle → R,OK")
    ask("Did the 10 s prime stop after about 1 s?")


def t7_max(a, b):
    heading("T7", "Maximum run time accepted")
    expect(b, f"prime {MAX_MOTION_MS}", "R,OK")
    st, rem = status(b)
    R.check(st == "FWD" and rem and rem > MAX_MOTION_MS - 500, "B: running with ~120000 ms left", f"{st} {rem}")
    expect(b, "stop", "R,OK")


def t8_leds(a, b):
    heading("T8", "LED commands (watch pump B)")
    b.cmd("ledtime 0")
    b.cmd("ledcolor 255 0 255")
    b.cmd("ledmode 1 80")
    ask("B: all 20 LEDs solid MAGENTA?")
    b.cmd("ledcolor 0 255 255")
    b.cmd("ledfade 500 300 500 300")
    b.cmd("ledmode 2 120")
    ask("B: CYAN, fading in and out about once every 1.6 s?")
    b.cmd("ledcolor 255 128 0")
    b.cmd("ledchase 60 4")
    b.cmd("ledmode 3 120")
    ask("B: ORANGE dot with a tail LOOPING around the strip in one direction?")
    b.cmd("ledmode 4 120")
    ask("B: the orange dot now BOUNCING back and forth?")
    b.cmd("ledchase 20 2")
    ask("B: bouncing faster with a shorter tail?")
    b.cmd("ledtime 1500")
    say("Timer set to 1.5 s; the animation should finish its sweep and go dark.")
    time.sleep(4)
    ask("B: LEDs went dark within a few seconds (after finishing a sweep)?")
    show_color(b, 255, 255, 255, 60)
    ask("B: all LEDs solid WHITE?")
    b.cmd("ledmode 1 0")
    ask("B: LEDs off after `ledmode 1 0`?")


def t9_dispense_led(a, b):
    heading("T9", "Dispense with LED mode")
    b.cmd("ledtime 0")
    b.cmd("ledcolor 0 0 255")
    b.cmd("ledchase 75 4")
    b.cmd("ledmode 1 0")
    expect(b, "dispense 4000 3", "R,OK")
    say("Running 4 s with a blue chase…")
    wait_idle(b, 6)
    time.sleep(2.0)
    ask("B: blue chase while the motor ran, then LEDs dark after it stopped?")


def t10_scale(a, b):
    heading("T10", "Scale")
    lines, _ = b.cmd("weight")
    m = next((re.fullmatch(r"P,WEIGHT,(-?[\d.]+)", l) for l in lines if l.startswith("P,WEIGHT,")), None)
    if not R.check(m is not None and final(lines) == "R,OK", "B: `weight` returns a number", " / ".join(lines)):
        return
    if not do("Put B's EMPTY liquid container on its scale (nothing else). This overwrites B's stored tare and calibration"):
        R.record("Scale tare / calibration", "SKIP")
        return
    lines, dt = b.cmd("tare", 6)
    R.check(final(lines) == "R,OK", "B: `tare` → R,OK", " / ".join(lines))
    R.record("B: tare duration", "PASS", f"{dt * 1000:.0f} ms (bridge timeout is {BRIDGE_TIMEOUT_S * 1000:.0f} ms; see T13)")
    time.sleep(1.0)
    # Not a pass/fail yet: before calibration the reading may be in raw counts.
    # "Back to ≈0 g" below checks the tare once the scale is calibrated.
    w0 = read_weight(b)
    R.record("B: reading after tare, before calibration", "PASS", f"{w0}")
    known = ask_number("Weight of your known object in grams (s to skip calibration)")
    if known:
        do(f"Place the {known:g} g object on B's scale (keep the container there) and let it settle")
        time.sleep(1.0)
        before = read_weight(b)
        R.record("B: reading with the object, before calibration", "PASS",
                 f"{before}; on a never-calibrated pump, about {abs(before - (w0 or 0)) / known:.1f} counts/g")
        lines, _ = b.cmd(f"cal {known:g}", 6)
        R.check(final(lines) == "R,OK", f"B: `cal {known:g}` → R,OK", " / ".join(lines))
        time.sleep(1.0)
        w = read_weight(b)
        R.check(w is not None and abs(w - known) <= known * 0.01, f"B: reads {known:g} g ±1%", f"{w}")
        samples = []
        for _ in range(10):
            samples.append(read_weight(b))
            time.sleep(0.25)
        samples = [s for s in samples if s is not None]
        sd = statistics.pstdev(samples) if len(samples) > 1 else None
        R.check(sd is not None and sd < 1.0, "B: noise over 10 readings < 1 g (std dev)", f"{sd:.3f} g" if sd is not None else "")
        do("Remove the object (leave the container)")
        time.sleep(1.5)
        w = read_weight(b)
        R.check(w is not None and abs(w) < max(2.0, known * 0.01), "B: back to ≈0 g", f"{w}")
        b.known_weight = known


def read_weight(p):
    lines, _ = p.cmd("weight")
    for l in lines:
        if l.startswith("P,WEIGHT,"):
            try:
                return float(l.split(",")[2])
            except ValueError:
                return None
    return None


def bridge_basics(src, dst, prefix, motion=True):
    payloads, crc, fin, dt, lines = bridge(src, dst.id, "version")
    R.check(payloads == [f"P,VER,{EXPECTED_VERSION}", "R,OK"] and crc,
            f"{prefix}: bridged version → two packets, valid CRCs", f"{' / '.join(lines)} ({dt * 1000:.0f} ms)")
    payloads, crc, fin, dt, lines = bridge(src, dst.id, "gid")
    R.check(payloads == [f"P,ID,{dst.id}", "R,OK"] and crc, f"{prefix}: bridged gid → P,ID,{dst.id}", " / ".join(lines))
    payloads, crc, fin, dt, lines = bridge(src, dst.id, "status")
    R.check(bool(payloads) and payloads[0].startswith("P,STATUS,IDLE") and crc, f"{prefix}: bridged status", " / ".join(lines))
    if not motion:
        return
    payloads, crc, fin, dt, lines = bridge(src, dst.id, "dispense 2000")
    R.check(payloads == ["R,OK"] and crc, f"{prefix}: bridged `dispense 2000` → $[R,OK]", " / ".join(lines))
    st, rem = status(dst)
    R.check(st == "FWD", f"{prefix}: {dst.label}'s own USB reports FWD", f"{st} {rem}")
    payloads, crc, fin, dt, lines = bridge(src, dst.id, "status")
    R.check(bool(payloads) and payloads[0].startswith("P,STATUS,FWD"), f"{prefix}: bridged status shows FWD", " / ".join(lines))
    wait_idle(dst, 4)


def t11_bridge(a, b):
    heading("T11", f"Bridge A → B (ID {b.id})")
    b.drain(0.1)
    bridge_basics(a, b, "A→B")
    payloads, crc, fin, dt, lines = bridge(a, b.id, "weight")
    R.check(bool(payloads) and payloads[0].startswith("P,WEIGHT,") and crc, "A→B: bridged weight", " / ".join(lines))
    payloads, _, _, _, lines = bridge(a, b.id, "frobnicate")
    R.check(payloads == ["R,INVLD"], "A→B: unknown bridged command → $[R,INVLD]", " / ".join(lines))
    payloads, _, _, _, lines = bridge(a, b.id, "dispense 0")
    R.check(payloads == ["R,ERR"], "A→B: bridged `dispense 0` → $[R,ERR]", " / ".join(lines))
    payloads, _, _, _, lines = bridge(a, b.id, "dispense 3000 4")
    R.check(payloads == ["R,OK"], "A→B: bridged `dispense 3000 4` → $[R,OK]", " / ".join(lines))
    wait_idle(b, 5)
    ask("B ran ~2 s, then ran ~3 s again with a bouncing LED animation?")
    stray = b.drain(0.2)
    R.check(stray == "", "B: its USB stayed silent during bridged traffic", repr(stray[:80]))


def t12_bridge_errors(a, b, quick=False):
    heading("T12", "Bridge error handling")
    absent = next(i for i in range(200, 255) if i not in (a.id, b.id))
    payloads, crc, fin, dt, lines = bridge(a, absent, "version", timeout=BRIDGE_TIMEOUT_S + 2)
    R.check(fin == "R,TIMEOUT" and abs(dt - BRIDGE_TIMEOUT_S) <= 0.3,
            f"A: bridge to absent ID {absent} → R,TIMEOUT after ~{BRIDGE_TIMEOUT_S:g} s", f"{fin} after {dt * 1000:.0f} ms")
    if quick:
        return
    for target, why in ((a.id, "own ID"), (0, "ID 0"), (256, "ID 256")):
        lines, _ = a.cmd(f"bridge {target} version")
        R.check(final(lines) == "R,ERR", f"A: bridge to {why} → R,ERR", " / ".join(lines))
    lines, _ = a.cmd("bridge " + str(b.id) + " " + "x" * 210)
    R.check(final(lines) == "R,ERR", "A: bridged command over 200 chars → R,ERR", " / ".join(lines))
    # Two requests back to back: the second is rejected while the first is pending
    a.ser.reset_input_buffer()
    a.write_raw(f"bridge {b.id} weight\nbridge {b.id} version\n".encode())
    got, deadline = [], time.monotonic() + 3
    while time.monotonic() < deadline and not any(g.startswith("$[R,") for g in got):
        l = a.read_line(deadline)
        if l:
            got.append(l)
    R.check("R,ERR" in got and any(g.startswith("$[R,OK") for g in got) and not any("P,VER" in g for g in got),
            "A: second bridge while one is pending → R,ERR, first completes", " / ".join(got))
    payloads, _, _, _, lines = bridge(a, b.id, f"bridge {a.id} version")
    R.check(payloads == ["R,INVLD"], "B: `bridge` received over the bus → $[R,INVLD]", " / ".join(lines))
    stray = b.drain(0.2)
    R.check(stray == "", "B: its USB stayed silent", repr(stray[:80]))


def t13_bridged_scale(a, b):
    heading("T13", "Bridged tare / cal (must beat the bridge timeout)")
    if not do("Leave B's empty container on its scale; this re-tares B over the bridge"):
        R.record("Bridged tare", "SKIP")
        return
    payloads, crc, fin, dt, lines = bridge(a, b.id, "tare", timeout=BRIDGE_TIMEOUT_S + 3)
    late = a.drain(2.0)
    R.check(payloads == ["R,OK"], f"A→B: bridged tare → $[R,OK] within the {BRIDGE_TIMEOUT_S:g} s bridge timeout",
            f"{' / '.join(lines)} after {dt * 1000:.0f} ms" + (f"; late: {late.strip()}" if late.strip() else ""))
    known = getattr(b, "known_weight", None)
    if known:
        do(f"Place the {known:g} g object on B's scale again")
        payloads, crc, fin, dt, lines = bridge(a, b.id, f"cal {known:g}", timeout=BRIDGE_TIMEOUT_S + 3)
        late = a.drain(2.0)
        R.check(payloads == ["R,OK"], f"A→B: bridged `cal {known:g}` → $[R,OK] within {BRIDGE_TIMEOUT_S:g} s",
                f"{' / '.join(lines)} after {dt * 1000:.0f} ms" + (f"; late: {late.strip()}" if late.strip() else ""))
        time.sleep(1.0)
        w = read_weight(b)
        R.check(w is not None and abs(w - known) <= known * 0.01, f"B: still reads {known:g} g ±1% after bridged cal", f"{w}")
        do("Remove the object")


def t14_broadcast(a, b):
    heading("T14", "Broadcast (ID 255)")
    a.cmd("ledtime 0")
    b.cmd("ledtime 0")
    lines, _ = a.cmd("bridge 255 ledcolor 255 255 0")
    R.check(lines == ["R,OK"], "A: broadcast ledcolor → only the local R,OK", " / ".join(lines))
    lines, _ = a.cmd("bridge 255 ledmode 1 60")
    R.check(lines == ["R,OK"], "A: broadcast ledmode → only the local R,OK", " / ".join(lines))
    ask("BOTH pumps solid YELLOW?")
    a.cmd("bridge 255 ledmode 1 0")
    # One pump runs at a time (power rule): stop B over the bus, then A locally
    for runner, how in ((b, "over the bus"), (a, "locally on the bridge")):
        expect(runner, "prime 10000", "R,OK")
        time.sleep(0.5)
        lines, _ = a.cmd("bridge 255 stop")
        R.check(lines == ["R,OK"], "A: broadcast stop → R,OK", " / ".join(lines))
        time.sleep(0.15)
        st, _ = status(runner)
        R.check(st == "IDLE", f"Broadcast stop halted {runner.label} ({how})", str(st))
    for c in ("prime 1000", "version"):
        lines, _ = a.cmd(f"bridge 255 {c}")
        R.check(final(lines) == "R,ERR", f"A: broadcast `{c}` → R,ERR", " / ".join(lines))
    time.sleep(0.2)
    sb, _ = status(b)
    R.check(sb == "IDLE", "B didn't move on a rejected broadcast", str(sb))
    stray = b.drain(0.2)
    R.check(stray == "", "B: no reply leaked to its USB", repr(stray[:80]))


def t15_reverse_bridge(a, b):
    heading("T15", f"Bridge B → A (ID {a.id})")
    bridge_basics(b, a, "B→A")


def t16_soak(a, b, n):
    heading("T16", f"Bus soak ({n} round trips each)")
    def bridged():
        payloads, crc, fin, _, lines = bridge(a, b.id, "status")
        return payloads[-1:] == ["R,OK"] and crc, lines

    def direct():
        lines, _ = b.cmd("status", 1.0)
        return final(lines) == "R,OK", lines

    def soak_runs():
        yield "bridged status A→B, motors off", bridged, None
        yield "direct status on B", direct, None
        yield "bridged status A→B, B's motor running", bridged, b
        yield "bridged status A→B, A's motor running", bridged, a

    for label, fn, runner in soak_runs():
        if runner:
            expect(runner, f"prime {MAX_MOTION_MS}", "R,OK", f"{runner.label}: motor on for the soak")
        times, fails = [], []
        for i in range(n):
            t0 = time.monotonic()
            ok, lines = fn()
            dt = time.monotonic() - t0
            if ok:
                times.append(dt * 1000)
            else:
                fails.append(f"#{i}: {' / '.join(lines) or '(no reply)'}")
            if i % 50 == 49:
                print(f"     {label}: {i + 1}/{n}", end="\r")
        print(" " * 40, end="\r")
        detail = (f"avg {statistics.mean(times):.1f} ms, max {max(times):.1f} ms" if times else "") + \
                 (f"; {len(fails)} failures, first {fails[0]}" if fails else "")
        if runner:
            runner.cmd("stop")
            time.sleep(0.3)
        R.check(not fails, f"{label}: 0 failures in {n}", detail)


def wait_for_ports(count, timeout=60):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        ports = find_ports()
        if len(ports) >= count:
            time.sleep(1.0)
            return find_ports()
        time.sleep(0.3)
    return find_ports()


def t17_persistence(a, b):
    heading("T17", "Power cycle and persistence")
    b.close()
    if not do("Unplug B's USB AND its motor supply, wait 3 s"):
        b.reopen(b.port)
        R.record("Power cycle", "SKIP")
        return
    t0 = time.monotonic()
    while b.port in find_ports() and time.monotonic() - t0 < 30:
        time.sleep(0.2)
    if b.port in find_ports():
        say(f"{YELLOW}B's port {b.port} is still present; was the USB unplugged?{RESET}")
    say("Now plug B's supply and USB back in, and watch its LEDs and GO LED during boot.")
    ports = wait_for_ports(2, 90)
    new = [p for p in ports if p != a.port]
    if not R.check(len(new) == 1, "B re-enumerated on USB", ", ".join(ports)):
        raise Abort()
    time.sleep(1.0)
    b.reopen(new[0])
    ask("B's LEDs faded RED in and out for about 5 s, then went dark?")
    ask("B's GO LED toggles on/off every 2 s?")
    lines, _ = b.cmd("gid")
    R.check(f"P,ID,{b.id}" in lines, f"B kept ID {b.id}", " / ".join(lines))
    known = getattr(b, "known_weight", None)
    if known and do(f"Place the {known:g} g object on B's scale (container still on)"):
        time.sleep(1.0)
        w = read_weight(b)
        R.check(w is not None and abs(w - known) <= max(known * 0.02, 2), f"B still reads {known:g} g (±2%) after reboot", f"{w}")
        do("Remove the object")
    payloads, crc, fin, dt, lines = bridge(a, b.id, "version")
    R.check(payloads[-1:] == ["R,OK"] and crc, "A can still bridge to B after reboot", " / ".join(lines))


# ---------------------------------------------------------------- single pump

def settled_weight(p, samples=8):
    """Average of several readings; the scale makes about 10 new ones a second."""
    values = []
    for _ in range(samples):
        v = read_weight(p)
        if v is not None:
            values.append(v)
        time.sleep(0.25)
    return statistics.mean(values) if values else None


def weigh_object(b, grams, what):
    """Weighs an object against the empty reading just before it goes on, so a
    slowly moving zero doesn't count as a weighing error."""
    zero = settled_weight(b)
    if not do(f"Place the {what} on B's scale (keep the container there) and let it settle"):
        return None, zero
    time.sleep(1.0)
    loaded = settled_weight(b)
    return (loaded - zero if loaded is not None and zero is not None else None), zero


def scale_linearity(b):
    known = getattr(b, "known_weight", None)
    if not known:
        return
    heading("S2", "Second weight and stability")
    second = ask_number(f"Weight of a DIFFERENT known object in grams, ideally not {known:g} (s to skip)")
    if second:
        delta, zero = weigh_object(b, second, f"{second:g} g object")
        R.check(delta is not None and abs(delta - second) <= max(second * 0.01, 1.0),
                f"B: {second:g} g object adds {second:g} g ±1% (or ±1 g)",
                f"added {delta:.2f} g, empty reading was {zero:.2f} g" if delta is not None else "")
        do("Remove the object (leave the container)")
        time.sleep(1.5)
    if not do(f"Place the {known:g} g object on B's scale for a 30 s stability check"):
        return
    time.sleep(1.0)
    samples = []
    t_end = time.monotonic() + 30
    while time.monotonic() < t_end:
        samples.append(read_weight(b))
        time.sleep(0.25)
    samples = [v for v in samples if v is not None]
    spread = max(samples) - min(samples)
    R.check(spread < 3.0, "B: stays within 3 g over 30 s under load",
            f"spread {spread:.2f} g, std dev {statistics.pstdev(samples):.3f} g, {len(samples)} readings")
    do("Remove the object (leave the container)")
    time.sleep(1.5)
    w = settled_weight(b)
    # 3 g is about 0.06% of the 5 kg rating, near the hysteresis typical of such cells
    R.check(w is not None and abs(w) < 3.0, "B: back within 3 g of zero after the load", f"{w:.2f} g")


def power_cycle(b):
    """Returns False if skipped. The scale must stay untouched throughout."""
    b.close()
    if not do("Unplug B's USB and motor supply at the FAR end, without touching the pump or scale. Wait 3 s"):
        b.reopen(b.port)
        return False
    t0 = time.monotonic()
    while find_ports() and time.monotonic() - t0 < 30:
        time.sleep(0.2)
    say("Now restore B's motor supply and USB, again without touching the pump.")
    t0 = time.monotonic()
    while not find_ports() and time.monotonic() - t0 < 90:
        time.sleep(0.2)
    ports = find_ports()
    if not R.check(len(ports) == 1, "B came back on USB", ", ".join(ports)):
        raise Abort()
    time.sleep(6.0)  # past the boot animation
    b.reopen(ports[0])
    return True


def scale_persistence(b):
    known = getattr(b, "known_weight", None)
    if not known:
        return
    heading("S3", "Zero and calibration across power cycles")
    say("Container on, nothing else. Leave the pump and scale completely untouched until asked.")
    for cycle in (1, 2):
        before = settled_weight(b)
        if not power_cycle(b):
            R.record(f"Power cycle {cycle}", "SKIP")
            return
        after = settled_weight(b)
        time.sleep(20)
        later = settled_weight(b)
        R.check(after is not None and abs(after - before) < 5.0, f"Power cycle {cycle}: zero moves under 5 g",
                f"{before:.2f} → {after:.2f} g, then {later:.2f} g after 20 s more")
    delta, zero = weigh_object(b, known, f"{known:g} g object (the {known:g} g one, not the other)")
    if delta is not None:
        R.check(abs(delta - known) <= max(known * 0.02, 2.0), f"B: {known:g} g object adds {known:g} g ±2% after reboots",
                f"added {delta:.2f} g, empty reading was {zero:.2f} g")
        do("Remove the object")


def main_scale_only(port):
    """Direct scale tests on the one pump on USB; no bus, no second pump."""
    b = Pump(port, "B")
    print(f"{BOLD}DrinkBotPump {EXPECTED_VERSION} direct scale test{RESET}\nPump on {b.port} (\"B\" in the prompts)")
    print("Answer prompts as asked, or q to quit and save the report.")
    aborted = False
    try:
        heading("S0", "Firmware")
        lines, _ = b.cmd("version")
        if not R.check(f"P,VER,{EXPECTED_VERSION}" in lines, f"Runs {EXPECTED_VERSION}", " / ".join(lines)):
            raise Abort()
        lines, _ = b.cmd("gid")
        m = next((re.fullmatch(r"P,ID,(\d+)", l) for l in lines if l.startswith("P,ID,")), None)
        b.id = int(m.group(1)) if m else None
        t10_scale(None, b)
        scale_linearity(b)
        scale_persistence(b)
    except (Abort, KeyboardInterrupt):
        aborted = True
        print(f"\n{YELLOW}Stopped early.{RESET}")
    except (serial.SerialException, OSError) as e:
        aborted = True
        R.record("Serial error", "FAIL", str(e))
    finally:
        b.close()
    c = R.counts()
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reports")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{R.started.strftime('%Y%m%d-%H%M%S')}_scale_id{b.id}.md")
    R.write(path, f"- Date: {R.started:%Y-%m-%d %H:%M}\n- Suite: direct scale{' (stopped early)' if aborted else ''}\n"
                  f"- Pump: ID {b.id} on {b.port}\n- Firmware: {EXPECTED_VERSION}")
    color = GREEN if c["FAIL"] == 0 else RED
    print(f"\n{BOLD}{color}{c['PASS']} pass, {c['FAIL']} fail, {c['SKIP']} skip{RESET}\nReport: {path}")
    sys.exit(1 if c["FAIL"] else 0)


# ---------------------------------------------------------------- main

FULL = ["T0", "T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "T9", "T10",
        "T11", "T12", "T13", "T14", "T15", "T16", "T17"]
QUICK = ["T0", "T1", "T2", "T4", "T5", "T6", "T9", "T10", "T11", "T12", "T16", "T17"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", help="serial port of pump A (reference / bridge)")
    ap.add_argument("--b", help="serial port of pump B (unit under test)")
    ap.add_argument("--suite", choices=("full", "quick"), default="full")
    ap.add_argument("--only", help="comma-separated tests, e.g. T5,T11 (T0 and T2 always run)")
    ap.add_argument("--scale-only", action="store_true", help="direct scale tests on the single pump on USB")
    args = ap.parse_args()

    if args.scale_only:
        ports = [args.b] if args.b else find_ports()
        if len(ports) != 1:
            sys.exit(f"--scale-only needs exactly one pump on USB, found {len(ports)}: {ports}. Use --b to choose.")
        main_scale_only(ports[0])

    ports = [args.a, args.b] if args.a and args.b else find_ports()
    if len(ports) != 2:
        sys.exit(f"Need exactly two pumps on USB, found {len(ports)}: {ports}. Use --a/--b to choose.")
    a, b = Pump(ports[0], "A"), Pump(ports[1], "B")
    quick = args.suite == "quick"
    tests = QUICK if quick else FULL
    if args.only:
        wanted = {t.strip().upper() for t in args.only.split(",")}
        tests = [t for t in FULL if t in wanted or t in ("T0", "T2")]

    print(f"{BOLD}DrinkBotPump {EXPECTED_VERSION} hardware test — {args.suite} suite{RESET}")
    print(f"A = {a.port}\nB = {b.port}")
    print("Answer y (looks right), n (wrong), s (skip) or q (quit and save the report).")

    table = {
        "T0": lambda: t0_identify(a, b), "T1": lambda: t1_clean_usb(a, b),
        "T2": lambda: t2_ids(a, b, quick), "T3": lambda: t3_parser(a, b),
        "T4": lambda: t4_limits(a, b), "T5": lambda: t5_forward(a, b),
        "T6": lambda: t6_reverse(a, b), "T7": lambda: t7_max(a, b),
        "T8": lambda: t8_leds(a, b), "T9": lambda: t9_dispense_led(a, b),
        "T10": lambda: t10_scale(a, b), "T11": lambda: t11_bridge(a, b),
        "T12": lambda: t12_bridge_errors(a, b, quick), "T13": lambda: t13_bridged_scale(a, b),
        "T14": lambda: t14_broadcast(a, b), "T15": lambda: t15_reverse_bridge(a, b),
        "T16": lambda: t16_soak(a, b, 100 if quick else 300), "T17": lambda: t17_persistence(a, b),
    }
    aborted = False
    try:
        for t in tests:
            try:
                table[t]()
            except PowerRule as e:
                R.record(f"{t} power rule", "FAIL", str(e))
            except (serial.SerialException, OSError) as e:
                R.record(f"{t} serial error", "FAIL", str(e))
                raise Abort()
            finally:
                try:
                    stop_all(a, b)
                except Exception:
                    pass
    except (Abort, KeyboardInterrupt):
        aborted = True
        print(f"\n{YELLOW}Stopped early.{RESET}")
    finally:
        try:
            leds_off(a, b)
        except Exception:
            pass
        a.close()
        b.close()

    c = R.counts()
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reports")
    os.makedirs(out_dir, exist_ok=True)
    stamp = R.started.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(out_dir, f"{stamp}_A{a.id}_B{b.id}_{args.suite}.md")
    header = (f"- Date: {R.started:%Y-%m-%d %H:%M}\n- Suite: {args.suite}{' (stopped early)' if aborted else ''}\n"
              f"- Pump A: ID {a.id} on {a.port}\n- Pump B: ID {b.id} on {b.port}\n- Firmware: {EXPECTED_VERSION}")
    R.write(path, header)
    color = GREEN if c["FAIL"] == 0 else RED
    print(f"\n{BOLD}{color}{c['PASS']} pass, {c['FAIL']} fail, {c['SKIP']} skip{RESET}")
    for t, chk, st, d in R.rows:
        if st == "FAIL":
            print(f"  {RED}FAIL{RESET} {t} {chk}  {DIM}{d}{RESET}")
    print(f"Report: {path}")
    sys.exit(1 if c["FAIL"] else 0)


if __name__ == "__main__":
    main()
