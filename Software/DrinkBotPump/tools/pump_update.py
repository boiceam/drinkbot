#!/usr/bin/env python3
"""Update DrinkBotPump firmware over USB or through the RS485 bus.

The pump on --port is talked to directly. With --id, the update goes through
that pump as a bridge to the pump with that ID.

  pump_update.py DrinkBotPump.ino.bin --port /dev/cu.usbmodem14101 --id 2
  pump_update.py DrinkBotPump.ino.bin --port /dev/cu.usbmodem14101

Update one pump at a time, with no pump running. Update the bridge last.
Stopping at any point before the install leaves the current firmware running.

The image must be the uncompressed .bin of a DrinkBotPump build (0.6.0 or newer)
made with the same Arduino core and flash layout as the pump's firmware:
  FQBN rp2040:rp2040:rpipico:dbgport=Disabled,flash=2097152_524288
The pump checks this itself. Anything else needs a USB flash.

Needs pyserial (pip install pyserial).
"""

import argparse
import base64
import glob
import hashlib
import re
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: python3 -m pip install pyserial")

CHUNK = 120  # a multiple of 3, so base64 has no padding; fits every pump buffer
RETRIES = 5
BRIDGE_REPLY_S = 4.5  # longer than the pump's 3 s bridge timeout
DIRECT_REPLY_S = 3.0
INSTALL_WAIT_S = 60
LOADER_BYTES = 0x3000  # the pump compares these against its own flash


class UpdateError(Exception):
    pass


def check_packet(line):
    """'$[payload|CRC]' -> payload, or None if malformed or the CRC is wrong."""
    m = re.fullmatch(r"\$\[(.*)\|([0-9A-Fa-f]{8})\]", line)
    if not m:
        return None
    crc = zlib.crc32(("$[" + m.group(1) + "|").encode()) & 0xFFFFFFFF
    return m.group(1) if crc == int(m.group(2), 16) else None


class Link:
    def __init__(self, port, target=None):
        self.port = port
        self.target = target
        self.ser = None
        self.open()

    def open(self):
        self.ser = serial.Serial(self.port, 115200, timeout=0.05)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None

    def _read_line(self, deadline):
        buf = b""
        while time.monotonic() < deadline:
            c = self.ser.read(1)
            if not c:
                continue
            if c == b"\n":
                return buf.decode(errors="replace").rstrip("\r")
            buf += c
        return None

    def send(self, command, reply_s=None):
        """One exchange. Returns (payload lines, final result), where the final
        result is 'R,...' or None if the exchange failed and can be retried."""
        wire = f"bridge {self.target} {command}" if self.target else command
        if reply_s is None:
            reply_s = BRIDGE_REPLY_S if self.target else DIRECT_REPLY_S
        self.ser.reset_input_buffer()
        self.ser.write((wire + "\n").encode())
        self.ser.flush()
        deadline = time.monotonic() + reply_s
        payloads = []
        while True:
            line = self._read_line(deadline)
            if line is None:
                return payloads, None
            if line.startswith("$["):
                payload = check_packet(line)
                if payload is None:
                    # Corrupted. If it was the final packet, the bridge is done
                    # with this request; otherwise it reports R,TIMEOUT shortly.
                    if line.startswith("$[R,"):
                        return payloads, None
                    continue
                if payload.startswith("R,"):
                    return payloads, payload
                payloads.append(payload)
            elif line.startswith("R,"):
                return payloads, None if line == "R,TIMEOUT" else line
            elif line.startswith("P,"):
                payloads.append(line)

    def request(self, command, reply_s=None):
        """Retries transport failures. Returns (payloads, 'R,OK'/'R,ERR'/...)."""
        for attempt in range(RETRIES):
            payloads, result = self.send(command, reply_s)
            if result is not None:
                return payloads, result
            time.sleep(0.2 * (attempt + 1))
        raise UpdateError(f"no reply to `{command.split()[0]}` after {RETRIES} attempts")


def read_version(link):
    payloads, result = link.request("version")
    for p in payloads:
        if p.startswith("P,VER,"):
            return p.split(",", 2)[2]
    raise UpdateError(f"no version in reply: {payloads} {result}")


def read_fwstat(link):
    payloads, _ = link.request("fwstat")
    for p in payloads:
        m = re.fullmatch(r"P,FW,(\w+),(\d+),(\d+)", p)
        if m:
            return m.group(1), int(m.group(2)), int(m.group(3))
    raise UpdateError(f"pump didn't answer fwstat (needs firmware 0.6.0 or newer): {payloads}")


def load_image(path):
    """Returns (image bytes, version from its DBP-IMAGE tag)."""
    data = open(path, "rb").read()
    if data[:2] == b"\x1f\x8b":
        raise UpdateError("compressed images aren't accepted; use the plain .bin")
    if data[:4] == b"UF2\n":
        raise UpdateError("that's a UF2 file; use the .bin from the same build")
    m = re.search(rb"DBP-IMAGE:([0-9A-Za-z.\-]+)", data)
    if not m:
        raise UpdateError("not DrinkBotPump firmware 0.6.0 or newer (no DBP-IMAGE tag)")
    return data, m.group(1).decode()


def send_image(link, image, stop_after=None):
    md5 = hashlib.md5(image).hexdigest()
    _, result = link.request(f"fwbegin {len(image)} {md5}", BRIDGE_REPLY_S + 2)
    if result != "R,OK":
        raise UpdateError(
            f"pump refused the update ({result}): it may be running, the image may not fit, "
            "or it was built without a file system (see README)")
    t0 = time.monotonic()
    retries = 0
    offset = 0
    while offset < len(image):
        chunk = image[offset:offset + CHUNK]
        text = base64.b64encode(chunk).decode()
        for attempt in range(RETRIES):
            _, result = link.send(f"fwdata {offset} {text}")
            if result == "R,OK":
                break
            retries += 1
            if result is not None:
                # The pump disagrees about the position: ask it where it is
                state, received, _ = read_fwstat(link)
                if state != "RECEIVING" and offset < LOADER_BYTES:
                    raise UpdateError(
                        "the pump refused this image: its first 12 KB don't match the pump's flash. "
                        "It was built with a different Arduino core or flash layout, so flash it over USB.")
                if state != "RECEIVING" or received not in (offset, offset + len(chunk)):
                    raise UpdateError(f"out of step at {offset}: pump is {state} at {received}")
            time.sleep(0.2 * (attempt + 1))
        else:
            raise UpdateError(f"chunk at {offset} failed {RETRIES} times; is the pump powered?")
        offset += len(chunk)
        rate = offset / max(time.monotonic() - t0, 0.001)
        print(f"\r  {offset}/{len(image)} bytes  {100 * offset // len(image)}%  "
              f"{rate:.0f} B/s  {(len(image) - offset) / rate:.0f} s left  retries {retries}   ",
              end="", flush=True)
        if stop_after is not None and offset >= stop_after:
            print()
            return False
    print(f"\n  Sent in {time.monotonic() - t0:.0f} s with {retries} retries")
    _, result = link.request("fwend", BRIDGE_REPLY_S + 2)
    if result != "R,OK":
        raise UpdateError("the pump rejected the image (MD5 or format check failed)")
    return True


def wait_for_install(link, port_is_target):
    """Sends fwapply, then waits until the pump is back on a fresh boot."""
    try:
        link.send("fwapply")
    except (serial.SerialException, OSError):
        pass  # direct: the port disappears as the pump reboots
    if port_is_target:
        link.close()
        time.sleep(1.0)
    time.sleep(1.0)  # the pump waits 100 ms before it reboots
    deadline = time.monotonic() + INSTALL_WAIT_S
    resent = False
    while time.monotonic() < deadline:
        try:
            if port_is_target and link.ser is None:
                ports = glob.glob(link.port)
                if not ports:
                    # macOS may give the rebooted pump a different port name
                    time.sleep(0.5)
                    continue
                link.open()
            _, result = link.send("fwstat")
            if result == "R,OK":
                state, _, _ = read_fwstat(link)
                if state == "IDLE":
                    return True
                if state == "STAGED" and not resent:
                    # The fwapply itself was lost
                    link.send("fwapply")
                    resent = True
        except (serial.SerialException, OSError, UpdateError):
            link.close()
            if not port_is_target:
                link.open()
        time.sleep(1.0)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="uncompressed firmware .bin")
    ap.add_argument("--port", required=True, help="USB serial port of the pump you're connected to")
    ap.add_argument("--id", type=int, help="update this pump through the bus; omit to update the pump on --port")
    ap.add_argument("--stop-after", type=int, help="testing: send this many bytes, then stop without installing")
    args = ap.parse_args()

    try:
        image, image_version = load_image(args.image)
    except UpdateError as e:
        print(f"FAILED: {e}", file=sys.stderr)
        return 1
    link = Link(args.port, args.id)
    who = f"pump {args.id} through {args.port}" if args.id else f"the pump on {args.port}"
    try:
        old = read_version(link)
        print(f"Updating {who}: {old} → {image_version}, {len(image)} bytes")
        if not send_image(link, image, args.stop_after):
            print(f"Stopped after {args.stop_after} bytes as requested. Still on {read_version(link)}.")
            return 0
        print("  Image verified. Installing; the pump reboots now…")
        if not wait_for_install(link, args.id is None):
            raise UpdateError(
                f"{who} didn't come back within {INSTALL_WAIT_S} s. Power-cycle it and check `version`. "
                "To recover, hold BOOTSEL while plugging its USB in, and flash it over USB.")
        new = read_version(link)
        print(f"  Done: {old} → {new}")
        if new != image_version:
            raise UpdateError(f"expected version {image_version}, the pump reports {new}")
        return 0
    except UpdateError as e:
        print(f"\nFAILED: {e}", file=sys.stderr)
        return 1
    except serial.SerialException as e:
        print(f"\nFAILED: lost the USB connection: {e}", file=sys.stderr)
        return 1
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
