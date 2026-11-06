#!/usr/bin/env python3
"""Guided power-cut tests for pump firmware updates (see TEST_PLAN.md §7).

Pump A stays on USB as the bridge; pump B is the one whose power you cut.
"Cut power" always means B's USB AND its motor supply, so the RP2040 goes dark.

  fw_interrupt_test.py --a PORT_A --b PORT_B --id 2 --old OLD.bin --new NEW.bin --big BIG.bin

Run this with B on the version of OLD.bin. NEW.bin must be a different version
and BIG.bin a build that differs in most flash pages (e.g. another optimization
level), so its install lasts long enough to interrupt. B ends on NEW.bin.
"""

import argparse
import datetime
import glob
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from pump_update import Link, UpdateError, load_image, read_fwstat, read_version, send_image  # noqa: E402

BOLD, GREEN, RED, RESET = "\033[1m", "\033[32m", "\033[31m", "\033[0m"
results = []


def check(ok, name, detail=""):
    results.append((name, ok, detail))
    print(f"   {GREEN + 'PASS' if ok else RED + 'FAIL'}{RESET}  {name}  {detail}")
    return ok


def prompt(text):
    if input(f"   {BOLD}> {text}  [Enter when done, q to quit] {RESET}").strip().lower() == "q":
        raise KeyboardInterrupt


def usb_ports():
    return set(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))


class Rig:
    def __init__(self, args):
        self.port_a = args.a
        self.port_b = args.b
        self.id = args.id

    def b_link(self):
        return Link(self.port_b)

    def power_cycle_b(self):
        """Asks for a power cut and waits until B is back on USB."""
        prompt("Cut B's power (USB and motor supply)")
        self.wait_b_back()

    def wait_b_back(self):
        deadline = time.monotonic() + 30
        while self.port_b in usb_ports() and time.monotonic() < deadline:
            time.sleep(0.1)
        print(f"   {BOLD}> Wait 3 s, then restore B's motor supply and USB.{RESET}")
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            others = usb_ports() - {self.port_a}
            if others:
                self.port_b = sorted(others)[0]
                time.sleep(6)  # boot, including an install if one is pending
                return
            time.sleep(0.2)
        raise UpdateError("B didn't come back on USB within 2 minutes")

    def b_state(self):
        link = self.b_link()
        try:
            return read_version(link), read_fwstat(link)[0]
        finally:
            link.close()

    def stage(self, image):
        """Sends an image to B over USB and stages it (fast)."""
        link = self.b_link()
        try:
            if not send_image(link, image):
                raise UpdateError("image wasn't staged")
        finally:
            link.close()

    def bridge_ok(self):
        link = Link(self.port_a, self.id)
        try:
            return read_version(link)
        finally:
            link.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("a", "b", "old", "new", "big"):
        ap.add_argument(f"--{name}", required=True)
    ap.add_argument("--id", type=int, required=True)
    args = ap.parse_args()
    rig = Rig(args)
    old_img, old_ver = load_image(args.old)
    new_img, new_ver = load_image(args.new)
    big_img, big_ver = load_image(args.big)
    print(f"{BOLD}Firmware update power-cut tests on pump {args.id}{RESET}")

    try:
        start, _ = rig.b_state()
        if not check(start == old_ver, f"B starts on {old_ver}", f"reports {start}"):
            return 1

        print(f"\n{BOLD}1. Power cut during a bus transfer{RESET}")
        link = Link(rig.port_a, rig.id)
        try:
            send_image(link, new_img, stop_after=4800)
        finally:
            link.close()
        print("   4.8 KB sent through the bus and left unfinished.")
        rig.power_cycle_b()
        ver, state = rig.b_state()
        check(ver == old_ver and state == "IDLE", "B runs its old firmware, no update pending", f"{ver} {state}")
        check(rig.bridge_ok() == old_ver, "A can still reach B over the bus")

        print(f"\n{BOLD}2. Staged, cancelled, then a power cut{RESET}")
        rig.stage(new_img)
        link = rig.b_link()
        link.request("fwabort")
        link.close()
        print("   Image staged, then cancelled with fwabort.")
        rig.power_cycle_b()
        ver, _ = rig.b_state()
        check(ver == old_ver, "The cancelled image was not installed", ver)

        print(f"\n{BOLD}3. Staged, no fwapply, then a power cut{RESET}")
        rig.stage(new_img)
        print("   Image staged and left pending. It should install on the next boot.")
        rig.power_cycle_b()
        ver, state = rig.b_state()
        check(ver == new_ver and state == "IDLE", "The staged image installed on boot", f"{ver} {state}")

        print(f"\n{BOLD}4. Timing a large install (no power cut){RESET}")
        rig.stage(big_img)
        link = rig.b_link()
        t0 = time.monotonic()
        try:
            link.ser.write(b"fwapply\n")
            link.ser.flush()
        finally:
            link.close()
        time.sleep(0.5)
        while rig.port_b not in usb_ports() and time.monotonic() - t0 < 60:
            time.sleep(0.05)
        time.sleep(0.5)
        while time.monotonic() - t0 < 60:
            try:
                ver, state = rig.b_state()
                break
            except Exception:
                time.sleep(0.2)
        install_s = time.monotonic() - t0
        check(ver == big_ver, f"Installed {big_ver}", f"back after {install_s:.1f} s")

        print(f"\n{BOLD}5. Power cut during an install{RESET}")
        print(f"   The last install took about {install_s:.0f} s. The window is short, so get ready:")
        print("   hold B's USB plug, and have its motor supply within reach.")
        rig.stage(new_img)
        prompt("Ready to pull B's USB and motor supply the moment you see GO")
        link = rig.b_link()
        link.ser.write(b"fwapply\n")
        link.ser.flush()
        link.close()
        print(f"\n   {BOLD}{RED}>>> GO: PULL B's POWER NOW <<<{RESET}\n")
        # The port also disappears as the pump reboots, so only you can tell
        # when the power was really cut and restored.
        prompt("Wait 3 s, restore B's motor supply and USB, watch for its LEDs")
        deadline = time.monotonic() + 120
        while not (usb_ports() - {rig.port_a}) and time.monotonic() < deadline:
            time.sleep(0.2)
        others = usb_ports() - {rig.port_a}
        if not others:
            raise UpdateError("B didn't come back on USB within 2 minutes")
        rig.port_b = sorted(others)[0]
        time.sleep(10)  # the restarted install, then a normal boot
        ver, state = rig.b_state()
        check(ver == new_ver and state == "IDLE", "B finished the install after power returned", f"{ver} {state}")
        check(rig.bridge_ok() == new_ver, "A can reach B over the bus")
        link = rig.b_link()
        lines, _ = link.request("gid")
        link.close()
        check(f"P,ID,{args.id}" in lines, f"B kept ID {args.id}", " ".join(lines))
    except KeyboardInterrupt:
        print("\nStopped.")
    except Exception as e:  # keep the results so far, and say how to recover
        check(False, "Test stopped by an error", str(e))
        print("If B doesn't respond, hold BOOTSEL while plugging in its USB and flash it over USB.")

    failed = [r for r in results if not r[1]]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reports",
                       datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + f"_fw_interrupt_id{args.id}.md")
    with open(out, "w") as f:
        f.write(f"# Firmware update power-cut test, pump {args.id}\n\n| Check | Result | Detail |\n|---|---|---|\n")
        for name, ok, detail in results:
            f.write(f"| {name} | {'PASS' if ok else 'FAIL'} | {detail} |\n")
    print(f"\n{BOLD}{len(results) - len(failed)} pass, {len(failed)} fail{RESET}\nReport: {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
