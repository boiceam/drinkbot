#!/usr/bin/env python3
"""Controller-to-bridge interface tests on the real controller board.

Setup: the dedicated bridge board (pump firmware 0.7.1 or newer, ID 0) on the
controller's USB host port, with no pumps on its bus and no motor, scale or LEDs.
The controller must be reachable over the network.

  bridge_test.py --host 192.168.86.250 [--pump-image DrinkBotPump.ino.bin]

Saves a test config with one (absent) pump to exercise bridged requests, and
restores the original config at the end. The original is held in memory only
and never printed, because it contains the Wi-Fi password. Python standard
library only; shares its WebSocket and HTTP helpers with board_test.py.
"""

import argparse
import datetime
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_test as bt  # noqa: E402
from board_test import BOLD, RED, RESET, check, section  # noqa: E402

BRIDGE_ERRORS = ("E_BRIDGE_UNRESPONSIVE", "E_USB_LOST", "E_STOP_UNCONFIRMED")


def raw(ws, command, pump_id=0, timeout=10):
    """pumpRaw -> (ok, reply lines, error dict)."""
    ok, r = ws.request("pumpRaw", pumpId=pump_id, command=command, timeout=timeout)
    lines = (r.get("data") or {}).get("lines") or r.get("lines") or []
    return ok, lines, r


def error_codes(ws):
    s = ws.latest("state")
    return [e["code"] for e in (s or {}).get("errors", [])]


def logs_since(ws, mark):
    out = []
    for _, m in ws.of_type("log", mark):
        out.append(m)
    return out


def history(ws):
    m = ws.latest("logHistory")
    return (m or {}).get("lines", [])


def wait_bridge(ws, timeout=15):
    """Waits for a pumpFw message reporting the bridge board."""
    return ws.wait(lambda m: m.get("type") == "pumpFw" and m.get("bridgeId") == 0 and m.get("bridgeVersion"),
                   timeout)


def run(board, ws, original, pump_image):
    heap0 = board.info()["freeHeap"]

    section("1 Detection at boot")
    info = board.info()
    check(info["simulator"] is False, "Controller found a pump USB device (not simulator mode)")
    fw = wait_bridge(ws)
    check(fw is not None, "Bridge reported as ID 0", fw and fw.get("bridgeId"))
    check(fw and fw.get("bridgeVersion") == "0.7.1", "Bridge firmware read as 0.7.1", fw and fw.get("bridgeVersion"))
    msgs = [e.get("msg", "") for e in history(ws)]
    check(any("Dedicated bridge board, firmware 0.7.1" in m for m in msgs), "Boot log names the bridge board")
    check(not any(c in error_codes(ws) for c in BRIDGE_ERRORS), "No bridge or USB errors", error_codes(ws))
    usb_warn = [m for m in msgs if "CDC error" in m or "TX failed" in m]
    check(not usb_warn, "No USB transfer errors in the boot log", usb_warn[:3])

    section("2 Direct commands to the bridge")
    ok, lines, _ = raw(ws, "version")
    check(ok and "P,VER,0.7.1" in lines and lines[-1] == "R,OK", "version", lines)
    ok, lines, _ = raw(ws, "gid")
    check(ok and "P,ID,0" in lines, "gid reports 0", lines)
    ok, lines, _ = raw(ws, "status")
    check(ok and any(l.startswith("P,STATUS,IDLE") for l in lines), "status is IDLE", lines)
    ok, lines, _ = raw(ws, "fwstat")
    check(ok and any(l.startswith("P,FW,IDLE") for l in lines), "fwstat is IDLE", lines)
    ok, lines, _ = raw(ws, "ledcolor 0 0 255")
    check(ok and lines and lines[-1] == "R,OK", "ledcolor accepted (no LEDs fitted)", lines)
    t = time.monotonic()
    ok, lines, _ = raw(ws, "weight", timeout=15)
    check(lines != [], "weight with no scale answers instead of hanging",
          f"{lines} in {(time.monotonic() - t) * 1000:.0f} ms")
    # The controller turns a pump's R,INVLD / R,ERR into a PUMP_ERROR reply
    for bad in ("nosuchcommand", "ledcolor 1"):
        ok, _, r = raw(ws, bad)
        check(not ok and r.get("code") == "PUMP_ERROR" and "INVLD" in r.get("message", ""),
              f"`{bad}` rejected by the bridge", r.get("message"))
    ok, _, r = raw(ws, "dispense 1000")
    check(not ok and r.get("code") == "INVALID", "Raw motion refused by the controller", r.get("message"))
    ok, _, r = raw(ws, "bridge 3 status")
    check(not ok, "Raw `bridge` refused by the controller", r.get("message"))
    ok, r = ws.request("pumpSetId", pumpId=0, newId=5)
    check(not ok, "Changing the bridge board's ID refused", r.get("message"))
    ok, lines, _ = raw(ws, "gid")
    check("P,ID,0" in lines, "Bridge still ID 0", lines)

    section("3 Request soak (300 x status)")
    lat, fails = [], []
    for i in range(300):
        t = time.monotonic()
        ok, lines, r = raw(ws, "status")
        lat.append((time.monotonic() - t) * 1000)
        if not (ok and any(l.startswith("P,STATUS,IDLE") for l in lines)):
            fails.append((i, lines or r.get("message")))
    check(not fails, "300 round trips with no failures", f"{len(fails)} failed {fails[:3]}")
    lat.sort()
    check(True, "Round-trip latency over Wi-Fi + controller + USB",
          f"median {statistics.median(lat):.0f} ms, p95 {lat[int(len(lat) * 0.95)]:.0f} ms, max {lat[-1]:.0f} ms")
    check(not any(c in error_codes(ws) for c in BRIDGE_ERRORS), "No bridge errors after the soak", error_codes(ws))

    section("4 Rescan")
    good = 0
    for i in range(5):
        m0 = ws.mark()
        ws.request("rescan", timeout=10)
        done = ws.wait_since(m0, lambda m: m.get("type") == "log" and "Discovery done" in m.get("msg", ""), 10)
        found = [x for x in logs_since(ws, m0) if "Dedicated bridge board" in x.get("msg", "")]
        good += 1 if (done and found) else 0
    check(good == 5, "5 rescans each find the bridge board", f"{good}/5")
    check(not any(c in error_codes(ws) for c in BRIDGE_ERRORS), "No bridge errors", error_codes(ws))

    section("5 E-Stop")
    m0 = ws.mark()
    t = time.monotonic()
    ok, _ = ws.request("estop")
    check(ok, "E-Stop accepted", f"{(time.monotonic() - t) * 1000:.0f} ms")
    time.sleep(2)
    check("E_STOP_UNCONFIRMED" not in error_codes(ws), "Bridge confirmed the stop", error_codes(ws))
    ws.request("clearErrors")
    time.sleep(0.5)
    ok, lines, _ = raw(ws, "status")
    check(ok and any(l.startswith("P,STATUS,IDLE") for l in lines), "Link still works after E-Stop", lines)

    section("6 Bridged requests to an absent pump")
    cfg = json.loads(json.dumps(original))
    cfg["pumps"] = [{"pumpId": 1, "liquid": "Test", "msPerMl": 100, "densityGPerMl": 1.0,
                     "color": [255, 160, 0], "autoPrimeSec": 0}]
    cfg["recipes"] = []
    m0 = ws.mark()
    t = time.monotonic()
    ok, r = ws.request("saveConfig", config=cfg, timeout=15)
    check(ok, "Config with pump 1 saved", r.get("message", ""))
    miss = ws.wait_since(m0, lambda m: m.get("type") == "state" and any(
        e["code"] == "E_PUMP_MISSING" for e in m.get("errors", [])), 20)
    check(miss is not None, "Pump 1 reported missing (bridge relayed the timeout)",
          f"after {(time.monotonic() - t):.1f} s")
    p1 = ws.pump(1)
    check(p1 is not None and not p1.get("present"), "Pump 1 shown as not present")
    check(not any(c in error_codes(ws) for c in ("E_BRIDGE_UNRESPONSIVE", "E_USB_LOST")),
          "Bridge not blamed for the missing pump", error_codes(ws))
    t = time.monotonic()
    ok, lines, r = raw(ws, "status", pump_id=1, timeout=15)
    dt = time.monotonic() - t
    check(not ok and 2.5 < dt < 12, "Bridged request to pump 1 fails with a timeout",
          f"{lines or r.get('message')} in {dt:.1f} s")
    ok, lines, _ = raw(ws, "status")
    check(ok and any(l.startswith("P,STATUS,IDLE") for l in lines), "Bridge itself still answers", lines)
    m0 = ws.mark()
    ws.request("estop")
    time.sleep(6)
    unconf = [e for e in (ws.latest("state") or {}).get("errors", []) if e["code"] == "E_STOP_UNCONFIRMED"]
    check(not unconf, "E-Stop with a missing pump: bridge confirms, no stop is expected from pump 1", unconf)
    ws.request("clearErrors")
    status, _ = board.post("/api/config", json.dumps(original).encode(), "application/json")
    check(status == 200, "Original config back")
    time.sleep(3)

    if pump_image:
        section("7 Pump firmware plan with the bridge current")
        data = open(pump_image, "rb").read()
        status, body = board.upload("/api/pump-firmware", data, "DrinkBotPump.ino.bin")
        check(status == 200, "0.7.1 image stored", status)
        fw = ws.wait(lambda m: m.get("type") == "pumpFw" and m.get("image"), 10)
        check(fw and fw["targets"] == [] and fw["bridgeVersion"] == "0.7.1",
              "Nothing to update: the bridge already runs 0.7.1", fw and fw["targets"])
        ok, r = ws.request("pumpFwUpdate", timeout=10)
        check(not ok, "Update with nothing to do is refused", r.get("message"))
        ok, _ = ws.request("pumpFwDelete")
        check(ok, "Stored image deleted")

    section("8 Controller reboot (bridge stays powered)")
    ws.request("reboot", force=True)
    ws.close()
    time.sleep(6)
    t = time.monotonic()
    ws = board.connect(60)
    fw = wait_bridge(ws, 20)
    check(board.info()["simulator"] is False and fw is not None,
          "Bridge found again after a controller reboot", f"web back {(time.monotonic() - t) + 6:.0f} s after reboot")
    msgs = [e.get("msg", "") for e in history(ws)]
    usb_warn = [m for m in msgs if "CDC error" in m or "TX failed" in m]
    check(not usb_warn, "No USB transfer errors after the reboot", usb_warn[:3])
    ok, lines, _ = raw(ws, "version")
    check(ok and "P,VER,0.7.1" in lines, "version after the reboot", lines)

    check(True, "Free heap", f"{heap0 // 1024} KB at start, {board.info()['freeHeap'] // 1024} KB at end")
    return ws


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--pump-image", help="optional DrinkBotPump .bin matching the bridge's version")
    args = ap.parse_args()
    board = bt.Board(args.host)
    print(f"{BOLD}Controller-bridge interface tests on {args.host}{RESET}")
    original = json.loads(board.get("/api/config")[1])
    ws = board.connect()
    try:
        ws = run(board, ws, original, args.pump_image)
    except Exception as e:  # keep the results, and always restore the config
        check(False, "Test run stopped by an error", repr(e))
    finally:
        bt.SECTION[0] = "Restore"
        try:
            now = json.loads(board.get("/api/config")[1])
            if now.get("pumps") != original.get("pumps") or now.get("wifi") != original.get("wifi"):
                status, _ = board.post("/api/config", json.dumps(original).encode(), "application/json")
                check(status == 200, "Original config restored", status)
            else:
                check(True, "Original config in place")
        except Exception as e:
            check(False, "Restoring the original config", repr(e))
        ws.close()

    results = bt.results
    failed = [r for r in results if not r[2]]
    print(f"\n{BOLD}{len(results) - len(failed)} pass, {len(failed)} fail{RESET}")
    for sec, name, _, detail in failed:
        print(f"  {RED}FAIL{RESET} [{sec}] {name}  {detail}")
    info = board.info()
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "reports")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "_bridge.md")
    with open(path, "w") as f:
        f.write(f"# Controller-bridge interface test\n\n- Controller build: {info['buildDate']}\n"
                "- Bridge board at ID 0 on the USB host port, no pumps on the bus, nothing else attached\n"
                f"- Result: {len(results) - len(failed)} pass, {len(failed)} fail\n\n"
                "| Section | Check | Result | Detail |\n|---|---|---|---|\n")
        for sec, name, ok, detail in results:
            f.write(f"| {sec} | {name} | {'PASS' if ok else 'FAIL'} | {detail.replace('|', '/')} |\n")
    print("Report:", os.path.normpath(path))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
