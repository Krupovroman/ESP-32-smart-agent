#!/usr/bin/env python3
"""Push Wi-Fi networks and keys from a local JSON file to the board (after a factory restore,
an NVS erase or "settings reset"). Values are never printed.

    python tools/secrets_push.py secrets.json [--port COM7] [--no-wifi]

secrets.json (gitignored):
    {"wifi": [{"ssid": "home", "pass": "..."}],
     "ai_key": "sk-or-v1-...", "ai_url": "https://<relay-host>/<secret>/api/v1",
     "tts_key": "", "stt_key": ""}
Every top-level key except "wifi" is a setting name (see "dev.py settings").
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev  # noqa: E402


def _safe(e, secrets):
    """Error text without any of the secret values."""
    msg = e.msg or ""
    if any(s and s in msg for s in secrets):
        msg = "(message withheld)"
    return f"{e.err} {msg}".strip()


def push(d, data, wifi=True, say=print):
    """Sends the settings and networks of data (a dict) through Device d; returns the failures."""
    secrets = [str(v) for k, v in data.items() if k != "wifi"]
    secrets += [str(n.get("pass", "")) for n in data.get("wifi", [])]
    fails = 0
    for key, val in data.items():
        if key == "wifi":
            continue
        if not isinstance(val, (str, int)):
            say(f"skip {key}: not a string or number")
            fails += 1
            continue
        try:
            d.cmd(dev.join_argv(["set", key, str(val)]), timeout=10.0)
            say(f"set {key}: OK ({len(str(val))} chars)")
        except dev.DevError as e:
            say(f"set {key}: {_safe(e, secrets)}")
            fails += 1
    for net in data.get("wifi", []) if wifi else []:
        ssid = net.get("ssid", "")
        if not ssid:
            continue
        try:
            d.cmd(dev.join_argv(["wifi", "connect", ssid, net.get("pass", "")]), timeout=40.0)
            say(f"wifi {ssid}: saved and connected")
        except dev.DevError as e:
            say(f"wifi {ssid}: {_safe(e, secrets)}")
            fails += 1
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--port", default=os.environ.get("RA_PORT", "COM7"))
    ap.add_argument("--no-wifi", action="store_true", help="only the settings, no wifi connect")
    a = ap.parse_args()
    with open(a.file, encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        print("error: the file must hold a JSON object", file=sys.stderr)
        return 2
    try:
        d = dev.Device(a.port)   # device log lines are not printed
    except (dev.DevError, OSError) as e:
        print(f"error: cannot talk to {a.port}: {e}", file=sys.stderr)
        return 2
    try:
        d.wait_ready(15.0)
        return 1 if push(d, data, wifi=not a.no_wifi) else 0
    finally:
        d.close()


if __name__ == "__main__":
    sys.exit(main())
