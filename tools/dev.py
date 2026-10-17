#!/usr/bin/env python3
"""Drive the board's dev console over the native USB-Serial-JTAG port.

    python tools/dev.py info
    python tools/dev.py shot shots/home.png        # screenshot -> PNG
    python tools/dev.py tap 180 260
    python tools/dev.py swipe 300 180 60 180 300
    python tools/dev.py mic dump shots/rec.wav     # last recording -> WAV
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


def fetch_blob(con, command, timeout=30.0):
    """Run a command that answers with BLOB:BEGIN/BLOB:END; return (kind, meta, bytes)."""
    ok, lines = con.cmd(command, timeout=timeout, echo=False)
    if not ok:
        sys.exit(f"{command} failed: " + " | ".join(lines[-3:]))
    header = next((l for l in lines if l.startswith("BLOB:BEGIN")), None)
    if not header:
        sys.exit("no BLOB:BEGIN in response")
    parts = header.split()
    kind, n, crc, meta = parts[1], int(parts[2]), int(parts[3], 16), parts[4:]
    payload = "".join(l[5:] for l in lines if l.startswith("BLOB:") and not l.startswith(("BLOB:BEGIN", "BLOB:END")))
    data = base64.b64decode(payload)
    if len(data) != n:
        sys.exit(f"size mismatch {len(data)} != {n}")
    if zlib.crc32(data) & 0xFFFFFFFF != crc:
        sys.exit("CRC mismatch")
    for l in lines:
        if not l.startswith("BLOB:"):
            print(l)
    return kind, meta, data


def do_shot(con, path):
    t0 = time.time()
    _, meta, data = fetch_blob(con, "shot")
    w, h = int(meta[0]), int(meta[1])
    rgb565le_to_png(path, w, h, data)
    print(f"saved {path} ({w}x{h}, {time.time() - t0:.1f}s)")


def do_save_blob(con, command, path):
    _, _, data = fetch_blob(con, command)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print(f"saved {path} ({len(data)} bytes)")


def run_one(con, text):
    parts = text.split()
    if parts[0] == "shot":
        do_shot(con, parts[1] if len(parts) > 1 else "shots/shot.png")
    elif parts[:2] == ["mic", "dump"]:
        do_save_blob(con, "mic dump", parts[2] if len(parts) > 2 else "shots/mic.wav")
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
        ok, _ = con.cmd(text, timeout=60.0)
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
