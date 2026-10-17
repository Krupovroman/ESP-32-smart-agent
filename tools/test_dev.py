#!/usr/bin/env python3
"""Offline tests of tools/dev.py (protocol v2) against a simulated device.

    python tools/test_dev.py          # stdlib only, no board or pyserial needed

The fake transport answers like the firmware (components/devconsole): "#id" requests,
@id OK/ERR results, BEGIN/D/END blobs, READY/ACK uploads, "@! EVT", "@! READY" and id-less
log lines, and hands its output back in random fragments so line reassembly is exercised.
"""
import base64
import json
import math
import os
import queue
import random
import struct
import sys
import tempfile
import threading
import types
import unittest
import wave
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev  # noqa: E402


def crc_hex(data):
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


class FakeTransport:
    """Simulated firmware behind a byte pipe."""

    BLOB_CHUNK = 3072
    UP_CHUNK, UP_WINDOW = 1024, 2

    def __init__(self, seed=1):
        self.rng = random.Random(seed)
        self.out = queue.Queue()
        self.closed = False
        self.generation = 1
        self.inbuf = b""
        self.upload = None
        self.uploaded = None
        self.requests = []
        self.max_in_flight = 0
        self.lock = threading.Lock()
        self.shot_px = None

    # -- byte pipe

    def read(self):
        if self.closed:
            return None
        try:
            return self.out.get(timeout=0.02)
        except queue.Empty:
            return b""

    def write(self, data):
        with self.lock:
            self.inbuf += data
            while b"\n" in self.inbuf:
                line, self.inbuf = self.inbuf.split(b"\n", 1)
                self.handle(line.decode())

    def close(self):
        self.closed = True

    def emit(self, line):
        b = (line + "\n").encode()
        i = 0
        while i < len(b):  # random fragments, sometimes splitting inside a line
            n = self.rng.randint(1, 300)
            self.out.put(b[i:i + n])
            i += n

    # -- firmware

    def blob(self, fid, kind, data, meta):
        self.emit(f"@{fid} BEGIN {kind} {{{meta},\"bytes\":{len(data)},\"crc32\":\"{crc_hex(data)}\"}}")
        seq = 0
        for off in range(0, len(data), self.BLOB_CHUNK):
            self.emit(f"@{fid} D {seq} " + base64.b64encode(data[off:off + self.BLOB_CHUNK]).decode())
            if seq == 0:  # output of other tasks between frames
                self.emit("I (1234) audio_in: mic ready (slot right, shift 12)")
                self.emit('@! EVT {"t":1234,"ev":"touch","x":10,"y":20,"p":1}')
            seq += 1
        self.emit(f"@{fid} END {{\"bytes\":{len(data)},\"crc32\":\"{crc_hex(data)}\",\"lines\":{seq}}}")

    def handle(self, line):
        if self.upload is not None:
            return self.handle_upload(line)
        self.requests.append(line)
        if not line.startswith("#"):
            return
        head, _, cmd = line.partition(" ")
        fid = int(head[1:])
        argv = cmd.split()
        name = argv[0] if argv else ""
        if name == "crc":
            text = " ".join(argv[1:]).encode()
            self.emit(f'@{fid} OK {{"crc32":"{crc_hex(text)}","len":{len(text)}}}')
        elif name == "ping":
            self.emit(f'@{fid} OK {{"uptime_ms":500,"booted":true,"ready":true,"proto":2}}')
        elif name == "info":
            self.emit("W (99) touch: CST816 NACK")
            self.emit(f'@{fid} OK {{"ver":"m3-1-gabc","part":"ota_0","out":["line a"]}}')
        elif name == "fail":
            self.emit(f'@{fid} ERR ESP_FAIL {{"msg":"boom","out":["partial"]}}')
        elif name == "slow":
            pass
        elif name == "shot":
            w, h = (int(argv[3]), int(argv[4])) if len(argv) == 5 else (60, 60)
            px = bytearray()
            for y in range(h):
                for x in range(w):
                    px += struct.pack("<H", 0xF800 if x < w // 2 else 0x001F)
            self.shot_px = bytes(px)
            self.blob(fid, "shot", self.shot_px, f'"w":{w},"h":{h},"fmt":"rgb565le"')
            self.emit(f"@{fid} OK {{}}")
        elif name == "badblob":
            data = b"\x01" * 100
            self.emit(f'@{fid} BEGIN wav {{"bytes":100,"crc32":"00000000"}}')
            self.emit(f"@{fid} D 0 " + base64.b64encode(data).decode())
            self.emit(f'@{fid} END {{"bytes":100,"crc32":"00000000"}}')
            self.emit(f"@{fid} OK {{}}")
        elif name == "cutblob":  # a D line the device cut short when the host stalled
            data = b"\x02" * 300
            self.emit(f'@{fid} BEGIN wav {{"bytes":300,"crc32":"{crc_hex(data)}"}}')
            self.emit(f"@{fid} D 0 " + base64.b64encode(data).decode()[:123])
            self.emit(f'@{fid} END {{"bytes":300,"crc32":"{crc_hex(data)}"}}')
            self.emit(f"@{fid} OK {{}}")
        elif name == "mic" and argv[1:2] == ["dump"]:
            wavb = dev.wav_bytes(b"\x10\x00\xf0\xff" * 4000)
            self.blob(fid, "wav", wavb, '"rate":16000,"ch":1,"fmt":"s16le"')
            self.emit(f"@{fid} OK {{}}")
        elif name in ("mic", "ota") and argv[1:2] in (["load"], ["push"]):
            kv = dict(a.split("=", 1) for a in argv[2:] if "=" in a)
            self.upload = {"fid": fid, "bytes": int(kv["bytes"]), "crc": kv["crc32"], "data": bytearray(),
                           "seq": 0, "since_ack": 0, "kind": argv[1]}
            self.emit(f'@{fid} READY {{"chunk":{self.UP_CHUNK},"window":{self.UP_WINDOW}}}')
        elif name == "reboot":
            self.emit(f'@{fid} OK {{"reboot":"app"}}')
            threading.Timer(0.2, lambda: self.emit(
                '@! READY {"ver":"m3-1-gabc","reset":"software","boot":7,"part":"ota_0","boot_ms":930,'
                '"selftest":"ok","safe_mode":false,"proto":2}')).start()
        else:
            self.emit(f'@{fid} ERR ESP_ERR_NOT_FOUND {{"msg":"unknown command: {name}"}}')

    def handle_upload(self, line):
        u = self.upload
        fid = u["fid"]
        if line == "END":
            self.upload = None
            data = bytes(u["data"])
            if len(data) != u["bytes"] or crc_hex(data) != u["crc"]:
                self.emit(f'@{fid} ERR ESP_ERR_INVALID_CRC {{"msg":"crc mismatch"}}')
                return
            self.uploaded = data
            if u["kind"] == "push":
                self.emit(f'@{fid} OK {{"bytes":{len(data)},"boot":"ota_1","reboot":false}}')
            else:
                self.emit(f'@{fid} OK {{"samples":{(len(data) - 44) // 2},"bytes":{len(data)}}}')
            return
        assert line.startswith("D "), line
        _, seq, b64 = line.split(" ", 2)
        assert int(seq) == u["seq"], (seq, u["seq"])
        chunk = base64.b64decode(b64)
        assert len(chunk) <= self.UP_CHUNK
        u["data"] += chunk
        u["seq"] += 1
        u["since_ack"] += 1
        self.max_in_flight = max(self.max_in_flight, u["since_ack"])
        if u["seq"] % self.UP_WINDOW == 0:
            u["since_ack"] = 0
            self.emit(f"@{fid} ACK {u['seq'] - 1}")


def make_wav(path, rate, ch, seconds, freq=440.0, sampwidth=2):
    n = int(rate * seconds)
    with wave.open(path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(sampwidth)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            v = int(12000 * math.sin(2 * math.pi * freq * i / rate))
            for c in range(ch):
                if sampwidth == 2:
                    frames += struct.pack("<h", v if c == 0 else v // 2)
                else:
                    frames += bytes([(v >> 8) + 128])
        w.writeframes(bytes(frames))
    return n


class DevTests(unittest.TestCase):
    def setUp(self):
        self.fake = FakeTransport()
        self.logs = []
        self.dev = dev.Device(transport=self.fake, on_log=self.logs.append)
        self.tmp = tempfile.mkdtemp(prefix="devpy_")

    def tearDown(self):
        self.dev.close()

    def test_crc_and_base64_vectors(self):
        # the same vectors as dc_codec_selfcheck() in components/devconsole/upload.c
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)
        self.assertEqual(zlib.crc32(b"56789", zlib.crc32(b"1234")), 0xCBF43926)
        self.assertEqual(base64.b64encode(b"foobar"), b"Zm9vYmFy")
        self.assertEqual(base64.b64encode(b"fo"), b"Zm8=")
        self.assertEqual(base64.b64decode(b"Zm9vYg=="), b"foob")

    def test_handshake_sent_crc(self):
        self.assertTrue(any(r.endswith("crc 123456789") for r in self.fake.requests))
        self.assertTrue(all(r.startswith("#") for r in self.fake.requests))

    def test_cmd_json_out_and_logs(self):
        r = self.dev.cmd("info")
        self.assertEqual(r["ver"], "m3-1-gabc")
        self.assertEqual(r["out"], ["line a"])
        self.assertIn("W (99) touch: CST816 NACK", self.logs)

    def test_err_raises(self):
        with self.assertRaises(dev.DevError) as cm:
            self.dev.cmd("fail")
        self.assertEqual(cm.exception.err, "ESP_FAIL")
        self.assertEqual(cm.exception.msg, "boom")
        self.assertEqual(cm.exception.payload["out"], ["partial"])
        with self.assertRaises(dev.DevError) as cm:
            self.dev.cmd("nosuch")
        self.assertEqual(cm.exception.err, "ESP_ERR_NOT_FOUND")

    def test_timeout(self):
        with self.assertRaises(dev.DevError) as cm:
            self.dev.cmd("slow", timeout=0.3)
        self.assertEqual(cm.exception.err, "TIMEOUT")
        self.assertEqual(self.dev.cmd("ping")["proto"], 2)  # still usable afterwards

    def test_shot_png(self):
        path = os.path.join(self.tmp, "s.png")
        meta = self.dev.shot(path, mask=False)
        self.assertEqual((meta["w"], meta["h"]), (60, 60))
        with open(path, "rb") as f:
            png = f.read()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        w, h = struct.unpack(">II", png[16:24])
        self.assertEqual((w, h), (60, 60))
        idat_len = struct.unpack(">I", png[33:37])[0]
        raw = zlib.decompress(png[41:41 + idat_len])
        self.assertEqual(raw[0], 0)                    # filter byte
        self.assertEqual(tuple(raw[1:4]), (255, 0, 0))  # 0xF800 -> red
        self.assertEqual(tuple(raw[1 + 59 * 3:1 + 60 * 3]), (0, 0, 255))
        # the event and log that arrived between the D lines were routed, not lost
        self.assertEqual(self.dev.wait_event({"ev": "touch"}, timeout=1, since=0)["x"], 10)
        self.assertIn("I (1234) audio_in: mic ready (slot right, shift 12)", self.logs)

    def test_shot_crop_cli(self):
        path = os.path.join(self.tmp, "c.png")
        args = types.SimpleNamespace(timeout=5.0)
        dev.run_one(self.dev, f"shot 0 0 4 2 {path}", args)
        self.assertTrue(any(r.endswith("shot 0 0 4 2") for r in self.fake.requests))
        self.assertTrue(os.path.exists(path))

    def test_blob_crc_mismatch(self):
        with self.assertRaises(dev.DevError) as cm:
            self.dev.blob("badblob")
        self.assertIn("CRC", cm.exception.msg)

    def test_blob_damaged_line(self):
        with self.assertRaises(dev.DevError) as cm:
            self.dev.blob("cutblob")
        self.assertEqual(cm.exception.err, "PROTOCOL")
        self.assertEqual(self.dev.cmd("ping")["proto"], 2)  # still usable afterwards

    def test_mic_dump(self):
        path = os.path.join(self.tmp, "m.wav")
        n = self.dev.mic_dump(path)
        with wave.open(path, "rb") as w:
            self.assertEqual((w.getframerate(), w.getnchannels(), w.getsampwidth()), (16000, 1, 2))
            self.assertEqual(w.getnframes(), 8000)
        self.assertEqual(n, 44 + 16000)

    def test_upload_windows_and_crc(self):
        data = bytes(random.Random(5).randrange(256) for _ in range(10 * 1024 + 77))
        seen = []
        r = self.dev.upload("ota push", data, progress=seen.append)
        self.assertEqual(self.fake.uploaded, data)
        self.assertLessEqual(self.fake.max_in_flight, FakeTransport.UP_WINDOW)
        self.assertEqual(r["bytes"], len(data))
        self.assertEqual(seen[-1], 100)

    def test_mic_load_resamples(self):
        src = os.path.join(self.tmp, "in.wav")
        n = make_wav(src, 44100, 2, 0.5)
        r = self.dev.mic_load(src)
        up = self.fake.uploaded
        self.assertEqual(up[:4], b"RIFF")
        rate, = struct.unpack("<I", up[24:28])
        ch, = struct.unpack("<H", up[22:24])
        self.assertEqual((rate, ch), (16000, 1))
        self.assertEqual(r["samples"], n * 16000 // 44100)

    def test_wav_prepare_passthrough_bit_exact(self):
        src = os.path.join(self.tmp, "p.wav")
        make_wav(src, 16000, 1, 0.25)
        with wave.open(src, "rb") as w:
            frames = w.readframes(w.getnframes())
        out = dev.wav_prepare(src)
        self.assertEqual(out[44:], frames)
        self.assertEqual(struct.unpack("<I", out[40:44])[0], len(frames))

    def test_wav_prepare_8bit_and_limits(self):
        src = os.path.join(self.tmp, "u8.wav")
        make_wav(src, 8000, 1, 0.1, sampwidth=1)
        out = dev.wav_prepare(src)
        self.assertEqual(len(out) - 44, 2 * 1600)
        longf = os.path.join(self.tmp, "long.wav")
        make_wav(longf, 1000, 1, 31)
        with self.assertRaises(ValueError):
            dev.wav_prepare(longf)

    def test_reboot_ready(self):
        mark = self.dev.event_mark()
        self.assertEqual(self.dev.cmd("reboot")["reboot"], "app")
        ready = self.dev.wait_event({"ev": "ready"}, timeout=2, since=mark)
        self.assertEqual(ready["part"], "ota_0")
        self.assertEqual(self.dev.ready["boot"], 7)

    def test_wait_ready_ping_fallback(self):
        # no READY arrives, but ping reports a booted device
        self.assertTrue(self.dev.wait_ready(timeout=2)["booted"])

    def test_wait_ready_after_reboot_ignores_old_uptime(self):
        # the fake reports uptime 500 ms: a reboot requested 0.1 s ago is "fresh" enough
        # (uptime <= elapsed + 1 s), one requested in the future is not and needs READY
        import time as _t
        self.assertTrue(self.dev.wait_ready(timeout=1, after=_t.time() - 0.1)["booted"])
        with self.assertRaises(dev.DevError):
            self.dev.wait_ready(timeout=0.8, after=_t.time() + 5)

    def test_run_reboot_then_wait_ready(self):
        args = types.SimpleNamespace(timeout=5.0)
        dev.run_one(self.dev, "reboot", args)
        self.assertTrue(hasattr(args, "last_reboot"))
        dev.run_one(self.dev, "wait-ready 3", args)   # satisfied by the READY the fake sends

    def test_unrouted_frames_are_logs(self):
        self.fake.emit("@0 OK {}")
        self.fake.emit("@999 ERR ESP_FAIL {\"msg\":\"late\"}")
        self.dev.cmd("ping")
        self.assertIn("@0 OK {}", self.logs)


class FakePort:
    """Stands in for serial.Serial: records the sizes asked from read()."""

    instances = []

    def __init__(self):
        self.pending = bytearray()
        self.asked = []
        self.closed = False
        self.written = b""
        FakePort.instances.append(self)

    def open(self):
        pass

    @property
    def in_waiting(self):
        if self.closed:
            raise OSError("closed")
        return len(self.pending)

    def read(self, n):
        self.asked.append(n)
        out, self.pending = bytes(self.pending[:n]), self.pending[n:]
        return out

    def write(self, data):
        if self.closed:
            raise OSError("closed")
        self.written += data

    def close(self):
        self.closed = True


class TransportTests(unittest.TestCase):
    def setUp(self):
        FakePort.instances = []
        self.saved = sys.modules.get("serial")
        sys.modules["serial"] = types.SimpleNamespace(Serial=FakePort, SerialException=OSError)
        self.t = dev.SerialTransport("COMX", open_wait=0)

    def tearDown(self):
        if self.saved is None:
            del sys.modules["serial"]
        else:
            sys.modules["serial"] = self.saved

    def test_read_asks_only_for_pending_bytes(self):
        p = self.t.s
        p.pending += b"@1 OK {}\n"
        self.assertEqual(self.t.read(), b"@1 OK {}\n")
        self.assertEqual(p.asked, [9])        # never a blocking read(4096)
        self.assertEqual(self.t.read(), b"")  # idle: one byte with the short timeout
        self.assertEqual(p.asked[-1], 1)

    def test_stale_handle_does_not_close_the_new_one(self):
        old = self.t.s
        self.t._drop(old)
        new = self.t._open(0)
        self.assertIsNot(old, new)
        self.t._drop(old)                     # the other thread reports the old failure late
        self.assertIs(self.t.s, new)
        self.assertFalse(new.closed)

    def test_write_reopens_after_failure(self):
        self.t.s.closed = True                # the board re-enumerated
        self.t.write(b"#1 ping\n")
        self.assertEqual(self.t.s.written, b"#1 ping\n")
        self.assertEqual(len(FakePort.instances), 2)


class ParserTests(unittest.TestCase):
    def test_join_argv_quotes_spaces_only(self):
        self.assertEqual(dev.join_argv(["shot", "shots\\a.png"]), "shot shots\\a.png")
        self.assertEqual(dev.join_argv(["set", "sys_prompt", 'a "b" c']), 'set sys_prompt "a \\"b\\" c"')
        self.assertEqual(dev.join_argv(["set", "tts_url", ""]), 'set tts_url ""')

    def test_log_listen_vs_device_command(self):
        self.assertTrue(dev.is_local_listen(["log"]))
        self.assertTrue(dev.is_local_listen(["log", "10"]))
        self.assertTrue(dev.is_local_listen(["events", "2.5"]))
        self.assertFalse(dev.is_local_listen(["log", "*", "warn"]))
        self.assertFalse(dev.is_local_listen(["log", "wifi"]))
        self.assertFalse(dev.is_local_listen(["info"]))

    def test_loads_tolerates_garbage(self):
        self.assertEqual(dev._loads(""), {})
        self.assertEqual(dev._loads("{\"a\":1}"), {"a": 1})
        self.assertEqual(dev._loads("not json")["raw"], "not json")

    def test_final(self):
        self.assertEqual(dev.Device._final("OK", ""), {})
        self.assertIsNone(dev.Device._final("D", "0 AAAA"))
        with self.assertRaises(dev.DevError) as cm:
            dev.Device._final("ERR", "ESP_ERR_INVALID_STATE {\"msg\":\"not ready\"}")
        self.assertEqual((cm.exception.err, cm.exception.msg), ("ESP_ERR_INVALID_STATE", "not ready"))

    def test_png_mask(self):
        d = tempfile.mkdtemp(prefix="devpy_")
        p = os.path.join(d, "m.png")
        dev.rgb565le_to_png(p, 4, 4, b"\xff\xff" * 16, mask=True)
        with open(p, "rb") as f:
            png = f.read()
        raw = zlib.decompress(png[41:41 + struct.unpack(">I", png[33:37])[0]])
        self.assertEqual(tuple(raw[1:4]), (51, 51, 51))       # corner dimmed
        self.assertEqual(tuple(raw[1 + 13 + 3:1 + 13 + 6]), (255, 255, 255))  # row 1, x 1 inside

    def test_json_roundtrip_of_device_escapes(self):
        # what dc_line_json_str() produces for quotes, backslashes, control chars and UTF-8
        line = '@3 OK {"out":["a \\"q\\" \\\\ \\u0009 привет"]}'
        m = dev.FRAME_RE.match(line)
        self.assertEqual(dev.Device._final(m.group(2), m.group(3))["out"], ['a "q" \\ \t привет'])


if __name__ == "__main__":
    unittest.main(verbosity=2)
