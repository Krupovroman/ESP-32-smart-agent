#!/usr/bin/env python3
"""Drive the board's dev console (protocol v2, docs/architecture.md §7) over USB-Serial-JTAG.

    python tools/dev.py info                       # any device command, result printed as JSON
    python tools/dev.py shot shots/home.png        # screenshot -> PNG (add "nomask" to keep the corners)
    python tools/dev.py tap 180 260
    python tools/dev.py mic dump shots/rec.wav     # last recording -> WAV
    python tools/dev.py mic load speech.wav        # any WAV -> 16 kHz mono s16 -> injection buffer
    python tools/dev.py ota push build/round_assistant.bin   # also: ota-push <bin> [noreboot]
    python tools/dev.py wait-ready 10              # wait for "@! READY" (or a booted device)
    python tools/dev.py events 10                  # print events for 10 s
    python tools/dev.py --until ev=selftest events 15   # ... or until a matching one arrives
    python tools/dev.py cmd "settings reset"       # one device line as typed
    python tools/dev.py log 10                     # print everything the device prints for 10 s
    python tools/dev.py run "tap 180 260" "sleep 0.5" "shot shots/after.png"
    python tools/dev.py --out tests/audio/hi.wav ai fetch -l ru "Привет"   # save any command's blob

Port: COM7 by default, override with --port or the RA_PORT environment variable.
Library use: Device(port).cmd("heap") -> dict, .blob(), .shot(), .upload(), .wait_event().
"""
import argparse
import base64
import json
import os
import queue
import re
import struct
import subprocess
import sys
import threading
import time
import wave
import zlib
from array import array
from collections import deque

CRC_TEXT, CRC_EXPECTED = "123456789", "cbf43926"
FRAME_RE = re.compile(r"^@(\d+) (\S+)(?: (.*))?$")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
INJECT_RATE = 16000
INJECT_MAX_S = 30


class DevError(Exception):
    """The device answered ERR, or the exchange failed."""

    def __init__(self, err, msg, payload=None):
        super().__init__(f"{err}: {msg}")
        self.err, self.msg, self.payload = err, msg, payload or {}


class ProtocolError(DevError):
    def __init__(self, msg):
        super().__init__("PROTOCOL", msg)


# ----------------------------------------------------------------------------- transport

class SerialTransport:
    """pyserial port that reopens itself after the board re-enumerates (reboot, OTA)."""

    def __init__(self, port, open_wait=5.0):
        import serial  # only needed for real hardware (tests use a fake transport)

        self.serial = serial
        self.port = port
        self.s = None
        self.closed = False
        self.generation = 0  # bumped on every (re)open: a partial line from before is stale
        self.wlock = threading.Lock()
        self.olock = threading.RLock()  # reader and writer threads both reopen the port
        self._open(open_wait)

    def _open(self, wait):
        deadline = time.time() + wait
        while True:
            with self.olock:
                if self.closed:
                    raise OSError("transport closed")
                if self.s is not None:  # the other thread reopened it meanwhile
                    return self.s
                try:
                    s = self.serial.Serial()
                    s.port = self.port
                    s.baudrate = 115200
                    s.timeout = 0.05
                    s.write_timeout = 2.0
                    s.dtr = False  # set before open(): DTR/RTS drive the chip's reset on USB-Serial-JTAG
                    s.rts = False
                    s.open()
                    self.s = s
                    self.generation += 1
                    return s
                except (self.serial.SerialException, OSError):
                    if time.time() >= deadline:
                        raise
            time.sleep(0.3)

    def _drop(self, failed=None):
        """Closes the port; with failed, only if that handle is still the current one."""
        with self.olock:
            s = self.s
            if s is None or (failed is not None and s is not failed):
                return
            self.s = None
        try:
            s.close()
        except Exception:
            pass

    def read(self):
        """Some bytes, b'' when idle, None once closed. Survives re-enumeration."""
        if self.closed:
            return None
        s = self.s
        if s is None:
            try:
                s = self._open(0)
            except Exception:
                time.sleep(0.3)
                return b""
        try:
            # read(n) on Windows waits for all n bytes or the whole timeout: ask only for what
            # is there (or one byte, which returns as soon as it arrives), so replies and upload
            # ACKs are seen without a 50 ms delay each
            n = s.in_waiting
            data = s.read(n if n else 1)
            if data and not n:
                more = s.in_waiting
                if more:
                    data += s.read(more)
            return data
        except Exception:
            if self.closed:
                return None
            self._drop(s)
            time.sleep(0.3)
            return b""

    def write(self, data):
        with self.wlock:
            deadline = time.time() + 3.0
            while True:
                s = self.s
                try:
                    if s is None:
                        s = self._open(0)
                    s.write(data)
                    return
                except Exception:
                    if self.closed:
                        raise
                    if s is not None:  # a failed open must not close the reader's new handle
                        self._drop(s)
                    if time.time() >= deadline:
                        raise
                    time.sleep(0.3)

    def close(self):
        self.closed = True
        self._drop()


# ----------------------------------------------------------------------------- device

class Device:
    def __init__(self, port=None, transport=None, on_log=None, check=True, open_wait=5.0):
        self.t = transport or SerialTransport(port or os.environ.get("RA_PORT", "COM7"), open_wait)
        self.on_log = on_log
        self.logs = deque(maxlen=5000)
        self.events = []  # every "@! EVT" payload (dict) since connect, in order
        self.ready = None  # payload of the last "@! READY"
        self.ready_count = 0
        self._cond = threading.Condition()
        self._waiters = {}
        self._lock = threading.Lock()
        self._next_id = 1
        self._buf = b""
        self._th = threading.Thread(target=self._reader, name="dev-reader", daemon=True)
        self._th.start()
        if check:
            try:
                self.check_protocol()
            except Exception:
                self.close()  # free the port for the next tool
                raise

    # -- reader thread

    def _reader(self):
        gen = getattr(self.t, "generation", 0)
        while True:
            data = self.t.read()
            if data is None:
                return
            if getattr(self.t, "generation", 0) != gen:
                gen = self.t.generation
                self._buf = b""  # the port was reopened: drop the half line from before
            if not data:
                continue
            self._buf += data
            while b"\n" in self._buf:
                raw, self._buf = self._buf.split(b"\n", 1)
                self._dispatch(raw.rstrip(b"\r").decode("utf-8", "replace"))

    def _dispatch(self, line):
        line = ANSI_RE.sub("", line)
        if line.startswith("@! "):
            kind, _, rest = line[3:].partition(" ")
            payload = _loads(rest)
            with self._cond:
                if kind == "EVT":
                    self.events.append(payload)
                elif kind == "READY":
                    self.ready = payload
                    self.ready_count += 1
                    self.events.append(dict(payload, ev="ready"))
                self._cond.notify_all()
            return
        m = FRAME_RE.match(line)
        if m:
            q = self._waiters.get(int(m.group(1)))
            if q is not None:
                q.put((m.group(2), m.group(3) or ""))
                return
        self.logs.append(line)  # log line, id-0 result, or a frame nobody waits for
        if self.on_log:
            self.on_log(line)

    # -- request plumbing

    def _start(self, line):
        with self._lock:
            fid = self._next_id
            self._next_id = self._next_id % 65535 + 1
            q = queue.Queue()
            self._waiters[fid] = q
        self.t.write(f"#{fid} {line}\n".encode())
        return fid, q

    def _end(self, fid):
        with self._lock:
            self._waiters.pop(fid, None)

    @staticmethod
    def _next(q, timeout, what):
        try:
            return q.get(timeout=timeout)
        except queue.Empty:
            raise DevError("TIMEOUT", f"no answer to '{what}' within {timeout:.0f} s") from None

    @staticmethod
    def _final(kind, rest):
        """Result of an OK/ERR frame (None for other frames)."""
        if kind == "OK":
            return _loads(rest) if rest else {}
        if kind == "ERR":
            name, _, js = rest.partition(" ")
            p = _loads(js) if js else {}
            raise DevError(name, p.get("msg", ""), p)
        return None

    def _run(self, line, timeout, on_frame=None):
        """Sends a command and returns the OK payload; timeout counts from the last frame."""
        fid, q = self._start(line)
        try:
            while True:
                kind, rest = self._next(q, timeout, line)
                res = self._final(kind, rest)
                if res is not None:
                    return res
                if on_frame:
                    on_frame(fid, kind, rest)
        finally:
            self._end(fid)

    # -- public API

    def cmd(self, line, timeout=10.0):
        """Runs one command; returns its JSON payload (printed lines are in "out")."""
        return self._run(line, timeout)

    def check_protocol(self, timeout=3.0):
        try:
            r = self.cmd(f"crc {CRC_TEXT}", timeout)
        except DevError as e:
            if e.err == "TIMEOUT":
                raise ProtocolError("no protocol-v2 answer (old firmware, or the port is busy)") from None
            raise
        if r.get("crc32") != CRC_EXPECTED:
            raise ProtocolError(f"CRC self-check failed: {r.get('crc32')} != {CRC_EXPECTED}")
        return r

    def blob(self, line, timeout=30.0):
        """Runs a command that sends BEGIN/D/END; returns (meta, data, result)."""
        st = {"meta": None, "kind": None, "parts": {}, "end": None}

        def on_frame(_fid, kind, rest):
            if kind == "BEGIN":
                k, _, js = rest.partition(" ")
                st["kind"], st["meta"] = k, _loads(js)
            elif kind == "D":
                seq, _, b64 = rest.partition(" ")
                try:  # a line the device had to cut short (host stalled) is not valid base64
                    st["parts"][int(seq)] = base64.b64decode(b64, validate=True)
                except ValueError:
                    raise DevError("PROTOCOL", f"'{line}': damaged data line {seq[:8]}") from None
            elif kind == "END":
                st["end"] = _loads(rest)

        res = self._run(line, timeout, on_frame)
        meta = st["meta"]
        if meta is None or st["end"] is None:
            raise DevError("PROTOCOL", f"'{line}' sent no complete blob")
        parts = st["parts"]
        if sorted(parts) != list(range(len(parts))):
            raise DevError("PROTOCOL", f"blob lines missing: got {len(parts)}")
        data = b"".join(parts[i] for i in range(len(parts)))
        if len(data) != meta.get("bytes"):
            raise DevError("PROTOCOL", f"blob size {len(data)} != {meta.get('bytes')}")
        if f"{zlib.crc32(data) & 0xFFFFFFFF:08x}" != meta.get("crc32"):
            raise DevError("PROTOCOL", "blob CRC mismatch")
        meta = dict(meta, kind=st["kind"])
        return meta, data, res

    def upload(self, line, data, timeout=20.0, progress=None):
        """Uploads data for 'mic load' / 'ota push' (bytes= and crc32= are appended)."""
        crc = zlib.crc32(data) & 0xFFFFFFFF
        full = f"{line} bytes={len(data)} crc32={crc:08x}"
        fid, q = self._start(full)
        try:
            kind, rest = self._next(q, timeout, full)
            self._final(kind, rest)  # raises on ERR
            if kind != "READY":
                raise DevError("PROTOCOL", f"expected READY, got {kind}")
            p = _loads(rest)
            chunk, window = int(p["chunk"]), int(p["window"])
            n = (len(data) + chunk - 1) // chunk
            last_pct = -1
            try:
                for seq in range(n):
                    b64 = base64.b64encode(data[seq * chunk:(seq + 1) * chunk]).decode()
                    self.t.write(f"D {seq} {b64}\n".encode())
                    if (seq + 1) % window == 0:
                        while True:
                            kind, rest = self._next(q, timeout, f"ACK {seq}")
                            self._final(kind, rest)
                            if kind == "ACK" and int(rest) == seq:
                                break
                    pct = (seq + 1) * 100 // n
                    if progress and pct // 10 != last_pct // 10:
                        last_pct = pct
                        progress(pct)
                self.t.write(b"END\n")
            except BaseException:
                # end the device's upload loop now (it ignores a stray ABORT if it already
                # stopped), so the next command is not swallowed as an unexpected data line
                try:
                    self.t.write(b"ABORT\n")
                except Exception:
                    pass
                raise
            while True:
                kind, rest = self._next(q, timeout * 3, "END")
                res = self._final(kind, rest)
                if res is not None:
                    return res
        finally:
            self._end(fid)

    def shot(self, path, mask=True, region=None):
        line = "shot" if not region else "shot " + " ".join(str(v) for v in region)
        meta, data, _ = self.blob(line)
        rgb565le_to_png(path, meta["w"], meta["h"], data, mask=mask and not region)
        return meta

    def mic_dump(self, path):
        _, data, _ = self.blob("mic dump")
        _write(path, data)
        return len(data)

    def mic_load(self, path_or_bytes, progress=None):
        data = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else wav_prepare(path_or_bytes)
        return self.upload("mic load", bytes(data), progress=progress)

    def event_mark(self):
        """Index to pass as since= so wait_event only looks at later events."""
        with self._cond:
            return len(self.events)

    def wait_event(self, pred, timeout=10.0, since=None):
        """Waits for an event matching pred (a callable, or a dict of required key/values)."""
        if isinstance(pred, dict):
            want = pred
            pred = lambda e: all(e.get(k) == v for k, v in want.items())  # noqa: E731
        deadline = time.time() + timeout
        with self._cond:
            i = len(self.events) if since is None else since
            while True:
                while i < len(self.events):
                    e = self.events[i]
                    i += 1
                    if pred(e):
                        return e
                left = deadline - time.time()
                if left <= 0:
                    raise DevError("TIMEOUT", "event not seen")
                self._cond.wait(left)

    def wait_ready(self, timeout=10.0, after=None):
        """Waits for '@! READY' after now, or for a device that already booted (ping).
        after: time.time() of a requested reboot; a ping then only counts when the device's
        uptime shows it restarted since (READY may be printed before the port is reopened)."""
        deadline = time.time() + timeout
        with self._cond:
            seen = self.ready_count
        while time.time() < deadline:
            with self._cond:
                if self.ready_count > seen:
                    return self.ready
            try:
                p = self.cmd("ping", timeout=1.0)
                booted = p.get("booted", p.get("ready"))
                fresh = after is None or p.get("uptime_ms", 0) <= (time.time() - after) * 1000 + 1000
                if booted and fresh:
                    return p
            except (DevError, OSError):
                pass
            with self._cond:
                self._cond.wait(0.3)
        raise DevError("TIMEOUT", f"device not ready within {timeout:.0f} s")

    def close(self):
        self.t.close()


# ----------------------------------------------------------------------------- data helpers

def _loads(s):
    s = (s or "").strip()
    if not s:
        return {}
    try:
        return json.loads(s)
    except ValueError:
        return {"raw": s}


def _write(path, data):
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def rgb565le_to_png(path, w, h, data, mask=True):
    """RGB565 little-endian -> 8-bit RGB PNG (stdlib only). mask dims pixels outside r = w/2."""
    rows = []
    r2 = (w / 2) ** 2
    for y in range(h):
        row = bytearray(b"\x00")
        for x in range(w):
            i = (y * w + x) * 2
            v = data[i] | (data[i + 1] << 8)
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            px = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
            dx, dy = x - w / 2 + 0.5, y - h / 2 + 0.5
            if mask and dx * dx + dy * dy > r2:  # outside the round glass
                px = tuple(c // 5 for c in px)
            row += bytes(px)
        rows.append(bytes(row))

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
    png += chunk(b"IEND", b"")
    _write(path, png)


def wav_bytes(pcm, rate=INJECT_RATE):
    """16-bit mono WAV file image around little-endian PCM bytes."""
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
    hdr += struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    return hdr + b"data" + struct.pack("<I", len(pcm)) + bytes(pcm)


def wav_prepare(path, rate=INJECT_RATE, max_s=INJECT_MAX_S):
    """Reads any PCM WAV and returns a 16 kHz mono s16 WAV (downmix, linear resample).
    A file that already is 16 kHz mono s16 passes through bit-exact."""
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if ch == 1 and sw == 2 and sr == rate:
        pcm = raw
    else:
        mono = _downmix(_decode(raw, sw), ch)
        pcm = _resample(mono, sr, rate).tobytes() if sys.byteorder == "little" else _swap16(_resample(mono, sr, rate))
    if len(pcm) // 2 > rate * max_s:
        raise ValueError(f"{path}: longer than {max_s} s")
    return wav_bytes(pcm, rate)


def _decode(raw, sw):
    """Samples as ints scaled to the 16-bit range."""
    if sw == 1:
        return [(b - 128) << 8 for b in raw]
    if sw == 2:
        a = array("h")
        a.frombytes(raw)
        if sys.byteorder != "little":
            a.byteswap()
        return a.tolist()
    if sw == 3:
        return [int.from_bytes(raw[i:i + 3], "little", signed=True) >> 8 for i in range(0, len(raw) - 2, 3)]
    if sw == 4:
        a = array("i")
        a.frombytes(raw)
        if sys.byteorder != "little":
            a.byteswap()
        return [v >> 16 for v in a]
    raise ValueError(f"unsupported sample width {sw}")


def _downmix(samples, ch):
    if ch == 1:
        return samples
    return [sum(samples[i:i + ch]) // ch for i in range(0, len(samples) - ch + 1, ch)]


def _resample(samples, src, dst):
    out = array("h")
    if not samples:
        return out
    if src == dst:
        out.extend(max(-32768, min(32767, v)) for v in samples)
        return out
    n = len(samples) * dst // src
    last = len(samples) - 1
    step = src / dst
    for i in range(n):
        pos = i * step
        i0 = int(pos)
        frac = pos - i0
        a = samples[i0]
        b = samples[i0 + 1] if i0 < last else a
        v = int(round(a + (b - a) * frac))
        out.append(max(-32768, min(32767, v)))
    return out


def _swap16(a):
    a = array("h", a)
    a.byteswap()
    return a.tobytes()


# ----------------------------------------------------------------------------- CLI

_print_lock = threading.Lock()


def _say(*a):
    with _print_lock:
        print(*a, flush=True)


def print_result(res):
    for line in res.get("out", []):
        _say(line)
    rest = {k: v for k, v in res.items() if k != "out"}
    if rest:
        _say(json.dumps(rest, ensure_ascii=False))


def _is_number(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


def is_local_listen(parts):
    """'log [s]' / 'events [s]' listen on the PC; 'log <tag> <level>' is the device command."""
    return bool(parts) and parts[0] in ("log", "events") and (
        len(parts) == 1 or (len(parts) == 2 and _is_number(parts[1])))


def run_one(dev, text, args):
    parts = text.split()
    if not parts:
        return
    head = parts[0]
    if head == "shot" and parts[1:2] != ["snap"]:
        # shot [x y w h] [out.png] [nomask]
        rest = parts[1:]
        region = None
        if len(rest) >= 4 and all(p.isdigit() for p in rest[:4]):
            region, rest = [int(p) for p in rest[:4]], rest[4:]
        files = [p for p in rest if p != "nomask"]
        path = files[0] if files else "shots/shot.png"
        t0 = time.time()
        mask = "nomask" not in rest and not getattr(args, "no_mask", False)
        meta = dev.shot(path, mask=mask, region=region)
        _say(f"saved {path} ({meta['w']}x{meta['h']}, {time.time() - t0:.1f}s)")
    elif parts[:2] == ["mic", "dump"] or head == "mic-dump":
        rest = parts[2:] if head == "mic" else parts[1:]
        path = rest[0] if rest else "shots/mic.wav"
        _say(f"saved {path} ({dev.mic_dump(path)} bytes)")
    elif (parts[:2] == ["mic", "load"] and len(parts) > 2 and not parts[2].startswith("bytes=")) or head == "wav":
        path = parts[-1]
        print_result(dev.mic_load(path, progress=lambda p: _say(f"upload {p}%")))
    elif (parts[:2] == ["ota", "push"] and len(parts) > 2 and not parts[2].startswith("bytes=")) or head == "ota-push":
        path = parts[2] if head == "ota" else parts[1]
        line = "ota push" + (" noreboot" if "noreboot" in parts else "")
        with open(path, "rb") as f:
            img = f.read()
        t0 = time.time()
        res = dev.upload(line, img, timeout=30.0, progress=lambda p: _say(f"upload {p}%"))
        print_result(res)
        _say(f"pushed {len(img)} bytes in {time.time() - t0:.1f}s")
        if res.get("reboot"):
            ready = dev.wait_ready(timeout=30.0, after=time.time())
            _say("ready: " + json.dumps(ready, ensure_ascii=False))
            print_result(dev.cmd("ota info"))
    elif head == "wait-ready":
        timeout = float(parts[1]) if len(parts) > 1 else args.timeout
        # after a reboot in the same run, a ping must come from the new boot
        _say(json.dumps(dev.wait_ready(timeout, after=getattr(args, "last_reboot", None)), ensure_ascii=False))
    elif head == "sleep":
        time.sleep(float(parts[1]))
    elif is_local_listen(parts):
        secs = float(parts[1]) if len(parts) > 1 else 5.0
        until = parse_until(getattr(args, "until", None))
        mark = dev.event_mark()
        end = time.time() + secs
        while time.time() < end:
            time.sleep(0.05)
            while mark < len(dev.events):  # log lines print themselves through on_log
                e = dev.events[mark]
                ev = json.dumps(e, ensure_ascii=False)
                _say(ev if head == "events" else "@! EVT " + ev)
                mark += 1
                if until and event_matches(e, until):
                    return
        if until:
            raise DevError("TIMEOUT", f"no event with {args.until} within {secs:g} s")
    elif getattr(args, "out", None):
        # any command that sends a blob (e.g. "ai fetch -l ru <text>" -> WAV)
        meta, data, res = dev.blob(text, timeout=args.timeout)
        _write(args.out, data)
        print_result(res)
        _say(f"saved {args.out} ({len(data)} bytes, {meta.get('kind')})")
    else:
        res = dev.cmd(text, timeout=args.timeout)
        if res.get("reboot") or res.get("rollback"):
            args.last_reboot = time.time()
        print_result(res)


def parse_until(spec):
    """'ev=state,to=idle' -> {'ev': 'state', 'to': 'idle'} (None/'' -> None)."""
    if not spec:
        return None
    out = {}
    for part in spec.split(","):
        k, sep, v = part.partition("=")
        if not sep or not k:
            raise ValueError(f"--until wants key=value[,key=value], got '{spec}'")
        out[k.strip()] = v.strip()
    return out


def event_matches(ev, want):
    """Values compare as text, so 'turn=7' and 'ok=true' match JSON numbers and booleans."""
    def text(v):
        return json.dumps(v) if isinstance(v, bool) or v is None else str(v)
    return all(k in ev and text(ev[k]) == v for k, v in want.items())


def join_argv(args):
    """One command line from CLI words; words with spaces or quotes are quoted the way the
    device's esp_console_split_argv() reads them, so "set sys_prompt 'two words'" keeps one value."""
    out = []
    for a in args:
        if a == "" or any(c.isspace() or c == '"' for c in a):
            a = '"' + a.replace("\\", "\\\\").replace('"', '\\"') + '"'
        out.append(a)
    return " ".join(out)


def esptool_reset(port):
    """Hard reset through esptool (closes nothing: call it while no Device holds the port)."""
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, "--after", "hard-reset",
                    "chip-id"], check=True)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True, encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("RA_PORT", "COM7"))
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds of silence before a command fails")
    ap.add_argument("--no-logs", action="store_true", help="do not print device log lines")
    ap.add_argument("--no-check", action="store_true", help="skip the CRC handshake on connect")
    ap.add_argument("--out", metavar="FILE", help="save the blob the command sends (e.g. ai fetch) to FILE")
    ap.add_argument("--mask", action="store_true", help="shot: dim pixels outside the round glass (default)")
    ap.add_argument("--no-mask", action="store_true", help="shot: keep the corners as captured")
    ap.add_argument("--until", metavar="K=V[,K=V]", help="events: stop at the first event with these values")
    # REMAINDER: everything from the first word on is the command, so device options such as
    # "ai fetch -l ru ..." or "ai ask -a ..." are not taken for dev.py options (those go first)
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    if not a.command:
        ap.error("a command is required")
    try:
        parse_until(a.until)
    except ValueError as e:
        ap.error(str(e))
    if a.command[0] == "reset":
        esptool_reset(a.port)
        return 0
    passive = a.command[0] == "wait-ready" or is_local_listen(a.command)
    log_cb = None if a.no_logs else (lambda line: _say(line))
    # right after a flash or reset the port re-enumerates: wait-ready also waits for it to appear
    open_wait = min(5.0, a.timeout)
    if a.command[0] == "wait-ready":
        open_wait = float(a.command[1]) if len(a.command) > 1 and _is_number(a.command[1]) else a.timeout
    try:
        dev = Device(a.port, on_log=log_cb, check=not (a.no_check or passive), open_wait=open_wait)
    except DevError as e:  # ProtocolError, or the handshake answered ERR
        print(f"error: {e.msg}", file=sys.stderr)
        return 2
    except OSError as e:  # port missing or held by another program (monitor, flasher)
        print(f"error: cannot open {a.port}: {e}", file=sys.stderr)
        return 2
    try:
        if a.command[0] == "run":
            cmds = a.command[1:]
        elif a.command[0] == "cmd":   # dev.py cmd "settings reset": the argument is the device line
            cmds = [a.command[1] if len(a.command) == 2 else join_argv(a.command[1:])]
        else:
            cmds = [join_argv(a.command)]
        for c in cmds:
            run_one(dev, c, a)
        time.sleep(0.1)  # let trailing log lines print
    except DevError as e:
        for line in e.payload.get("out", []):
            _say(line)
        print(f"ERR {e.err}: {e.msg}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
