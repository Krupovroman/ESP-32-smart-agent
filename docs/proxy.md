# OpenRouter relay

openrouter.ai answers `403 Access denied by security policy` to requests from Russian IP
addresses (checked 2026-10-17: the device's egress is RU, the key itself is valid). The device
therefore talks to OpenRouter through a small HTTPS relay on the user's server in Germany.

| | |
|---|---|
| Server | `SERVER_IP`, Ubuntu 22.04 |
| Relay | Caddy 2.11 (official apt repo), `/etc/caddy/Caddyfile` |
| Hostname | `server-ip.sslip.io` (sslip.io wildcard DNS → the IP), Let's Encrypt cert, auto-renewed by Caddy |
| Device base URL | `https://server-ip.sslip.io/<secret>/api/v1` — set as `ai_url` (and `tts_url`/`stt_url` if they differ) |
| Secret | random 24-char path prefix, stored only in the local `secrets.json` (gitignored) and on the device |

Behaviour:

- Only `/<secret>/*` is forwarded (prefix stripped) to `https://openrouter.ai`; anything else gets 404.
- `X-Forwarded-For` / `X-Real-Ip` / `X-Forwarded-*` are removed so the device's RU address is not passed on.
- `flush_interval -1` so SSE tokens and TTS audio chunks stream through without buffering.
- No access log is configured, so neither the secret path nor the `Authorization` header is written to disk.
  The API key stays on the device; the relay never stores it.
- HTTP/3 is disabled (`protocols h1 h2`) because UDP 443 on this host is used by a VPN.

Admin access for maintenance: SSH key `~/.ssh/round_assistant_proxy` (comment `claude-round-assistant-proxy`)
in `/root/.ssh/authorized_keys`. Remove that line to revoke it.

Rotate the secret: generate a new one in `secrets.json`, update the `handle_path` line in the Caddyfile,
`systemctl reload caddy`, then `python tools/dev.py set ai_url <new url>`.

Check from the PC: `curl -s -o /dev/null -w "%{http_code}" "<ai_url>/models"` → `200`.
