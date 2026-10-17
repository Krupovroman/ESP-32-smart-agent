#!/usr/bin/env python3
"""PC-side check of the protocol in docs/protocol.md against the relay.

    set OPENROUTER_KEY=sk-or-...        (never commit it)
    python tools/ai_test.py chat shots/voice.wav
    python tools/ai_test.py tts "Привет! Это проверка голоса." shots/tts.wav

Base URL comes from secrets.json ("ai_url") or --base.
"""
import argparse
import base64
import json
import os
import struct
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SYSTEM_RU = """Ты — голосовой ассистент в маленьком круглом устройстве. Твой ответ покажут на крошечном экране и прочитают вслух синтезатором речи.

На входе — запись голоса пользователя или набранный им текст, а также, возможно, несколько предыдущих реплик разговора.

Заполни поля:
- transcript — дословно, что сказал пользователь, на языке оригинала, с пунктуацией. Если пользователь набрал текст, перепиши его без изменений. Если разборчивой речи нет, оставь transcript и reply пустыми.
- reply — твой ответ.
- lang — код языка ответа.

Как отвечать:
- На языке пользователя; если язык неясен — по-русски.
- Коротко: одно–три предложения, до 300 символов. Подробнее — только если об этом прямо попросили, и не длиннее 800 символов.
- Только простой разговорный текст для чтения вслух: без markdown, списков, заголовков, таблиц, ссылок, кода, эмодзи и спецсимволов.
- Числа, даты, время, проценты, деньги и единицы измерения пиши словами, как их произносят.
- Не используй сокращения вроде «т. е.», «г.», «руб.» — пиши слова полностью. Латиницу — только в именах и названиях.
- У тебя нет доступа к интернету: если спрашивают о свежих новостях, погоде или курсах, скажи об этом одной фразой.
- Если просьба неясна, переспроси одной короткой фразой."""

SCHEMA = {
    "type": "object",
    "properties": {
        "transcript": {"type": "string"},
        "reply": {"type": "string"},
        "lang": {"type": "string", "enum": ["ru", "en", "de", "fr", "es", "it", "pt", "pl", "tr", "uk", "zh", "ja",
                                             "ko", "ar", "other"]},
    },
    "required": ["transcript", "reply", "lang"],
    "additionalProperties": False,
}


def base_url(arg):
    if arg:
        return arg.rstrip("/")
    with open(os.path.join(ROOT, "secrets.json"), encoding="utf-8") as f:
        return json.load(f)["ai_url"].rstrip("/")


def post(url, key, body, accept="application/json"):
    data = json.dumps(body, ensure_ascii=False).encode()
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Authorization": f"Bearer {key}", "Content-Type": "application/json", "Accept": accept,
        "X-OpenRouter-Title": "Round Assistant", "HTTP-Referer": "https://github.com/round-assistant"})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, dict(r.headers), r.read(), time.time() - t0
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read(), time.time() - t0


def chat(args, key):
    wav = open(args.wav, "rb").read()
    body = {
        "models": [args.model, "google/gemini-3.5-flash-lite"],
        "messages": [
            {"role": "system", "content": SYSTEM_RU},
            {"role": "user", "content": [
                {"type": "text", "text": "Голосовое сообщение пользователя."},
                {"type": "input_audio", "input_audio": {"data": base64.b64encode(wav).decode(), "format": "wav"}},
            ]},
        ],
        "response_format": {"type": "json_schema",
                            "json_schema": {"name": "assistant_turn", "strict": True, "schema": SCHEMA}},
        "reasoning": {"effort": "minimal", "exclude": True},
        "max_tokens": 1024,
        "stream": False,
        "provider": {"require_parameters": True},
    }
    st, hdr, raw, dt = post(base_url(args.base) + "/chat/completions", key, body)
    print(f"status {st} in {dt:.2f}s, upload {len(wav) * 4 // 3 // 1024} KB")
    j = json.loads(raw)
    if st != 200:
        print(json.dumps(j, ensure_ascii=False)[:600])
        return
    msg = j["choices"][0]["message"]["content"]
    print("model:", j.get("model"), "| provider:", j.get("provider"), "| finish:", j["choices"][0]["finish_reason"])
    print("usage:", {k: j["usage"].get(k) for k in ("prompt_tokens", "completion_tokens", "cost")})
    out = json.loads(msg)
    print("transcript:", out["transcript"])
    print("reply:     ", out["reply"])
    print("lang:      ", out["lang"])


def tts(args, key):
    body = {"model": args.tts_model, "input": args.text, "voice": args.voice, "response_format": "pcm"}
    st, hdr, raw, dt = post(base_url(args.base) + "/audio/speech", key, body, accept="audio/pcm")
    ctype = hdr.get("Content-Type") or hdr.get("content-type")
    print(f"status {st} in {dt:.2f}s, {len(raw)} bytes, Content-Type: {ctype}")
    if st != 200:
        print(raw[:400].decode("utf-8", "replace"))
        return
    rate = 24000
    for part in (ctype or "").split(";"):
        if part.strip().startswith("rate="):
            rate = int(part.split("=")[1])
    hdr_wav = b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate,
                                                                                       rate * 2, 2, 16)
    with open(args.out, "wb") as f:
        f.write(hdr_wav + b"data" + struct.pack("<I", len(raw)) + raw)
    print(f"saved {args.out}: {len(raw) / 2 / rate:.1f}s at {rate} Hz")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("chat")
    c.add_argument("wav")
    c.add_argument("--model", default="google/gemini-3.1-flash-lite")
    t = sub.add_parser("tts")
    t.add_argument("text")
    t.add_argument("out")
    t.add_argument("--tts-model", default="microsoft/mai-voice-2.1-flash")
    t.add_argument("--voice", default="ru-RU-Masha:MAI-Voice-2.1-Flash")
    args = ap.parse_args()
    key = os.environ.get("OPENROUTER_KEY")
    if not key:
        sys.exit("set OPENROUTER_KEY")
    sys.stdout.reconfigure(encoding="utf-8")
    chat(args, key) if args.cmd == "chat" else tts(args, key)


if __name__ == "__main__":
    main()
