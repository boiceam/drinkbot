#!/usr/bin/env python3
"""DrinkBotController mock server, for developing the web UI without hardware.

Serves ../web/ and fakes the controller's WebSocket (/ws, SPEC_v3 §14) and HTTP
API (/api/config, /api/info, /api/ota, §13) with 8 simulated pumps (§16).

Standard library only, Python 3.8+.

    python3 tools/mock_server.py [--port 8080] [--speed 1.0] [--web DIR]

Then open http://localhost:8080/. --speed > 1 makes pours, primes and purges
run faster (wall-clock), which is handy when testing the progress views.

Protocol choices the spec leaves open (keep them in step with the firmware):
- Validation errors: error.details (WS) and errors (HTTP 400) are lists of
  {"path": "pumps[0].msPerMl", "message": "..."}.
- dispenseProgress / bulkProgress `step` is 1-based; the opening STARTING
  bulkProgress uses step 0.
- Simulator fault injection (only when hello.simulator is true):
  {"type":"simFault","pumpId":N,"fault":"missing","value":true|false}
  {"type":"simFault","pumpId":N,"fault":"timeout"}   next command times out
  {"type":"simFault","pumpId":N,"fault":"overrun"}   next motion never stops
  {"type":"simFault","pumpId":N,"fault":"weight","weightG":123.4}
"""

import argparse
import base64
import copy
import hashlib
import json
import os
import random
import re
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
FW_VERSION = "0.1.0-mock"
BUILD_DATE = time.strftime("%Y-%m-%d %H:%M")
LEVELS = ["ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"]
BULK_GAP_MS = 500
# Nominal flow of the physical pump: 600 ml/min = 100 ms/ml. Simulated pumps move
# liquid at this rate whatever the config's msPerMl says (that is the calibration).
NOMINAL_ML_PER_MIN = 600.0
NOMINAL_MS_PER_ML = 60000.0 / NOMINAL_ML_PER_MIN
MANUAL_RUN_MS = 120000  # manual prime/purge runs for the pump's longest allowed run (kMaxMotionMs)
SEVERITY = {
    "E_USB_LOST": "blocking", "E_BRIDGE_UNRESPONSIVE": "blocking",
    "E_PUMP_MISSING": "degraded", "E_PUMP_FW": "degraded", "E_CONFIG": "degraded",
    "E_DISPENSE_TIMEOUT": "latched", "E_PUMP_COMM": "latched", "E_STOP_UNCONFIRMED": "latched",
}

SPEED = 1.0
WEB_DIR = ""


def default_config():
    pumps = [
        # msPerMl 100 = calibrated to the pump's nominal 600 ml/min
        (1, "Tequila", 100, 0.95, [255, 160, 0], 4.0),
        (2, "Triple Sec", 100, 1.05, [255, 120, 40], 3.0),
        (3, "Lime Juice", 100, 1.03, [120, 255, 40], 3.0),
        (4, "Grapefruit Soda", 100, 1.02, [255, 90, 120], 2.5),
        (5, "Vodka", 100, 0.95, [180, 220, 255], 4.0),
        (6, "Gin", 100, 0.94, [90, 160, 255], 0),
        (7, "Tonic", 100, 1.03, [220, 255, 255], 2.0),
        (8, "Cranberry", 100, 1.04, [200, 0, 40], 3.5),
    ]
    return {
        "schemaVersion": 2,
        "device": {"hostname": "drinkbot", "apSsid": "DrinkBot"},
        "wifi": {"networks": [{"ssid": "HomeWiFi", "password": "secret123"}]},
        "levels": {"lowMl": 150, "emptyMl": 50, "actualEmptyMl": 20},
        "dispense": {"verifyTolerancePct": 15, "verifyToleranceMinMl": 5, "settleMs": 1500,
                     "minVolumeMl": 10, "maxVolumeMl": 1000},
        "defaults": {"volumeMl": 250, "recipeId": "margarita", "primeSec": 5, "purgeSec": 10},
        "ui": {"ledBrightness": 64},
        "pumps": [{"pumpId": i, "liquid": l, "msPerMl": m, "densityGPerMl": d, "color": c,
                   "autoPrimeSec": a} for (i, l, m, d, c, a) in pumps],
        "recipes": [
            {"id": "margarita", "name": "Margarita", "ingredients": [
                {"liquid": "Tequila", "parts": 2, "order": 1},
                {"liquid": "Triple Sec", "parts": 1, "order": 2},
                {"liquid": "Lime Juice", "parts": 1, "order": 3}]},
            {"id": "paloma", "name": "Paloma", "ingredients": [
                {"liquid": "Tequila", "parts": 1, "order": 1},
                {"liquid": "Lime Juice", "parts": 0.25, "order": 2},
                {"liquid": "Grapefruit Soda", "parts": 3, "order": 3}]},
            {"id": "gin-tonic", "name": "Gin & Tonic", "ingredients": [
                {"liquid": "Gin", "parts": 1, "order": 1},
                {"liquid": "Tonic", "parts": 3, "order": 2}]},
            {"id": "cosmopolitan", "name": "Cosmopolitan", "ingredients": [
                {"liquid": "Vodka", "parts": 1.5, "order": 1},
                {"liquid": "Triple Sec", "parts": 1, "order": 2},
                {"liquid": "Cranberry", "parts": 1, "order": 3},
                {"liquid": "Lime Juice", "parts": 0.5, "order": 4}]},
            {"id": "mojito", "name": "Mojito", "ingredients": [
                {"liquid": "White Rum", "parts": 2, "order": 1},
                {"liquid": "Lime Juice", "parts": 1, "order": 2}]},
        ],
        "buttons": [
            {"action": "setRecipe", "recipeId": "margarita"},
            {"action": "setRecipe", "recipeId": "paloma"},
            {"action": "setVolume", "volumeMl": 150},
            {"action": "setVolume", "volumeMl": 250},
            {"action": "setVolume", "volumeMl": 350},
            {"action": "dispense"},
            {"action": "estop"},
        ],
    }


# --------------------------------------------------------------------------- validation (§7.3)

def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _int(v):
    return _num(v) and float(v) == int(v)


def validate_config(cfg):
    errs = []

    def e(path, msg):
        errs.append({"path": path, "message": msg})

    if not isinstance(cfg, dict):
        e("", "config must be an object")
        return errs
    for sec in ("device", "wifi", "levels", "dispense", "defaults", "ui"):
        if not isinstance(cfg.get(sec), dict):
            e(sec, "missing section")
    for sec in ("pumps", "recipes", "buttons"):
        if not isinstance(cfg.get(sec), list):
            e(sec, "must be a list")
    if errs:
        return errs

    dev = cfg["device"]
    if not isinstance(dev.get("hostname"), str) or not re.match(r"^[A-Za-z0-9-]{1,32}$", dev["hostname"].strip()):
        e("device.hostname", "1-32 letters, digits or '-'")
    if not isinstance(dev.get("apSsid"), str) or not 1 <= len(dev["apSsid"].strip().encode()) <= 32:
        e("device.apSsid", "1-32 bytes")

    nets = cfg["wifi"].get("networks")
    if not isinstance(nets, list):
        e("wifi.networks", "must be a list")
    else:
        if len(nets) > 8:
            e("wifi.networks", "at most 8 networks")
        for i, n in enumerate(nets):
            p = "wifi.networks[%d]" % i
            ssid = str(n.get("ssid", "")).strip()
            if not 1 <= len(ssid.encode()) <= 32:
                e(p + ".ssid", "1-32 bytes")
            pw = str(n.get("password", ""))
            if pw and not 8 <= len(pw) <= 63:
                e(p + ".password", "empty or 8-63 characters")

    lv = cfg["levels"]
    for k in ("lowMl", "emptyMl", "actualEmptyMl"):
        if not _num(lv.get(k)):
            e("levels." + k, "must be a number")
    if not errs or all(not x["path"].startswith("levels") for x in errs):
        if not lv["actualEmptyMl"] >= 0:
            e("levels.actualEmptyMl", "must be >= 0")
        if not lv["actualEmptyMl"] < lv["emptyMl"]:
            e("levels.emptyMl", "must be greater than actualEmptyMl")
        if not lv["emptyMl"] < lv["lowMl"]:
            e("levels.lowMl", "must be greater than emptyMl")

    d = cfg["dispense"]
    for k, lo, hi in (("verifyTolerancePct", 0, 100), ("verifyToleranceMinMl", 0, 1000),
                      ("settleMs", 0, 10000), ("minVolumeMl", 1, 1000), ("maxVolumeMl", 1, 1000)):
        if not _num(d.get(k)) or not lo <= d[k] <= hi:
            e("dispense." + k, "must be %g-%g" % (lo, hi))
    vmin, vmax = d.get("minVolumeMl", 10), d.get("maxVolumeMl", 1000)
    if _num(vmin) and _num(vmax) and vmin >= vmax:
        e("dispense.maxVolumeMl", "must be greater than minVolumeMl")

    ui = cfg["ui"]
    if not _int(ui.get("ledBrightness")) or not 0 <= ui["ledBrightness"] <= 255:
        e("ui.ledBrightness", "integer 0-255")

    pumps = cfg["pumps"]
    if len(pumps) > 16:
        e("pumps", "at most 16 pumps")
    ids, liquids = {}, {}
    for i, p in enumerate(pumps):
        pp = "pumps[%d]" % i
        if not _int(p.get("pumpId")) or not 1 <= p["pumpId"] <= 254:
            e(pp + ".pumpId", "integer 1-254")
        elif p["pumpId"] in ids:
            e(pp + ".pumpId", "duplicate pump ID %d" % p["pumpId"])
        else:
            ids[p["pumpId"]] = i
        liq = str(p.get("liquid", "")).strip()
        if len(liq) > 32:
            e(pp + ".liquid", "at most 32 characters")
        if liq:
            if liq.lower() in liquids:
                e(pp + ".liquid", "duplicate liquid '%s'" % liq)
            liquids[liq.lower()] = i
        if not _int(p.get("msPerMl")) or not 1 <= p["msPerMl"] <= 65535:
            e(pp + ".msPerMl", "integer 1-65535")
        if not _num(p.get("densityGPerMl")) or not 0.5 <= p["densityGPerMl"] <= 2.0:
            e(pp + ".densityGPerMl", "must be 0.5-2.0")
        c = p.get("color")
        if not isinstance(c, list) or len(c) != 3 or not all(_int(x) and 0 <= x <= 255 for x in c):
            e(pp + ".color", "3 integers 0-255")
        a = p.get("autoPrimeSec")
        if not _num(a) or not 0 <= a <= 60 or abs(a * 10 - round(a * 10)) > 1e-6:
            e(pp + ".autoPrimeSec", "0-60 s in 0.1 s steps")

    recipes = cfg["recipes"]
    if len(recipes) > 64:
        e("recipes", "at most 64 recipes")
    rids = set()
    for i, r in enumerate(recipes):
        rp = "recipes[%d]" % i
        rid = str(r.get("id", ""))
        if not re.match(r"^[a-z0-9-]{1,32}$", rid):
            e(rp + ".id", "1-32 of a-z, 0-9, '-'")
        elif rid in rids:
            e(rp + ".id", "duplicate id '%s'" % rid)
        rids.add(rid)
        name = str(r.get("name", "")).strip()
        if not 1 <= len(name) <= 32:
            e(rp + ".name", "1-32 characters")
        ings = r.get("ingredients")
        if not isinstance(ings, list) or not 1 <= len(ings) <= 16:
            e(rp + ".ingredients", "1-16 ingredients")
            continue
        for j, g in enumerate(ings):
            gp = "%s.ingredients[%d]" % (rp, j)
            if not 1 <= len(str(g.get("liquid", "")).strip()) <= 32:
                e(gp + ".liquid", "1-32 characters")
            if not _num(g.get("parts")) or not g["parts"] > 0:
                e(gp + ".parts", "must be > 0")
            if not _int(g.get("order")) or not g["order"] >= 0:
                e(gp + ".order", "integer >= 0")

    btns = cfg["buttons"]
    if len(btns) != 7:
        e("buttons", "exactly 7 buttons")
    for i, b in enumerate(btns):
        bp = "buttons[%d]" % i
        a = b.get("action")
        if a not in ("none", "setRecipe", "setVolume", "dispense", "estop"):
            e(bp + ".action", "unknown action")
        elif a == "setRecipe" and b.get("recipeId") not in rids:
            e(bp + ".recipeId", "recipe does not exist")
        elif a == "setVolume" and (not _num(b.get("volumeMl")) or not vmin <= b["volumeMl"] <= vmax):
            e(bp + ".volumeMl", "must be %g-%g ml" % (vmin, vmax))

    df = cfg["defaults"]
    if not _num(df.get("volumeMl")) or not vmin <= df["volumeMl"] <= vmax:
        e("defaults.volumeMl", "must be %g-%g ml" % (vmin, vmax))
    if df.get("recipeId", "") not in rids and df.get("recipeId", "") != "":
        e("defaults.recipeId", "recipe does not exist")
    for k in ("primeSec", "purgeSec"):
        if not _num(df.get(k)) or not 1 <= df[k] <= 60:
            e("defaults." + k, "must be 1-60 s")
    return errs


def trim_strings(o):
    if isinstance(o, dict):
        return {k: trim_strings(v) for k, v in o.items()}
    if isinstance(o, list):
        return [trim_strings(v) for v in o]
    if isinstance(o, str):
        return o.strip()
    return o


# --------------------------------------------------------------------------- WebSocket client

class WsClient(object):
    def __init__(self, handler):
        self.h = handler
        self.lock = threading.Lock()
        self.alive = True
        self.ip = handler.client_address[0]

    def send(self, obj):
        if not self.alive:
            return
        data = json.dumps(obj, separators=(",", ":")).encode()
        n = len(data)
        if n < 126:
            hdr = struct.pack("!BB", 0x81, n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x81, 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x81, 127, n)
        try:
            with self.lock:
                self.h.wfile.write(hdr + data)
                self.h.wfile.flush()
        except (OSError, ValueError):
            self.alive = False

    def close(self):
        self.alive = False
        try:
            with self.lock:
                self.h.wfile.write(b"\x88\x02\x03\xe8")
                self.h.wfile.flush()
            self.h.connection.shutdown(socket.SHUT_RDWR)
        except (OSError, ValueError):
            pass

    def read_message(self):
        """Returns a text message, or None when the socket closes."""
        buf, opcode = b"", None
        rf = self.h.rfile
        while True:
            head = rf.read(2)
            if len(head) < 2:
                return None
            b1, b2 = head[0], head[1]
            fin, op = b1 & 0x80, b1 & 0x0F
            ln = b2 & 0x7F
            if ln == 126:
                ln = struct.unpack("!H", rf.read(2))[0]
            elif ln == 127:
                ln = struct.unpack("!Q", rf.read(8))[0]
            mask = rf.read(4) if b2 & 0x80 else b"\0\0\0\0"
            payload = bytearray(rf.read(ln))
            for i in range(len(payload)):
                payload[i] ^= mask[i % 4]
            if op == 8:
                return None
            if op == 9:
                with self.lock:
                    self.h.wfile.write(bytes([0x8A, len(payload)]) + bytes(payload))
                    self.h.wfile.flush()
                continue
            if op == 10:
                continue
            if op != 0:
                opcode = op
            buf += bytes(payload)
            if fin:
                return buf.decode("utf-8", "replace") if opcode == 1 else ""


# --------------------------------------------------------------------------- simulation

class Abort(Exception):
    pass


class Stopped(Exception):
    """A manual prime/purge was stopped by the user (or replaced by another)."""


class Sim(object):
    def __init__(self):
        self.lock = threading.RLock()
        self.clients = []
        self.boot = time.time()
        self.config = default_config()
        self.system = "BOOTING"
        self.errors = []
        self.web_level = "INFO"
        self.logs = []
        self.op_thread = None
        self.abort = threading.Event()
        self.manual_stop = threading.Event()
        self.manual_pid = None    # pump running a manual prime/purge
        self.manual_next = None   # (pid, direction) to start once it has stopped
        self.active_recipe = self.config["defaults"]["recipeId"]
        self.active_volume = self.config["defaults"]["volumeMl"]
        self.pumps = {}
        self.faults = {}
        self.last_pumps_bcast = 0
        self.pumps_dirty = False
        self.state_dirty = False
        self.rebuild_pumps()
        self.log("INFO", "SYS", "DrinkBotController %s (mock) booting" % FW_VERSION)
        self.log("INFO", "SIM", "No USB device: simulator mode with %d pumps" % len(self.pumps))
        self.log("INFO", "NET", "STA connected to HomeWiFi, IP 127.0.0.1, RSSI -58 dBm")
        self.system = "READY"
        self.recompute_errors()

    # ----- helpers
    def uptime(self):
        return int((time.time() - self.boot) * 1000)

    def log(self, level, tag, msg):
        line = {"t": self.uptime(), "level": level, "tag": tag, "msg": msg}
        with self.lock:
            self.logs.append(line)
            del self.logs[:-300]
        print("[%8d] %-5s %-4s - %s" % (line["t"], level, tag, msg))
        if LEVELS.index(level) <= LEVELS.index(self.web_level):
            self.broadcast(dict(type="log", **line))

    def broadcast(self, obj):
        for c in list(self.clients):
            c.send(obj)
            if not c.alive and c in self.clients:
                self.clients.remove(c)

    def rebuild_pumps(self):
        old = self.pumps
        self.pumps = {}
        cfg_ids = [p["pumpId"] for p in self.config["pumps"]]
        ids = list(cfg_ids)
        n = 1
        while len(ids) < 8:
            if n not in ids:
                ids.append(n)
            n += 1
        for idx, pid in enumerate(ids):
            pc = self.pump_cfg(pid)
            dens = pc["densityGPerMl"] if pc else 1.0
            prev = old.get(pid)
            self.pumps[pid] = {
                "pumpId": pid, "fwVersion": "0.6.0-sim", "isBridge": idx == 0,
                "weightG": prev["weightG"] if prev else round(750 * dens * random.uniform(0.3, 1.0), 1),
                "activity": "IDLE", "motion": None,
                "led": {"r": 0, "g": 0, "b": 0, "mode": 1, "brightness": 0},
            }
            self.faults.setdefault(pid, {"missing": False, "timeout": False, "overrun": False})
        # low/empty examples for the UI
        if not old:
            if 4 in self.pumps:
                self.pumps[4]["weightG"] = 120 * 1.02
            if 7 in self.pumps:
                self.pumps[7]["weightG"] = 40 * 1.03
        for pid in self.pumps:
            self.set_idle_led(pid)
        self.pumps_dirty = True

    def pump_cfg(self, pid):
        for p in self.config["pumps"]:
            if p["pumpId"] == pid:
                return p
        return None

    def pump_by_liquid(self, liquid):
        for p in self.config["pumps"]:
            if p["liquid"] and p["liquid"].lower() == liquid.lower():
                return p
        return None

    def present(self, pid):
        return pid in self.pumps and not self.faults[pid]["missing"]

    def level_ml(self, pid):
        pc = self.pump_cfg(pid)
        if not pc or not self.present(pid):
            return None
        return max(0.0, self.pumps[pid]["weightG"] / pc["densityGPerMl"])

    def level_state(self, pid):
        lv = self.level_ml(pid)
        if lv is None:
            return "UNKNOWN"
        L = self.config["levels"]
        if lv <= L["emptyMl"]:
            return "EMPTY"
        if lv <= L["lowMl"]:
            return "LOW"
        return "OK"

    def set_idle_led(self, pid):
        st = self.level_state(pid)
        led = {"r": 0, "g": 0, "b": 0, "mode": 1, "brightness": 0}
        if st == "LOW":
            led = {"r": 255, "g": 180, "b": 0, "mode": 2, "brightness": 255}
        elif st == "EMPTY":
            led = {"r": 255, "g": 0, "b": 0, "mode": 2, "brightness": 255}
        self.pumps[pid]["led"] = led
        self.pumps[pid]["lastState"] = st

    def recompute_errors(self):
        keep = [e for e in self.errors if e["latched"]]
        for pid in sorted(self.pumps):
            if self.pump_cfg(pid) and self.faults[pid]["missing"]:
                keep.append({"code": "E_PUMP_MISSING", "pumpId": pid,
                             "message": "Pump %d not responding" % pid, "latched": False})
        self.errors = keep
        if self.system in ("READY", "ERROR"):
            self.system = "ERROR" if self.errors else "READY"
        self.state_dirty = True

    def raise_error(self, code, pid, msg):
        with self.lock:
            self.errors.append({"code": code, "pumpId": pid, "message": msg,
                                "latched": SEVERITY[code] == "latched"})
            self.log("ERROR", "SYS", "Error raised: %s(%s) %s" % (code, pid, msg))
            self.state_dirty = True

    def blocking(self):
        return any(SEVERITY.get(e["code"]) in ("blocking", "latched") for e in self.errors)

    def leds(self):
        def c(col, mode):
            r, g, b = {"green": (0, 255, 0), "red": (255, 0, 0), "yellow": (255, 180, 0),
                       "white": (255, 255, 255), "off": (0, 0, 0)}[col]
            return {"r": r, "g": g, "b": b, "mode": mode}
        out = []
        for b in self.config["buttons"]:
            a = b.get("action")
            if a == "setRecipe" and b.get("recipeId") == self.active_recipe:
                out.append(c("red", "solid"))
            elif a == "setVolume" and b.get("volumeMl") == self.active_volume:
                out.append(c("red", "solid"))
            elif a == "dispense" and self.system == "DISPENSING":
                out.append(c("yellow", "blink"))
            else:
                out.append(c("off", "off"))
        while len(out) < 7:
            out.append(c("off", "off"))
        s = self.system
        if s == "BOOTING":
            out.append(c("white", "solid"))
        elif s in ("DISPENSING", "MANUAL"):
            out.append(c("red", "blink") if self.errors else c("yellow", "blink"))
        elif s == "ERROR":
            out.append(c("red", "blink"))
        else:
            out.append(c("green", "solid"))
        out.append(c("green", "solid"))
        return out

    # ----- messages
    def state_msg(self):
        return {"type": "state", "system": self.system, "errors": self.errors,
                "activeRecipeId": self.active_recipe, "activeVolumeMl": self.active_volume,
                "wifi": {"mode": "STA", "ssid": "HomeWiFi", "ip": "127.0.0.1",
                         "rssi": -55 - random.randint(0, 6)},
                "leds": self.leds()}

    def pumps_msg(self):
        out = []
        for pid in sorted(self.pumps):
            p = self.pumps[pid]
            pc = self.pump_cfg(pid)
            lv = self.level_ml(pid)
            pres = self.present(pid)
            out.append({"pumpId": pid, "liquid": pc["liquid"] if pc else "", "present": pres,
                        "fwVersion": p["fwVersion"] if pres else "", "isBridge": p["isBridge"],
                        "weightG": round(p["weightG"], 1) if pres else None,
                        "levelMl": round(lv, 1) if lv is not None else None,
                        "levelState": self.level_state(pid), "activity": p["activity"],
                        "led": p["led"]})
        return {"type": "pumps", "pumps": out}

    def hello(self):
        return {"type": "hello", "fwVersion": FW_VERSION, "buildDate": BUILD_DATE,
                "simulator": True, "hostname": self.config["device"]["hostname"]}

    def on_connect(self, c):
        c.send(self.hello())
        c.send(self.state_msg())
        c.send(self.pumps_msg())
        c.send({"type": "config", "config": self.config})
        lvl = LEVELS.index(self.web_level)
        c.send({"type": "logHistory", "lines": [l for l in self.logs if LEVELS.index(l["level"]) <= lvl]})
        self.clients.append(c)
        self.log("INFO", "WEB", "WebSocket client connected from %s (%d clients)" % (c.ip, len(self.clients)))

    # ----- ticking
    def tick_loop(self):
        last = time.time()
        while True:
            time.sleep(0.1)
            now = time.time()
            dt = (now - last) * 1000 * SPEED
            last = now
            with self.lock:
                for pid, p in self.pumps.items():
                    m = p["motion"]
                    if m:
                        pc = self.pump_cfg(pid)
                        mpm = NOMINAL_MS_PER_ML
                        dens = pc["densityGPerMl"] if pc else 1.0
                        step = min(dt, m["remaining"]) if not m["overrun"] else dt
                        sign = -1 if m["dir"] == "FWD" else 1
                        p["weightG"] = max(0.0, p["weightG"] + sign * step / mpm * dens)
                        m["remaining"] -= dt
                        if m["remaining"] <= 0 and not m["overrun"]:
                            p["motion"] = None
                        self.pumps_dirty = True
                    elif p["activity"] == "IDLE" and random.random() < 0.02:
                        p["weightG"] = max(0.0, p["weightG"] + random.uniform(-0.3, 0.3))
                    if p["activity"] == "IDLE" and self.present(pid):
                        st = self.level_state(pid)
                        if st != p.get("lastState"):
                            self.log("INFO", "PUMP", "Pump %d level %s -> %s" % (pid, p.get("lastState"), st))
                            self.set_idle_led(pid)
                            self.pumps_dirty = True
                if self.state_dirty:
                    self.state_dirty = False
                    self.broadcast(self.state_msg())
                busy = any(p["motion"] for p in self.pumps.values())
                if (self.pumps_dirty and now - self.last_pumps_bcast > (0.25 if busy else 0.1)) \
                        or now - self.last_pumps_bcast > 3:
                    self.pumps_dirty = False
                    self.last_pumps_bcast = now
                    self.broadcast(self.pumps_msg())

    # ----- pump primitives (simulate the PumpLink)
    def sleep(self, ms, stop=None):
        end = time.time() + ms / 1000.0 / SPEED
        while time.time() < end:
            if self.abort.is_set():
                raise Abort("E-Stop")
            if stop is not None and stop.is_set():
                raise Stopped()
            time.sleep(0.02)

    def pump_cmd(self, pid, cmd):
        time.sleep(0.005 if self.pumps.get(pid, {}).get("isBridge") else 0.025)
        if not self.present(pid):
            raise RuntimeError("PUMP_TIMEOUT:pump %d did not answer '%s'" % (pid, cmd))
        if self.faults[pid]["timeout"]:
            self.faults[pid]["timeout"] = False
            self.log("WARN", "PUMP", "Pump %d: timeout on '%s' (injected)" % (pid, cmd))
            raise RuntimeError("PUMP_TIMEOUT:pump %d timed out on '%s'" % (pid, cmd))
        self.log("VERBOSE", "PUMP", "TX %d: %s" % (pid, cmd))

    def run_motion(self, pid, direction, ms, activity, ledmode, stop=None):
        pc = self.pump_cfg(pid)
        col = pc["color"] if pc else [255, 255, 255]
        verb = "dispense" if activity == "DISPENSING" else ("prime" if direction == "FWD" else "purge")
        self.pump_cmd(pid, "%s %d" % (verb, ms))
        p = self.pumps[pid]
        with self.lock:
            p["activity"] = activity
            p["led"] = {"r": col[0], "g": col[1], "b": col[2], "mode": ledmode, "brightness": 255}
            overrun = self.faults[pid]["overrun"]
            self.faults[pid]["overrun"] = False
            p["motion"] = {"dir": direction, "remaining": ms, "overrun": overrun}
            self.pumps_dirty = True
        try:
            self.sleep(ms + 300, stop)
            waited = 0
            while p["motion"]:
                self.sleep(250, stop)
                waited += 250
                if waited > 5000:
                    with self.lock:
                        p["motion"] = None
                    self.raise_error("E_DISPENSE_TIMEOUT", pid, "Pump %d still running after expected time" % pid)
                    raise RuntimeError("PUMP_TIMEOUT:pump %d overran" % pid)
        finally:
            with self.lock:
                p["motion"] = None
                p["activity"] = "IDLE"
                self.set_idle_led(pid)
                self.pumps_dirty = True

    def start_op(self, state, fn):
        def run():
            try:
                fn()
            except Abort:
                pass
            except RuntimeError as ex:
                self.log("ERROR", "SYS", "Operation failed: %s" % str(ex).split(":", 1)[-1])
            finally:
                with self.lock:
                    self.system = "READY"
                    self.recompute_errors()
                    self.op_thread = None
        with self.lock:
            self.abort.clear()
            self.system = state
            self.state_dirty = True
            self.op_thread = threading.Thread(target=run, daemon=True)
            self.op_thread.start()

    # ----- operations
    def precheck_dispense(self, rid, vol):
        rec = next((r for r in self.config["recipes"] if r["id"] == rid), None)
        if not rec:
            raise ReqError("NOT_FOUND", "Unknown recipe '%s'" % rid)
        d = self.config["dispense"]
        if not d["minVolumeMl"] <= vol <= d["maxVolumeMl"]:
            raise ReqError("INVALID", "Volume must be %g-%g ml" % (d["minVolumeMl"], d["maxVolumeMl"]))
        tot = sum(g["parts"] for g in rec["ingredients"])
        steps, need = [], {}
        ings = sorted(enumerate(rec["ingredients"]), key=lambda x: (x[1]["order"], x[0]))
        for _, g in ings:
            pc = self.pump_by_liquid(g["liquid"])
            if not pc:
                raise ReqError("PRECHECK_FAILED", "%s is not loaded on any pump" % g["liquid"])
            if not self.present(pc["pumpId"]):
                raise ReqError("PRECHECK_FAILED", "%s: pump %d is missing" % (g["liquid"], pc["pumpId"]))
            ml = round(vol * g["parts"] / tot, 1)
            steps.append((pc, ml))
            need[pc["pumpId"]] = need.get(pc["pumpId"], 0) + ml
        for pid, ml in need.items():
            if self.level_ml(pid) - ml < self.config["levels"]["actualEmptyMl"]:
                raise ReqError("PRECHECK_FAILED", "Not enough %s (%.0f ml left, %.1f ml needed)"
                               % (self.pump_cfg(pid)["liquid"], self.level_ml(pid), ml))
        return rec, steps

    def do_dispense(self, rid, vol):
        rec, steps = self.precheck_dispense(rid, vol)
        n = len(steps)
        base = {"type": "dispenseProgress", "recipeId": rid, "volumeMl": vol, "steps": n}

        def run():
            self.log("INFO", "DISP", "Dispensing %s, %g ml, %d steps" % (rec["name"], vol, n))
            i, pc, ml = 0, None, 0
            try:
                for i, (pc, ml) in enumerate(steps, 1):
                    pid = pc["pumpId"]
                    self.broadcast(dict(base, step=i, liquid=pc["liquid"], pumpId=pid, targetMl=ml, status="STARTING"))
                    self.log("INFO", "DISP", "Step %d/%d: %s %.1f ml on pump %d" % (i, n, pc["liquid"], ml, pid))
                    w0 = self.pumps[pid]["weightG"]
                    try:
                        self.run_motion(pid, "FWD", int(pc["msPerMl"] * ml), "DISPENSING", 3)
                    except RuntimeError as ex:
                        if not any(e["code"] == "E_DISPENSE_TIMEOUT" and e["pumpId"] == pid for e in self.errors):
                            self.raise_error("E_PUMP_COMM", pid, str(ex).split(":", 1)[-1])
                        raise
                    self.sleep(self.config["dispense"]["settleMs"])
                    actual = round((w0 - self.pumps[pid]["weightG"]) / pc["densityGPerMl"] + random.uniform(-2, 2), 1)
                    tol = max(ml * self.config["dispense"]["verifyTolerancePct"] / 100.0,
                              self.config["dispense"]["verifyToleranceMinMl"])
                    if abs(actual - ml) > tol:
                        self.log("WARN", "DISP", "Step %d: poured %.1f ml, expected %.1f" % (i, actual, ml))
                    self.broadcast(dict(base, step=i, liquid=pc["liquid"], pumpId=pid, targetMl=ml,
                                        actualMl=actual, status="DONE"))
                self.broadcast(dict(base, step=n, liquid="", pumpId=0, targetMl=vol, status="COMPLETE"))
                self.log("INFO", "DISP", "Dispense complete: %s %g ml" % (rec["name"], vol))
            except (Abort, RuntimeError) as ex:
                reason = "E-Stop" if isinstance(ex, Abort) else str(ex).split(":", 1)[-1]
                self.broadcast(dict(base, step=i, liquid=pc["liquid"] if pc else "", pumpId=pc["pumpId"] if pc else 0,
                                    targetMl=ml, status="ABORTED", reason=reason))
                self.log("WARN", "DISP", "Dispense aborted at step %d/%d: %s" % (i, n, reason))
        self.start_op("DISPENSING", run)

    def do_single(self, pid, direction):
        """Manual prime/purge: runs until stopped (at most MANUAL_RUN_MS). Only one pump runs
        at a time: a request for another pump stops the running one first."""
        if not self.pump_cfg(pid):
            raise ReqError("NOT_FOUND", "Pump %d is not configured" % pid)
        if not self.present(pid):
            raise ReqError("PRECHECK_FAILED", "Pump %d is missing" % pid)
        with self.lock:
            if self.manual_pid is not None:
                self.log("INFO", "PUMP", "Stopping pump %d before %s pump %d"
                         % (self.manual_pid, "priming" if direction == "FWD" else "purging", pid))
                self.manual_next = (pid, direction)
                self.manual_stop.set()
                return
            self.manual_pid = pid
            self.manual_next = None
            self.manual_stop.clear()

        def run():
            job = (pid, direction)
            try:
                while job:
                    self.run_single(*job)
                    with self.lock:
                        job, self.manual_next = self.manual_next, None
                        self.manual_stop.clear()
                        self.manual_pid = job[0] if job else None
            finally:
                with self.lock:
                    self.manual_pid = None
                    self.manual_next = None
        self.start_op("MANUAL", run)

    def stop_single(self):
        with self.lock:
            self.manual_next = None
            self.manual_stop.set()

    def run_single(self, pid, direction):
        op = "prime" if direction == "FWD" else "purge"
        what = "Priming" if direction == "FWD" else "Purging"
        pc = self.pump_cfg(pid) or {"liquid": ""}
        base = {"type": "bulkProgress", "op": op, "step": 1, "steps": 1, "pumpId": pid,
                "liquid": pc["liquid"], "durationMs": MANUAL_RUN_MS}
        self.broadcast({"type": "bulkProgress", "op": op, "step": 0, "steps": 1,
                        "estimatedTotalMs": MANUAL_RUN_MS + 300, "status": "STARTING"})
        if self.level_state(pid) == "EMPTY":
            self.log("WARN", "PUMP", "Pump %d is EMPTY" % pid)
        self.log("INFO", "PUMP", "%s pump %d (%s) until stopped (at most %d s)"
                 % (what, pid, pc["liquid"], MANUAL_RUN_MS // 1000))
        self.broadcast(dict(base, status="RUNNING"))
        try:
            self.run_motion(pid, direction, MANUAL_RUN_MS, "PRIMING" if direction == "FWD" else "PURGING", 4,
                            stop=self.manual_stop)
            self.broadcast(dict(base, status="DONE"))
        except Stopped:
            self.pump_cmd(pid, "stop")
            self.log("INFO", "PUMP", "Pump %d stopped" % pid)
            self.broadcast(dict(base, status="DONE", reason="stopped"))
        except (Abort, RuntimeError) as ex:
            if isinstance(ex, RuntimeError) and not any(
                    e["code"] == "E_DISPENSE_TIMEOUT" and e["pumpId"] == pid for e in self.errors):
                self.raise_error("E_PUMP_COMM", pid, str(ex).split(":", 1)[-1])
            reason = "E-Stop" if isinstance(ex, Abort) else str(ex).split(":", 1)[-1]
            self.broadcast(dict(base, status="ABORTED", reason=reason))
            with self.lock:
                self.manual_next = None
            raise
        self.broadcast({"type": "bulkProgress", "op": op, "step": 1, "steps": 1, "status": "COMPLETE"})

    def do_bulk(self, op):
        mult = 1.0 if op == "primeAll" else 1.5
        qual = sorted([p for p in self.config["pumps"] if p["autoPrimeSec"] > 0], key=lambda p: p["pumpId"])
        runnable = [p for p in qual if self.present(p["pumpId"])]
        if not runnable:
            raise ReqError("PRECHECK_FAILED", "No pumps qualify for %s" % op)
        durs = [int(round(mult * p["autoPrimeSec"] * 1000)) for p in runnable]
        est = sum(durs) + (len(runnable) - 1) * BULK_GAP_MS + len(runnable) * 300
        n = len(qual)

        def run():
            self.broadcast({"type": "bulkProgress", "op": op, "step": 0, "steps": n,
                            "estimatedTotalMs": est, "status": "STARTING"})
            self.log("INFO", "PUMP", "%s: %d pumps, about %.1f s" % (op, n, est / 1000.0))
            i, p = 0, None
            try:
                first = True
                for i, p in enumerate(qual, 1):
                    pid = p["pumpId"]
                    base = {"type": "bulkProgress", "op": op, "step": i, "steps": n, "pumpId": pid,
                            "liquid": p["liquid"]}
                    if not self.present(pid):
                        self.log("WARN", "PUMP", "%s: skipping pump %d (missing)" % (op, pid))
                        self.broadcast(dict(base, status="SKIPPED"))
                        continue
                    if not first:
                        self.sleep(BULK_GAP_MS)
                    first = False
                    ms = int(round(mult * p["autoPrimeSec"] * 1000))
                    if self.level_state(pid) == "EMPTY":
                        self.log("WARN", "PUMP", "Pump %d is EMPTY" % pid)
                    self.broadcast(dict(base, status="RUNNING", durationMs=ms))
                    self.log("INFO", "PUMP", "%s: pump %d (%s) %.1f s" % (op, pid, p["liquid"], ms / 1000.0))
                    try:
                        self.run_motion(pid, "FWD" if op == "primeAll" else "REV", ms,
                                        "PRIMING" if op == "primeAll" else "PURGING", 4)
                    except RuntimeError as ex:
                        if not any(e["code"] == "E_DISPENSE_TIMEOUT" and e["pumpId"] == pid for e in self.errors):
                            self.raise_error("E_PUMP_COMM", pid, str(ex).split(":", 1)[-1])
                        raise
                    self.broadcast(dict(base, status="DONE", durationMs=ms))
                self.broadcast({"type": "bulkProgress", "op": op, "step": n, "steps": n, "status": "COMPLETE"})
                self.log("INFO", "PUMP", "%s complete" % op)
            except (Abort, RuntimeError) as ex:
                reason = "E-Stop" if isinstance(ex, Abort) else str(ex).split(":", 1)[-1]
                self.broadcast({"type": "bulkProgress", "op": op, "step": i, "steps": n,
                                "pumpId": p["pumpId"] if p else None, "status": "ABORTED", "reason": reason})
                self.log("WARN", "PUMP", "%s aborted: %s" % (op, reason))
        self.start_op("MANUAL", run)

    def estop(self, source):
        self.abort.set()
        with self.lock:
            for p in self.pumps.values():
                p["motion"] = None
                p["activity"] = "IDLE"
            for pid in self.pumps:
                self.set_idle_led(pid)
            self.pumps_dirty = True
        self.log("WARN", "SYS", "E-STOP activated (source=%s)" % source)

    def save_config(self, cfg):
        if self.system in ("DISPENSING", "MANUAL"):
            raise ReqError("BUSY", "Cannot save config while %s" % self.system)
        cfg = trim_strings(cfg)
        errs = validate_config(cfg)
        if errs:
            self.log("WARN", "CFG", "Config rejected: %d errors" % len(errs))
            raise ReqError("CONFIG_INVALID", "%d validation error(s)" % len(errs), errs)
        with self.lock:
            pumps_changed = cfg["pumps"] != self.config["pumps"]
            self.config = cfg
            if self.active_recipe and not any(r["id"] == self.active_recipe for r in cfg["recipes"]):
                self.active_recipe = ""
            if pumps_changed:
                self.log("INFO", "PUMP", "Pump config changed: rescanning")
                self.rebuild_pumps()
            self.recompute_errors()
            self.state_dirty = True
        self.log("INFO", "CFG", "Config saved")
        self.broadcast({"type": "config", "config": self.config})

    def reboot(self):
        def later():
            time.sleep(0.5)
            for c in list(self.clients):
                c.close()
            self.clients = []
            print("--- simulated reboot ---")
            with self.lock:
                self.boot = time.time()
                self.logs = []
                self.errors = []
                self.system = "READY"
                self.active_recipe = self.config["defaults"]["recipeId"]
                self.active_volume = self.config["defaults"]["volumeMl"]
                self.log("INFO", "SYS", "DrinkBotController %s (mock) booted after reboot" % FW_VERSION)
                self.recompute_errors()
        threading.Thread(target=later, daemon=True).start()

    # ----- request dispatch
    def handle(self, c, msg):
        t = msg.get("type")
        if t != "getConfig":
            self.log("DEBUG", "WEB", "%s from %s" % (t, c.ip))
        busy = self.system in ("DISPENSING", "MANUAL")

        def need_idle():
            if busy:
                raise ReqError("BUSY", "System is %s" % self.system)

        def pump_id():
            pid = msg.get("pumpId")
            if pid not in self.pumps:
                raise ReqError("NOT_FOUND", "No pump %s" % pid)
            return pid

        if t == "getConfig":
            c.send({"type": "config", "config": self.config})
        elif t == "saveConfig":
            self.save_config(msg.get("config"))
        elif t == "setRecipe":
            rid = msg.get("recipeId", "")
            if rid and not any(r["id"] == rid for r in self.config["recipes"]):
                raise ReqError("NOT_FOUND", "Unknown recipe '%s'" % rid)
            self.active_recipe = rid
            self.state_dirty = True
        elif t == "setVolume":
            v = msg.get("volumeMl")
            d = self.config["dispense"]
            if not _num(v) or not d["minVolumeMl"] <= v <= d["maxVolumeMl"]:
                raise ReqError("INVALID", "Volume must be %g-%g ml" % (d["minVolumeMl"], d["maxVolumeMl"]))
            self.active_volume = v
            self.state_dirty = True
        elif t == "dispense":
            need_idle()
            if self.blocking():
                raise ReqError("PRECHECK_FAILED", "Blocked by active errors; clear errors first")
            self.do_dispense(msg.get("recipeId") or self.active_recipe, msg.get("volumeMl") or self.active_volume)
        elif t in ("prime", "purge"):
            if self.manual_pid is None:  # a manual run may replace another manual run
                need_idle()
            if self.blocking():
                raise ReqError("PRECHECK_FAILED", "Blocked by active errors")
            self.do_single(pump_id(), "FWD" if t == "prime" else "REV")
        elif t == "pumpStop":
            pid = pump_id()
            if self.manual_pid is not None:
                self.stop_single()
            elif busy:
                raise ReqError("BUSY", "Use E-Stop to stop a dispense or Prime/Purge All")
            else:
                self.pump_cmd(pid, "stop")
        elif t in ("primeAll", "purgeAll"):
            need_idle()
            if self.blocking():
                raise ReqError("PRECHECK_FAILED", "Blocked by active errors")
            self.do_bulk(t)
        elif t == "estop":
            self.estop("web")
        elif t == "clearErrors":
            with self.lock:
                self.errors = [e for e in self.errors if not e["latched"]]
                self.recompute_errors()
            self.log("INFO", "SYS", "Latched errors cleared")
        elif t == "rescan":
            need_idle()
            with self.lock:
                self.rebuild_pumps()
                self.recompute_errors()
            self.log("INFO", "PUMP", "Rescan: %d pumps present" % sum(1 for p in self.pumps if self.present(p)))
        elif t == "reboot":
            if busy and not msg.get("force"):
                raise ReqError("BUSY", "System is %s; use force to E-Stop and reboot" % self.system)
            if busy:
                self.estop("web")
            self.log("WARN", "SYS", "Reboot requested from %s" % c.ip)
            self.reboot()
        elif t == "setLogLevel":
            lv = msg.get("level")
            if lv not in LEVELS:
                raise ReqError("INVALID", "Unknown level")
            self.web_level = lv
            self.log("INFO", "WEB", "Web log level set to %s" % lv)
        elif t == "pumpTare":
            need_idle()
            pid = pump_id()
            self.pump_cmd(pid, "tare")
            self.pumps[pid]["weightG"] = 0.0
            self.pumps_dirty = True
            self.log("INFO", "PUMP", "Pump %d tared" % pid)
        elif t == "pumpCalibrate":
            need_idle()
            pid = pump_id()
            g = msg.get("grams")
            if not _num(g) or g <= 0:
                raise ReqError("INVALID", "grams must be > 0")
            self.pump_cmd(pid, "cal %g" % g)
            self.pumps[pid]["weightG"] = float(g)
            self.pumps_dirty = True
            self.log("INFO", "PUMP", "Pump %d calibrated with %g g" % (pid, g))
        elif t == "pumpSetId":
            need_idle()
            pid = pump_id()
            nid = msg.get("newId")
            if not _int(nid) or not 1 <= nid <= 254:
                raise ReqError("INVALID", "newId must be 1-254")
            if nid in self.pumps:
                raise ReqError("INVALID", "ID %d already in use" % nid)
            self.pump_cmd(pid, "sid %d" % nid)
            with self.lock:
                self.pumps[nid] = self.pumps.pop(pid)
                self.pumps[nid]["pumpId"] = nid
                self.faults[nid] = self.faults.pop(pid)
                pc = self.pump_cfg(pid)
                if pc:
                    pc["pumpId"] = nid
                self.rebuild_pumps()
                self.recompute_errors()
            self.log("INFO", "PUMP", "Pump %d ID changed to %d" % (pid, nid))
            self.broadcast({"type": "config", "config": self.config})
        elif t == "pumpLedTest":
            need_idle()
            pid = pump_id()
            self.pump_cmd(pid, "ledcolor 255 0 0")

            def ledtest():
                for col in ((255, 0, 0), (0, 255, 0), (0, 0, 255)):
                    self.pumps[pid]["led"] = {"r": col[0], "g": col[1], "b": col[2], "mode": 1, "brightness": 255}
                    self.pumps_dirty = True
                    time.sleep(1)
                self.set_idle_led(pid)
                self.pumps_dirty = True
            threading.Thread(target=ledtest, daemon=True).start()
        elif t == "pumpRaw":
            need_idle()
            pid = pump_id()
            cmd = str(msg.get("command", "")).strip()
            if not cmd or len(cmd) > 63:
                raise ReqError("INVALID", "command must be 1-63 chars")
            if cmd.split()[0] in ("dispense", "prime", "purge", "bridge"):
                # One pump at a time (D-7): raw motion would bypass that rule
                raise ReqError("INVALID", "Use Prime / Purge or a recipe to run a pump")
            self.pump_cmd(pid, cmd)
            word = cmd.split()[0]
            p = self.pumps[pid]
            lines = {
                "version": ["P,VER,0.6.0-sim", "R,OK"],
                "gid": ["P,ID,%d" % pid, "R,OK"],
                "weight": ["P,WEIGHT,%.6f" % p["weightG"], "R,OK"],
                "status": ["P,STATUS,%s,%d" % (p["motion"]["dir"] if p["motion"] else "IDLE",
                                                max(0, p["motion"]["remaining"]) if p["motion"] else 0), "R,OK"],
            }.get(word, ["R,OK"] if word in ("ledcolor", "ledmode", "ledfade", "ledchase", "ledtime", "stop") else ["R,INVLD"])
            self.log("INFO", "PUMP", "Raw '%s' to pump %d -> %s" % (cmd, pid, lines[-1]))
            return {"lines": lines}
        elif t == "simFault":
            pid = pump_id()
            f = msg.get("fault")
            if f == "missing":
                self.faults[pid]["missing"] = bool(msg.get("value", True))
                with self.lock:
                    self.recompute_errors()
                    self.pumps_dirty = True
            elif f in ("timeout", "overrun"):
                self.faults[pid][f] = True
            elif f == "weight":
                w = msg.get("weightG")
                if not _num(w) or w < 0:
                    raise ReqError("INVALID", "weightG must be >= 0")
                self.pumps[pid]["weightG"] = float(w)
                self.pumps_dirty = True
            else:
                raise ReqError("INVALID", "Unknown fault")
            self.log("INFO", "SIM", "Fault injected on pump %d: %s %s" % (pid, f, msg.get("value", msg.get("weightG", ""))))
        else:
            raise ReqError("INVALID", "Unknown message type '%s'" % t)
        return None


class ReqError(Exception):
    def __init__(self, code, message, details=None):
        Exception.__init__(self, message)
        self.code, self.message, self.details = code, message, details


SIM = None


# --------------------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def send_json(self, code, obj, extra=None):
        body = json.dumps(obj, indent=2 if code == 200 and extra else None).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/ws":
            return self.websocket()
        if path == "/api/config":
            return self.send_json(200, SIM.config, {
                "Content-Disposition": "attachment; filename=drinkbot-config.json"})
        if path == "/api/info":
            return self.send_json(200, {"fwVersion": FW_VERSION, "buildDate": BUILD_DATE,
                                        "uptimeMs": SIM.uptime(), "freeHeap": 182000 + random.randint(0, 4000),
                                        "simulator": True, "ip": "127.0.0.1", "wifiMode": "STA"})
        if path == "/":
            path = "/index.html"
        fn = os.path.normpath(os.path.join(WEB_DIR, path.lstrip("/")))
        if not fn.startswith(WEB_DIR) or not os.path.isfile(fn):
            return self.send_json(404, {"ok": False, "error": "not found"})
        ctype = {".html": "text/html; charset=utf-8", ".js": "application/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml"}.get(
            os.path.splitext(fn)[1], "application/octet-stream")
        with open(fn, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        path = self.path.split("?")[0]
        n = int(self.headers.get("Content-Length") or 0)
        if path == "/api/config":
            body = self.rfile.read(n)
            if n > 32768:
                return self.send_json(413, {"ok": False, "errors": [{"path": "", "message": "larger than 32 KB"}]})
            try:
                cfg = json.loads(body.decode("utf-8"))
            except ValueError as ex:
                return self.send_json(400, {"ok": False, "errors": [{"path": "", "message": "bad JSON: %s" % ex}]})
            try:
                SIM.save_config(cfg)
            except ReqError as ex:
                if ex.code == "BUSY":
                    return self.send_json(409, {"ok": False, "errors": [{"path": "", "message": ex.message}]})
                return self.send_json(400, {"ok": False, "errors": ex.details or []})
            return self.send_json(200, {"ok": True})
        if path == "/api/ota":
            return self.ota(n)
        self.send_json(404, {"ok": False})

    def ota(self, n):
        if SIM.system not in ("READY", "ERROR"):
            self.rfile.read(n)
            return self.send_json(409, {"ok": False, "error": "busy"})
        got, first, last_pct = 0, b"", -1
        SIM.log("INFO", "OTA", "OTA upload started (%d bytes)" % n)
        while got < n:
            chunk = self.rfile.read(min(8192, n - got))
            if not chunk:
                break
            if not first:
                # skip the multipart header to find the image's first byte
                idx = chunk.find(b"\r\n\r\n")
                first = chunk[idx + 4:idx + 5] if idx >= 0 else chunk[:1]
            got += len(chunk)
            pct = int(got * 100 / max(n, 1))
            if pct != last_pct and pct % 5 == 0:
                last_pct = pct
                SIM.broadcast({"type": "ota", "percent": pct, "status": "WRITING"})
                time.sleep(0.05)
        if first != b"\xe9":
            SIM.broadcast({"type": "ota", "percent": last_pct, "status": "FAILED",
                           "message": "Invalid image header (not an ESP32 app image)"})
            SIM.log("ERROR", "OTA", "OTA failed: invalid image header")
            return self.send_json(400, {"ok": False, "error": "invalid image header"})
        SIM.broadcast({"type": "ota", "percent": 100, "status": "DONE", "message": "Rebooting"})
        SIM.log("INFO", "OTA", "OTA complete, rebooting")
        self.send_json(200, {"ok": True})
        SIM.reboot()

    def websocket(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key or "websocket" not in (self.headers.get("Upgrade") or "").lower():
            return self.send_json(400, {"ok": False, "error": "websocket upgrade expected"})
        if len(SIM.clients) >= 4:
            return self.send_json(503, {"ok": False, "error": "too many clients"})
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        self.wfile.flush()
        self.close_connection = True
        c = WsClient(self)
        SIM.on_connect(c)
        try:
            while c.alive:
                text = c.read_message()
                if text is None:
                    break
                if not text:
                    continue
                try:
                    msg = json.loads(text)
                except ValueError:
                    c.send({"type": "error", "code": "INVALID", "message": "bad JSON"})
                    continue
                rid = msg.get("reqId")
                try:
                    data = SIM.handle(c, msg)
                    if rid is not None:
                        ack = {"type": "ack", "reqId": rid}
                        if data is not None:
                            ack["data"] = data
                        c.send(ack)
                except ReqError as ex:
                    SIM.log("WARN", "WEB", "%s rejected: %s %s" % (msg.get("type"), ex.code, ex.message))
                    err = {"type": "error", "code": ex.code, "message": ex.message}
                    if rid is not None:
                        err["reqId"] = rid
                    if ex.details:
                        err["details"] = ex.details
                    c.send(err)
                except RuntimeError as ex:
                    code, _, m = str(ex).partition(":")
                    err = {"type": "error", "code": code if code in ("PUMP_TIMEOUT", "PUMP_ERROR") else "INTERNAL",
                           "message": m or str(ex)}
                    if rid is not None:
                        err["reqId"] = rid
                    c.send(err)
        except (OSError, ValueError, struct.error):
            pass
        finally:
            c.alive = False
            if c in SIM.clients:
                SIM.clients.remove(c)
                SIM.log("INFO", "WEB", "WebSocket client %s disconnected" % c.ip)


def main():
    global SIM, SPEED, WEB_DIR
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--speed", type=float, default=1.0, help="simulation speed multiplier")
    ap.add_argument("--web", default=os.path.join(here, "..", "web"))
    a = ap.parse_args()
    SPEED = max(0.1, a.speed)
    WEB_DIR = os.path.abspath(a.web)
    SIM = Sim()
    threading.Thread(target=SIM.tick_loop, daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    srv.daemon_threads = True
    print("DrinkBot mock server on http://%s:%d/ (web: %s, speed x%g)" % (a.host, a.port, WEB_DIR, SPEED))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
