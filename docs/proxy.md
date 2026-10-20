# OpenRouter relay

openrouter.ai answers `403 Access denied by security policy` to requests from Russian IP
addresses (checked 2026-10-17: the device's egress is RU, the key itself is valid). In that case the
device can talk to OpenRouter through a small HTTPS relay on any VPS outside the blocked region. The setup
below is one working example.

| | |
|---|---|
| Server | any small VPS outside the blocked region (here Ubuntu 22.04) |
| Relay | Caddy 2.11 (official apt repo), `/etc/caddy/Caddyfile` |
| Hostname | your own domain, or `<server-ip>.sslip.io` (sslip.io wildcard DNS → the IP); Let's Encrypt cert, auto-renewed by Caddy |
| Device base URL | `https://<relay-host>/<secret>/api/v1` — set as `ai_url` (and `tts_url`/`stt_url` if they differ) |
| Secret | random 24-char path prefix, stored only in the local `secrets.json` (gitignored) and on the device |

Behaviour:

- Only `/<secret>/*` is forwarded (prefix stripped) to `https://openrouter.ai`; anything else gets 404.
- `X-Forwarded-For` / `X-Real-Ip` / `X-Forwarded-*` are removed so the device's RU address is not passed on.
- `flush_interval -1` so SSE tokens and TTS audio chunks stream through without buffering.
- No access log is configured, so neither the secret path nor the `Authorization` header is written to disk.
  The API key stays on the device; the relay never stores it.
- HTTP/3 is disabled (`protocols h1 h2`); turn it back on if UDP 443 is free on your host.

Maintenance needs only ordinary SSH access to the server; the device never uses it.

Rotate the secret: generate a new one in `secrets.json`, update the `handle_path` line in the Caddyfile,
`systemctl reload caddy`, then `python tools/dev.py set ai_url <new url>`.

Check from the PC: `curl -s -o /dev/null -w "%{http_code}" "<ai_url>/models"` → `200`.
