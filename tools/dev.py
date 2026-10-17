#!/usr/bin/env python3
"""Drive the board's dev console over the native USB-Serial-JTAG port.

    python tools/dev.py info
    python tools/dev.py shot shots/home.png        # screenshot -> PNG
    python tools/dev.py tap 180 260
    python tools/dev.py swipe 300 180 60 180 300
    python tools/dev.py log 10                     # print device output for 10 s
    python tools/dev.py run "tap 180 260" "shot shots/after.png"

Port: COM7 by default, override with --port or the RA_PORT environment variable.
"""
import argparse
import base64
import os
import re
import struct
import sys
import time
import zlib

import serial

RESULT_RE = re.compile(r"^(?:ra>\s*)*(OK|ERR.*)$")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.1
    s.dtr = False  # keep the chip running (DTR/RTS drive reset on USB-Serial-JTAG)
    s.rts = False
    s.open()
    return s


class Console:
    def __init__(self, port):
        self.s = open_port(port)
        self.buf = b""
        self.s.write(b"\n")
        self.drain(0.4)

    def drain(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.s.read(4096)

    def lines(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(8192)
            if chunk:
                self.buf += chunk
                end = max(end, time.time() + 2.0)  # keep going while data flows
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                yield ANSI_RE.sub("", line.decode("utf-8", "replace")).rstrip("\r")
        raise TimeoutError("no response from device")

    def cmd(self, text, timeout=10.0, echo=True):
        """Send one command; return (ok, lines) once 'OK'/'ERR' arrives."""
        self.s.write(text.encode() + b"\n")
        out = []
        for line in self.lines(timeout):
            m = RESULT_RE.match(line.strip())
            if m:
                return m.group(1) == "OK", out
            if line.strip().endswith(text):  # echo of our own command
                continue
            out.append(line)
            if echo and not line.startswith("SHOT:"):
                print(line)
        return False, out


def rgb565le_to_png(path, w, h, data):
    rows = []
    for y in range(h):
        row = bytearray(b"\x00")
        for x in range(w):
            i = (y * w + x) * 2
            v = data[i] | (data[i + 1] << 8)
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            dx, dy = x - w / 2 + 0.5, y - h / 2 + 0.5
            if dx * dx + dy * dy > (w / 2) ** 2:  # outside the round glass: dim it
                row += bytes((((r << 3) | (r >> 2)) // 5, ((g << 2) | (g >> 4)) // 5, ((b << 3) | (b >> 2)) // 5))
            else:
                row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        rows.append(bytes(row))

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
    png += chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def do_shot(con, path):
    t0 = time.time()
    ok, lines = con.cmd("shot", timeout=20.0, echo=False)
    if not ok:
        sys.exit("shot failed: " + " | ".join(lines[-3:]))
    header = next((l for l in lines if l.startswith("SHOT:BEGIN")), None)
    if not header:
        sys.exit("no SHOT:BEGIN in response")
    _, w, h, fmt, n, crc = header.split()
    payload = "".join(l[5:] for l in lines if l.startswith("SHOT:") and not l.startswith(("SHOT:BEGIN", "SHOT:END")))
    data = base64.b64decode(payload)
    if len(data) != int(n):
        sys.exit(f"size mismatch {len(data)} != {n}")
    if zlib.crc32(data) & 0xFFFFFFFF != int(crc, 16):
        sys.exit("CRC mismatch")
    rgb565le_to_png(path, int(w), int(h), data)
    print(f"saved {path} ({w}x{h}, {time.time() - t0:.1f}s)")


def run_one(con, text):
    parts = text.split()
    if parts[0] == "shot":
        do_shot(con, parts[1] if len(parts) > 1 else "shots/shot.png")
    elif parts[0] == "sleep":
        time.sleep(float(parts[1]))
    elif parts[0] == "log":
        end = time.time() + float(parts[1] if len(parts) > 1 else 5)
        try:
            for line in con.lines(float(parts[1] if len(parts) > 1 else 5)):
                print(line)
                if time.time() > end:
                    break
        except TimeoutError:
            pass
    else:
        ok, _ = con.cmd(text)
        if not ok:
            sys.exit(f"command failed: {text}")


def main():
    sys.stdout.reconfigure(line_buffering=True)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("RA_PORT", "COM7"))
    ap.add_argument("command", nargs="+")
    a = ap.parse_args()
    con = Console(a.port)
    if a.command[0] == "run":
        for c in a.command[1:]:
            run_one(con, c)
    else:
        run_one(con, " ".join(a.command))


if __name__ == "__main__":
    main()
