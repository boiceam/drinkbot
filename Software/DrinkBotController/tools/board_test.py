#!/usr/bin/env python3
"""Acceptance tests that run on the real controller board with nothing attached.

The board must be in simulator mode (no pump on USB) and reachable over the
network. Runs through the same HTTP API and WebSocket the web app uses.

  board_test.py --host 192.168.86.250 --pump-image DrinkBotPump.ino.bin

It saves test configs on the board and restores the original config at the end.
The original is held in memory only and never printed, because it contains the
Wi-Fi password. Python standard library only. Numbers refer to SPEC_v3 §19.
"""

import argparse
import base64
import datetime
import gzip
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request

results = []
GREEN, RED, BOLD, RESET = "\033[32m", "\033[31m", "\033[1m", "\033[0m"


def check(ok, name, detail=""):
    results.append((SECTION[0], name, bool(ok), str(detail)))
    print(f"   {GREEN + 'PASS' if ok else RED + 'FAIL'}{RESET}  {name}" + (f"  ({detail})" if detail != "" else ""),
          flush=True)
    return bool(ok)


SECTION = [""]


def section(text):
    SECTION[0] = text
    print(f"\n{BOLD}== {text} =={RESET}", flush=True)


class Ws:
    """Minimal WebSocket client that keeps every message it receives."""

    def __init__(self, host):
        self.sock = socket.create_connection((host, 80), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            d = self.sock.recv(1)
            if not d:
                raise ConnectionError("closed during handshake")
            head += d
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError(head.split(b"\r\n")[0].decode())
        self.lock = threading.Lock()
        self.msgs = []
        self.seq = 0
        self.closed = False
        self.close_code = None
        self.error = None
        threading.Thread(target=self._reader, daemon=True).start()

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            d = self.sock.recv(n - len(buf))
            if not d:
                raise ConnectionError("closed")
            buf += d
        return buf

    def _reader(self):
        partial = b""
        self.sock.settimeout(None)
        try:
            while True:
                b0, b1 = self._read(2)
                op, n = b0 & 15, b1 & 127
                if n == 126:
                    n = struct.unpack(">H", self._read(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._read(8))[0]
                data = self._read(n)
                if op == 8:
                    self.close_code = struct.unpack(">H", data[:2])[0] if len(data) >= 2 else 0
                    break
                if op == 9:
                    self._frame(10, data)
                    continue
                if op in (0, 1):
                    partial += data
                    if b0 & 0x80:
                        with self.lock:
                            self.msgs.append((time.monotonic(), json.loads(partial.decode())))
                        partial = b""
        except (ConnectionError, OSError, ValueError) as e:
            self.error = f"{type(e).__name__}: {e}; last bytes {partial[-80:]!r}"
        self.closed = True

    def _frame(self, op, data):
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.sock.sendall(head + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def request(self, type_, timeout=8, **fields):
        """Sends a command. Returns (ok, ack data or error message dict)."""
        self.seq += 1
        req = f"t{self.seq}"
        self._frame(1, json.dumps(dict(type=type_, reqId=req, **fields)).encode())
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.lock:
                for _, m in self.msgs:
                    if m.get("reqId") == req and m.get("type") in ("ack", "error"):
                        return m["type"] == "ack", m
            time.sleep(0.01)
        return False, {"code": "NO_REPLY", "message": f"no reply to {type_}"}

    def latest(self, type_):
        with self.lock:
            for _, m in reversed(self.msgs):
                if m.get("type") == type_:
                    return m
        return None

    def of_type(self, type_, since=0):
        with self.lock:
            return [(t, m) for t, m in self.msgs[since:] if m.get("type") == type_]

    def mark(self):
        with self.lock:
            return len(self.msgs)

    def wait(self, pred, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.lock:
                msgs = list(self.msgs)
            for t, m in reversed(msgs):
                if pred(m):
                    return m
            time.sleep(0.02)
        return None

    def wait_since(self, mark, pred, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.lock:
                msgs = self.msgs[mark:]
            for _, m in msgs:
                if pred(m):
                    return m
            time.sleep(0.02)
        return None

    def system(self):
        s = self.latest("state")
        return s["system"] if s else None

    def wait_idle(self, timeout=30):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.system() in ("READY", "ERROR"):
                return True
            time.sleep(0.05)
        return False

    def pump(self, pid):
        p = self.latest("pumps")
        for x in (p or {}).get("pumps", []):
            if x["pumpId"] == pid:
                return x
        return None


class Board:
    def __init__(self, host):
        self.host = host
        self.base = f"http://{host}"

    def get(self, path, timeout=8):
        with urllib.request.urlopen(self.base + path, timeout=timeout) as r:
            return r.status, r.read()

    def post(self, path, body, ctype, timeout=120):
        req = urllib.request.Request(self.base + path, data=body, headers={"Content-Type": ctype}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def upload(self, path, data, name="x.bin"):
        b = "----boardtest" + os.urandom(6).hex()
        body = (f'--{b}\r\nContent-Disposition: form-data; name="firmware"; filename="{name}"\r\n'
                f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{b}--\r\n".encode()
        return self.post(path, body, "multipart/form-data; boundary=" + b)

    def info(self):
        return json.loads(self.get("/api/info")[1])

    def connect(self, timeout=40):
        end = time.monotonic() + timeout
        last = None
        while time.monotonic() < end:
            try:
                ws = Ws(self.host)
                if ws.wait(lambda m: m.get("type") == "state", 5):
                    return ws
                ws.close()
            except (OSError, ConnectionError) as e:
                last = e
            time.sleep(0.5)
        raise ConnectionError(f"can't connect: {last}")


def test_config(original):
    """The board's settings and Wi-Fi with a known set of simulated pumps."""
    cfg = json.loads(json.dumps(original))
    cfg["pumps"] = [
        {"pumpId": 1, "liquid": "Tequila", "msPerMl": 100, "densityGPerMl": 1.0, "color": [255, 160, 0], "autoPrimeSec": 2},
        {"pumpId": 2, "liquid": "Triple Sec", "msPerMl": 100, "densityGPerMl": 1.0, "color": [255, 120, 0], "autoPrimeSec": 0},
        {"pumpId": 4, "liquid": "Lime Juice", "msPerMl": 100, "densityGPerMl": 1.0, "color": [0, 255, 0], "autoPrimeSec": 1},
    ]
    cfg["recipes"] = [
        {"id": "margarita", "name": "Margarita", "ingredients": [
            {"liquid": "Tequila", "parts": 2, "order": 1},
            {"liquid": "Triple Sec", "parts": 1, "order": 2},
            {"liquid": "Lime Juice", "parts": 1, "order": 3}]},
        {"id": "tequila", "name": "Straight", "ingredients": [{"liquid": "Tequila", "parts": 1, "order": 1}]},
        {"id": "lime", "name": "Lime", "ingredients": [{"liquid": "Lime Juice", "parts": 1, "order": 1}]},
    ]
    return cfg


def fake_image(version="9.0.0", bus="50000", size=6000):
    tags = f"DBP-IMAGE:\0DBP-IMAGE:{version}\0DBP-BUS:{bus}\0".encode()
    return bytes(1000) + tags + bytes(max(0, size - 1000 - len(tags)))


def run(board, ws, original, pump_image):
    section("1 Boot and simulator")
    info = board.info()
    check(info["simulator"] is True, "Running in simulator mode with nothing on USB")
    check(ws.latest("hello") and ws.latest("hello")["simulator"] is True, "hello tells the UI to show the banner")
    status, root = board.get("/")
    check(status == 200 and len(root) > 1000, "Web UI is served", f"{len(root)} bytes")
    check(board.get("/app.js")[0] == 200 and board.get("/app.css")[0] == 200, "Script and styles are served")
    try:
        board.get("/hotspot-detect.html")
        check(False, "No captive portal: unknown URLs are not redirected")
    except urllib.error.HTTPError as e:
        check(e.code == 404, "No captive portal: unknown URLs return 404 (D-18)", e.code)

    ws.request("pumpFwDelete")  # left over from an interrupted run, if any
    section("Config: test setup and rejection (8)")
    ok, r = ws.request("saveConfig", config=test_config(original), timeout=15)
    check(ok, "Valid config saved", r.get("message", ""))
    ws.wait(lambda m: m.get("type") == "pumps" and len(m["pumps"]) == 3, 15)
    time.sleep(4)
    check(all((ws.pump(i) or {}).get("present") for i in (1, 2, 4)), "All 3 configured pumps found")
    bad = test_config(original)
    bad["pumps"][0]["pumpId"] = 300
    ok, r = ws.request("saveConfig", config=bad, timeout=15)
    paths = [d.get("path", "") for d in r.get("details", [])]
    check(not ok and r.get("code") == "CONFIG_INVALID" and any("pumps" in p for p in paths),
          "Invalid config over WebSocket rejected with a field path", paths)
    status, body = board.post("/api/config", json.dumps(bad).encode(), "application/json")
    check(status == 400, "Invalid config over HTTP rejected", status)
    status, body = board.post("/api/config", b"{not json", "application/json")
    check(status == 400, "Malformed JSON rejected", status)
    time.sleep(0.5)
    check(ws.pump(4) is not None and ws.pump(1)["liquid"] == "Tequila", "Rejected saves changed nothing")

    section("2 Volume math")
    m0 = ws.mark()
    ok, r = ws.request("dispense", recipeId="margarita", volumeMl=40)
    check(ok, "Dispense accepted", r.get("message", ""))
    done = ws.wait_since(m0, lambda m: m.get("type") == "dispenseProgress" and m["status"] in ("COMPLETE", "ABORTED"), 60)
    check(done and done["status"] == "COMPLETE", "Dispense completed", done and done["status"])
    steps = [m for _, m in ws.of_type("dispenseProgress", m0) if m["status"] == "DONE"]
    check([(s["pumpId"], s["targetMl"]) for s in steps] == [(1, 20), (2, 10), (4, 10)],
          "40 ml of 2:1:1 pours 20 / 10 / 10 ml in order", [(s["pumpId"], s["targetMl"]) for s in steps])
    check(all(abs(s.get("actualMl", 0) - s["targetMl"]) < 2 for s in steps), "Weights verify each pour",
          [s.get("actualMl") for s in steps])
    check(ws.wait_idle(), "Back to idle")

    section("3 Pre-check")
    ws.request("simFault", pumpId=1, fault="weight", weightG=30)
    time.sleep(4)
    m0 = ws.mark()
    ok, r = ws.request("dispense", recipeId="margarita", volumeMl=100)
    check(not ok and r.get("code") == "PRECHECK_FAILED" and "Tequila" in r.get("message", ""),
          "Too little liquid refused, naming it", r.get("message"))
    time.sleep(0.5)
    check(not ws.of_type("dispenseProgress", m0), "Nothing poured")
    ws.request("simFault", pumpId=1, fault="weight", weightG=700)
    time.sleep(4)

    section("4 E-Stop mid-pour")
    m0 = ws.mark()
    ok, _ = ws.request("dispense", recipeId="margarita", volumeMl=200)
    time.sleep(2)
    during = ws.pump(1)["activity"]
    t0 = time.monotonic()
    ok, _ = ws.request("estop")
    ack_ms = (time.monotonic() - t0) * 1000
    check(ok and during == "DISPENSING", "E-Stop accepted during the pour", f"acknowledged in {ack_ms:.0f} ms")
    ab = ws.wait_since(m0, lambda m: m.get("type") == "dispenseProgress" and m["status"] == "ABORTED", 2)
    check(ab is not None, "Pour reported as aborted", ab and ab.get("reason"))
    idle = ws.wait_since(m0, lambda m: m.get("type") == "state" and m["system"] in ("READY", "ERROR"), 1.0)
    check(idle is not None, "System idle again")
    t = time.monotonic()
    while time.monotonic() - t < 1.0 and any(ws.pump(i)["activity"] != "IDLE" for i in (1, 2, 4)):
        time.sleep(0.02)
    check(all(ws.pump(i)["activity"] == "IDLE" for i in (1, 2, 4)), "All pumps idle", f"within {(time.monotonic() - t0) * 1000:.0f} ms")
    time.sleep(1)
    check(not [m for _, m in ws.of_type("dispenseProgress", m0) if m.get("pumpId") == 2],
          "Remaining steps skipped")
    ok, raw = ws.request("pumpRaw", pumpId=1, command="status")
    check(ok and "IDLE" in str(raw), "The pump itself reports IDLE", raw.get("data", {}).get("lines"))

    section("5 Missing pump")
    ws.request("simFault", pumpId=4, fault="missing", value=True)
    ok, _ = ws.request("rescan")
    err = ws.wait(lambda m: m.get("type") == "state" and any(e["code"] == "E_PUMP_MISSING" and e.get("pumpId") == 4
                                                            for e in m["errors"]), 30)
    check(err is not None, "Pump 4 raises E_PUMP_MISSING")
    ws.wait_idle()
    time.sleep(1)
    ok, r = ws.request("dispense", recipeId="lime", volumeMl=20)
    check(not ok, "Recipe that needs it is blocked", r.get("message"))
    m0 = ws.mark()
    ok, r = ws.request("dispense", recipeId="tequila", volumeMl=10)
    check(ok, "Recipe without it still dispenses", r.get("message", ""))
    ws.wait_since(m0, lambda m: m.get("type") == "dispenseProgress" and m["status"] in ("COMPLETE", "ABORTED"), 20)
    ws.wait_idle()
    ws.request("simFault", pumpId=4, fault="missing", value=False)
    ws.request("rescan")
    clear = ws.wait(lambda m: m.get("type") == "state" and not any(e["code"] == "E_PUMP_MISSING" for e in m["errors"]), 30)
    check(clear is not None and ws.wait_idle(), "Rescan clears the error")

    section("11 Log")
    second = Ws(board.host)
    hist = second.wait(lambda m: m.get("type") == "logHistory", 5)
    second.close()
    check(hist and 0 < len(hist["lines"]) <= 300, "A new browser gets the log history", hist and len(hist["lines"]))

    section("14-16 Prime All / Purge All")
    time.sleep(1)
    m0 = ws.mark()
    ok, r = ws.request("primeAll")
    check(ok, "Prime All accepted", r.get("message", ""))
    ok2, r2 = ws.request("dispense", recipeId="tequila", volumeMl=10)
    check(not ok2 and r2.get("code") == "BUSY", "Dispense refused during Prime All (16)", r2.get("code"))
    end = ws.wait_since(m0, lambda m: m.get("type") == "bulkProgress" and m["status"] in ("COMPLETE", "ABORTED"), 20)
    check(end and end["status"] == "COMPLETE", "Prime All completed")
    done = [(m["pumpId"], m.get("durationMs")) for _, m in ws.of_type("bulkProgress", m0) if m["status"] == "RUNNING"]
    check([d[0] for d in done] == [1, 4], "Runs pumps 1 then 4; pump 2 (0 s) doesn't run", done)
    ws.wait_idle()
    m0 = ws.mark()
    t0 = time.monotonic()
    ws.request("purgeAll")
    end = ws.wait_since(m0, lambda m: m.get("type") == "bulkProgress" and m["status"] in ("COMPLETE", "ABORTED"), 20)
    took = time.monotonic() - t0
    check(end and end["status"] == "COMPLETE" and 4.2 < took < 7, "Purge All runs 1.5x as long (3 s + 1.5 s)", f"{took:.1f} s total")
    ws.wait_idle()
    m0 = ws.mark()
    ws.request("primeAll")
    ws.wait_since(m0, lambda m: m.get("type") == "bulkProgress" and m["status"] == "RUNNING" and m.get("pumpId") == 4, 8)
    time.sleep(0.2)
    t0 = time.monotonic()
    ws.request("estop")
    ab = ws.wait_since(m0, lambda m: m.get("type") == "bulkProgress" and m["status"] == "ABORTED", 1)
    check(ab is not None, "E-Stop aborts Prime All", f"{(time.monotonic() - t0) * 1000:.0f} ms")
    ws.wait_idle()

    section("19 / 21 Manual prime and purge, one pump at a time")
    time.sleep(0.5)
    ok, _ = ws.request("prime", pumpId=1)
    time.sleep(0.8)
    check(ok and ws.pump(1)["activity"] == "PRIMING", "Prime runs until stopped")
    ok2, r2 = ws.request("primeAll")
    check(not ok2 and r2.get("code") == "BUSY", "Prime All refused during a manual run", r2.get("code"))
    overlap = [False]
    def watch():
        for _ in range(200):
            act = [(ws.pump(i) or {}).get("activity", "IDLE") for i in (1, 2, 4)]
            overlap[0] |= sum(a != "IDLE" for a in act) > 1
            time.sleep(0.01)
    w = threading.Thread(target=watch)
    w.start()
    ok, _ = ws.request("purge", pumpId=4)
    w.join()
    check(ok and ws.pump(1)["activity"] == "IDLE" and ws.pump(4)["activity"] == "PURGING",
          "Purge on another pump stops the first, then starts")
    check(not overlap[0], "Never two pumps at once")
    ok, _ = ws.request("pumpStop", pumpId=4)
    time.sleep(0.5)
    check(ok and ws.pump(4)["activity"] == "IDLE", "Stop works")
    ws.wait_idle()
    ok, r = ws.request("pumpRaw", pumpId=2, command="prime 1000")
    check(not ok and r.get("code") == "INVALID", "Raw console refuses motion commands (21)", r.get("code"))
    ok, r = ws.request("pumpSetId", pumpId=0, newId=9)
    check(not ok and r.get("code") == "INVALID", "The bridge board's ID can't be changed (23)", r.get("code"))
    ok, r = ws.request("pumpRaw", pumpId=0, command="gid")
    check(ok and "P,ID,0" in str(r), "Raw console reaches the bridge board at ID 0 (23)", r.get("data", {}).get("lines"))
    pumps = ws.latest("pumps")["pumps"]
    check(not any(p["isBridge"] for p in pumps) and not any(p["pumpId"] == 0 for p in pumps),
          "The bridge board isn't in the pump list (23)")

    section("A-13 Client limit")
    extra, refused = [], False
    try:
        for _ in range(4):
            extra.append(Ws(board.host))
        time.sleep(1)
        refused = any(e.closed for e in extra)
    except (OSError, ConnectionError):
        refused = True
    check(refused, "A 5th browser is refused", f"close code {[e.close_code for e in extra if e.closed]}")
    for e in extra:
        e.close()
    time.sleep(1)

    section("Slow client")
    # A browser that stops reading must not lose its connection or affect others
    stuck = socket.create_connection((board.host, 80), timeout=5)
    stuck.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 512)
    stuck.sendall((f"GET /ws HTTP/1.1\r\nHost: {board.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    time.sleep(1)
    m0 = ws.mark()
    for _ in range(6):
        ws.request("primeAll")
        ws.wait_since(ws.mark(), lambda m: m.get("type") == "state" and m["system"] == "READY", 6)
        time.sleep(0.3)
    ws.wait_idle()
    check(not ws.closed and len(ws.of_type("state", m0)) > 5, "Other browsers keep getting updates",
          f"{len(ws.of_type('state', m0))} state messages")
    stuck.close()
    time.sleep(1)
    check(board.info()["uptimeMs"] > 1000, "Board is fine afterward")

    section("24 Pump firmware: upload checks")
    def up(data, name="x.bin"):
        status, body = board.upload("/api/pump-firmware", data, name)
        try:
            return status, json.loads(body).get("message", "")
        except ValueError:
            return status, body[:80]
    time.sleep(1)
    for name, data, word in (("Junk", os.urandom(4000), "DBP-IMAGE"),
                             ("Empty", b"", ""),
                             ("Compressed", gzip.compress(fake_image()), "Compressed"),
                             ("UF2", b"UF2\n" + fake_image(), "UF2"),
                             ("Too old", fake_image("0.5.0"), "older"),
                             ("Wrong bus speed", fake_image("9.0.0", "115200"), "115200"),
                             ("No bus tag", b"\0" * 900 + b"DBP-IMAGE:9.0.0\0" + b"\0" * 900, "DBP-BUS"),
                             ("Too large", fake_image("9.0.0", "50000", 401 * 1024), "")):
        status, msg = up(data)
        check(status == 400 and word in msg, f"{name} refused", msg[:70])
    pf = ws.wait(lambda m: m.get("type") == "pumpFw", 5)
    check(pf and pf["image"] is None, "Nothing was stored", pf and pf["image"])

    image = open(pump_image, "rb").read()
    md5 = hashlib.md5(image).hexdigest()
    t0 = time.monotonic()
    status, msg = up(image, os.path.basename(pump_image))
    took = time.monotonic() - t0
    check(status == 200, "Real pump firmware accepted", f"{len(image)} bytes in {took:.1f} s {msg}")
    pf = ws.wait(lambda m: m.get("type") == "pumpFw" and m["image"], 5)
    check(pf and pf["image"]["md5"] == md5, "Controller's MD5 matches the file", pf and pf["image"])
    version = pf["image"]["version"]
    check(sorted(t["pumpId"] for t in pf["targets"]) == [0, 1, 2, 4] and pf["targets"][-1]["pumpId"] == 0,
          "Plan: pumps in order, bridge last",
          f"{[t['pumpId'] for t in pf['targets']]} needsUsb {pf['needsUsb']} missing {pf['missing']}")
    time.sleep(3)
    pf = ws.latest("pumpFw")
    check(not pf["update"]["active"] and pf["update"]["result"] == "" and ws.system() in ("READY", "ERROR")
          and ws.pump(1)["fwVersion"] != version, "Storing the file updated nothing (D-17)")

    section("24 Pump firmware: survives a reboot")
    ok, _ = ws.request("reboot")
    ws.close()
    time.sleep(3)
    ws = board.connect()
    pf = ws.wait(lambda m: m.get("type") == "pumpFw", 5)
    check(pf and pf["image"] and pf["image"]["md5"] == md5, "Stored file and MD5 still correct after reboot")
    check(board.info()["uptimeMs"] < 40000, "Board really rebooted", board.info()["uptimeMs"])
    ws.wait(lambda m: m.get("type") == "pumps" and len(m["pumps"]) == 3 and m["pumps"][0]["present"], 15)
    check(len(ws.latest("pumps")["pumps"]) == 3, "Config survived the reboot")

    section("24 Pump firmware: E-Stop and Cancel")
    ws.wait_idle()
    time.sleep(2)
    ok, r = ws.request("pumpFwUpdate")
    check(ok, "Update started by the command", r.get("message", ""))
    ws.wait(lambda m: m.get("type") == "pumpFw" and m["update"]["bytes"] > 2000, 30)
    ok2, r2 = ws.request("prime", pumpId=2)
    check(not ok2 and r2.get("code") == "BUSY", "Motor commands refused during an update", r2.get("code"))
    ws.request("estop")
    pf = ws.wait(lambda m: m.get("type") == "pumpFw" and not m["update"]["active"] and m["update"]["result"], 5)
    check(pf and pf["update"]["result"] == "CANCELLED", "E-Stop cancels the transfer", pf and pf["update"]["message"])
    ws.wait_idle()
    time.sleep(3)
    check(ws.pump(1)["fwVersion"] != version, "That pump keeps its old firmware", ws.pump(1)["fwVersion"])
    ok, r = ws.request("pumpFwUpdate")
    ws.wait(lambda m: m.get("type") == "pumpFw" and m["update"]["active"] and m["update"]["bytes"] > 2000, 30)
    ok, _ = ws.request("pumpFwCancel")
    pf = ws.wait(lambda m: m.get("type") == "pumpFw" and not m["update"]["active"] and m["update"]["result"] == "CANCELLED", 15)
    check(pf is not None, "Cancel stops the transfer")
    ws.wait_idle()
    time.sleep(3)

    section("24 Pump firmware: full update from flash storage")
    heap0 = board.info()["freeHeap"]
    m0 = ws.mark()
    t0 = time.monotonic()
    ok, r = ws.request("pumpFwUpdate")
    check(ok, "Update started", r.get("message", ""))
    order = []
    end = time.monotonic() + 600
    pf = None
    lowest_heap = heap0
    last_change = time.monotonic()
    last_bytes = None
    while time.monotonic() < end:
        if ws.latest("pumpFw")["update"]["bytes"] != last_bytes:
            last_bytes = ws.latest("pumpFw")["update"]["bytes"]
            last_change = time.monotonic()
            lowest_heap = min(lowest_heap, board.info()["freeHeap"])
        if time.monotonic() - last_change > 45:
            check(False, "Progress keeps arriving during the update", f"none for 45 s, at {last_bytes} bytes")
            break
        if ws.closed:
            check(False, "WebSocket stayed open during the update", f"close code {ws.close_code}, {ws.error}")
            break
        pf = ws.latest("pumpFw")
        u = pf["update"]
        if u["active"] and (not order or order[-1] != u["pumpId"]):
            order.append(u["pumpId"])
        if not u["active"] and u["result"] and time.monotonic() - t0 > 2:
            break
        time.sleep(0.05)
    took = time.monotonic() - t0
    check(pf["update"]["result"] == "COMPLETE", "Update completed", f"{pf['update']['message']} in {took:.0f} s")
    check(order == [1, 2, 4, 0], "Boards in order, bridge last", order)
    ws.wait_idle()
    time.sleep(8)
    versions = {i: ws.pump(i)["fwVersion"] for i in (1, 2, 4)}
    check(all(v == version for v in versions.values()), "Every pump reports the new version", versions)
    pf = ws.latest("pumpFw")
    check(pf["bridgeVersion"] == version and not pf["targets"], "Bridge too, and nothing left to update", pf["bridgeVersion"])
    st = ws.latest("state")
    check(not st["errors"], "No errors, including no E_USB_LOST", [e["code"] for e in st["errors"]])
    check(ws.request("pumpFwUpdate")[1].get("code") == "INVALID", "A second press has nothing to do")
    ok, _ = ws.request("pumpFwDelete")
    pf = ws.wait(lambda m: m.get("type") == "pumpFw" and m["image"] is None, 5)
    check(ok and pf is not None, "Stored file can be deleted")
    heap1 = board.info()["freeHeap"]
    check(heap1 > heap0 - 20000 and lowest_heap > 60000, "Heap stays healthy during and after the update",
          f"{heap0} -> {heap1} bytes, lowest seen {lowest_heap}")
    return ws


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--pump-image", required=True, help="a real DrinkBotPump .bin, 0.7.1 or newer")
    args = ap.parse_args()
    board = Board(args.host)
    print(f"{BOLD}Controller board tests on {args.host}{RESET}")
    original = json.loads(board.get("/api/config")[1])
    ws = board.connect()
    try:
        ws = run(board, ws, original, args.pump_image)
    except Exception as e:  # keep the results, and always restore the config
        check(False, "Test run stopped by an error", repr(e))
    finally:
        SECTION[0] = "Restore"
        print(f"\n{BOLD}== Restoring the original config =={RESET}")
        status, body = board.post("/api/config", json.dumps(original).encode(), "application/json")
        check(status == 200, "Original config restored", status)
        now = json.loads(board.get("/api/config")[1])
        check(now.get("wifi") == original.get("wifi") and now.get("pumps") == original.get("pumps"),
              "Wi-Fi and pumps match what was there before")
        ws.close()

    failed = [r for r in results if not r[2]]
    print(f"\n{BOLD}{len(results) - len(failed)} pass, {len(failed)} fail{RESET}")
    for sec, name, _, detail in failed:
        print(f"  {RED}FAIL{RESET} [{sec}] {name}  {detail}")
    info = board.info()
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "reports")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "_board.md")
    with open(path, "w") as f:
        f.write(f"# Controller board test\n\n- Firmware build: {info['buildDate']}\n- Simulator mode, nothing attached\n"
                f"- Result: {len(results) - len(failed)} pass, {len(failed)} fail\n\n"
                "| Section | Check | Result | Detail |\n|---|---|---|---|\n")
        for sec, name, ok, detail in results:
            f.write(f"| {sec} | {name} | {'PASS' if ok else 'FAIL'} | {detail.replace('|', '/')} |\n")
    print("Report:", os.path.normpath(path))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
