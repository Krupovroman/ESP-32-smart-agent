#!/usr/bin/env python3
"""End-to-end smoke test on the real board, no human needed (and no sound: run it with
volume 0 / text_only 1 when people are asleep).

    python tools/smoke.py                 # all checks
    python tools/smoke.py --no-ai         # skip the paid OpenRouter turns
    python tools/smoke.py --port COM7

Checks: protocol + boot self-test, memory gates, UI audit/fit, screenshot, Wi-Fi, key,
assistant turn from an injected Russian utterance, translator turn from an injected English
utterance, and (when the app supports it) that a noise-only clip is not sent to the AI.
Fixtures live in tests/audio (synthetic TTS voices + room noise).
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUDIO = os.path.join(ROOT, "tests", "audio")
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" - {detail}" if detail else ""))
    return ok


def out_text(res):
    return "\n".join(res.get("out", [])) if isinstance(res, dict) else str(res)


def try_cmd(d, line, timeout=30.0):
    try:
        return d.cmd(line, timeout), None
    except dev.DevError as e:
        return None, f"{e.err}: {e.msg}"


def voice_turn(d, wav, command):
    d.mic_load(os.path.join(AUDIO, wav))
    d.cmd("mic src inject fast")
    d.cmd("mic rec 8000", 20)
    try:
        return d.cmd(command, 90), None
    except dev.DevError as e:
        return None, f"{e.err}: {e.msg}"
    finally:
        d.cmd("mic src mic")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("RA_PORT", "COM7"))
    ap.add_argument("--no-ai", action="store_true")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    d = dev.Device(a.port)
    check("protocol v2 + CRC self-check", d.check_protocol())

    info = d.cmd("info")
    check("boot self-test ok", info.get("selftest") == "ok" and not info.get("safe_mode"),
          f"selftest={info.get('selftest')} reason={info.get('selftest_reason')}")
    check("panel batch detected", info.get("panel_batch") in (1, 2), f"id={info.get('panel_id')}")
    heap = info.get("int", {})
    check("internal heap >= 40 KB free", heap.get("free", 0) >= 40 * 1024, f"free={heap.get('free')} min={heap.get('min')}")
    check("internal largest block >= 16 KB", heap.get("largest", 0) >= 16 * 1024, f"largest={heap.get('largest')}")
    check("PSRAM >= 3 MB free", info.get("psram", {}).get("free", 0) >= 3 * 1024 * 1024)

    res, err = try_cmd(d, "ui audit")
    if err is None:
        check("UI audit: no geometry violations", "violations=0" in out_text(res), out_text(res)[:120])
        res, _ = try_cmd(d, "ui fit")
        check("UI fit: no text overflows", res is not None and "overflows=0" in out_text(res), out_text(res)[:120])
    try:
        d.shot(os.path.join(ROOT, "shots", "smoke_home.png"))
        check("screenshot", True, "shots/smoke_home.png")
    except dev.DevError as e:
        check("screenshot", False, str(e))

    up = False
    for _ in range(20):
        res, _ = try_cmd(d, "wifi status")
        if res is not None and "state=up" in out_text(res):
            up = True
            break
        time.sleep(1)
    check("Wi-Fi up", up)

    if not a.no_ai and up:
        res, err = try_cmd(d, "ai key", 30)
        check("AI key accepted via relay", err is None and "remaining=" in out_text(res), err or "")

        res, err = voice_turn(d, "q_ru.wav", "ai voice")
        text = out_text(res) if res else ""
        check("assistant turn from injected RU speech", err is None and any(k in text.lower() for k in ("двест", "двумст", "200")),
              err or next((l for l in text.splitlines() if l.startswith("reply:")), "")[:140])

        saved = {k: out_text(d.cmd(f"get {k}")).split("=", 1)[-1].strip() for k in ("tr_lang_a", "tr_lang_b")}
        d.cmd("set tr_lang_a ru")
        d.cmd("set tr_lang_b en")
        res, err = voice_turn(d, "q_en.wav", "ai trvoice")
        for k, v in saved.items():       # put the user's pair back
            d.cmd(f"set {k} {v}")
        text = out_text(res) if res else ""
        check("translator turn EN -> RU", err is None and any(k in text.lower() for k in ("вокзал", "станци")),
              err or next((l for l in text.splitlines() if l.startswith("translation:")), "")[:140])

        res, err = try_cmd(d, "help")
        if res is not None and "ptt" in out_text(res):
            d.mic_load(os.path.join(AUDIO, "noise.wav"))
            d.cmd("mic src inject")
            d.cmd("ptt down")
            time.sleep(2.5)
            d.cmd("ptt up")
            time.sleep(1.5)
            st, _ = try_cmd(d, "app state")
            d.cmd("mic src mic")
            gate = (st or {}).get("gate", {})
            check("noise-only clip is not sent to the AI", gate.get("verdict") in ("no_speech", "too_short", "silent"),
                  f"gate={gate}")  # ai.* holds the previous turn: the gate verdict alone says nothing was sent

    info2 = d.cmd("info")
    check("internal heap min-ever >= 20 KB after the run", info2.get("int", {}).get("min", 0) >= 20 * 1024,
          f"min={info2.get('int', {}).get('min')}")
    d.close()

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
