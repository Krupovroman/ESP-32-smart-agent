# OpenRouter network protocol spec: firmware v1 (ESP32-S3 Round Assistant)

**Status:** specification. Verified on **2026-10-17** against:

- live public endpoints: `GET /api/v1/models` with query params, `GET /api/v1/model/{author}/{slug}` and `GET /api/v1/models/{author}/{slug}/endpoints`;
- POST probes sent without a key, which all returned a 401 JSON error;
- the official OpenAPI spec `https://openrouter.ai/openapi.json` (v1.0.0);
- these docs pages: `api_reference/streaming`, `errors-and-debugging`, `limits`, `guides/features/structured-outputs`, `service-tiers`, `best-practices/reasoning-tokens`, `latency-and-performance`, `multimodal/{audio,tts,stt}`, `routing/{provider-selection,model-fallbacks}`, `app-attribution`;
- the cookbooks `audio/two-voice-podcast` and `audio/push-to-talk-button`;
- the ESP-IDF v6.1 `esp_http_client.h` and `esp_tls_errors.h` headers on GitHub raw.

No API key was available. Items that can only be confirmed with a key are marked **[needs key]**, and §11 lists them.

Corrections from `stack_2026-10-17.json` `verify:*` are already applied:

- the reasoning effort is `minimal`, not `none`;
- `/key` `limit_remaining` is a per-key cap, not the account balance;
- the ESP-IDF CMN cert bundle is enough;
- the PCM rate must be taken from `Content-Type`;
- picker filters need `-preview` and `structured_outputs` rules.

---

## 0. Decisions (v1)

| Item | Decision |
|---|---|
| Chat model | `google/gemini-3.1-flash-lite`. Checked live: GA, canonical slug `google/gemini-3.1-flash-lite-20260507`. Input modalities: text, image, video, file, audio. `supported_parameters` include `response_format`, `structured_outputs`, `reasoning`, `max_tokens`. `reasoning.supported_efforts` = high, medium, low, minimal, none (default `minimal`, `mandatory:false`). `expiration_date: null`. It has 8 endpoints, all listing `response_format`. |
| Chat fallback | `google/gemini-3.5-flash-lite` (`reasoning.mandatory:true`, efforts down to `minimal`), sent through the `models` array. |
| Reasoning | `{"effort":"minimal","exclude":true}`. Google: "`minimal` does not guarantee that thinking is off", and `none` is rejected by the 3.5 fallback. |
| Output | `response_format` = `json_schema` with `strict:true`, plus `provider.require_parameters:true`. |
| Temperature | Omitted. Google strongly recommends the default 1.0 for Gemini 3 and warns that lower values can cause looping. |
| Max tokens | `max_tokens` 1024 for the assistant, 768 for the translator. The OpenAPI spec marks `max_tokens` as deprecated in favour of `max_completion_tokens`, but `max_tokens` is the name listed in every model's `supported_parameters`, so keep it while `require_parameters` is on. |
| Streaming | **Non-streaming in v1** for chat and STT (reasons in §2.6). The TTS body is always read progressively. The SSE parser in §2.7 is for v1.1. |
| TTS | `microsoft/mai-voice-2.1-flash` with `response_format:"pcm"` and one voice per language (§5.5). For languages MAI lacks: `google/gemini-3.8-flash-lite-tts` for ja, ar and other; `elevenlabs/eleven-flash-v2.5` for uk; `x-ai/grok-voice-tts-1.0` when Google is unavailable. |
| STT fallback | `openai/whisper-large-v3-turbo` via JSON `/audio/transcriptions`. |
| Web search | OpenRouter's `web` plugin, `max_results` 3 (§2.8). Default **auto**: the assistant schema has `needs_web`; when the model sets it, the transcript goes again as one text request with the plugin. About $0.014 per searched turn against $0.00012 for a normal one, so it is never on for every turn by default. |
| Transport | HTTP/1.1. One `esp_http_client` handle per host, kept alive. ESP-IDF CMN cert bundle. Request body streamed with an exact `Content-Length`. |

---

## 1. Transport rules (all endpoints)

### 1.1 Endpoints (base `https://openrouter.ai/api/v1`)

| Use | Method + path | Request | Success response |
|---|---|---|---|
| Assistant / translator | `POST /chat/completions` | JSON with base64 WAV | `200 application/json` |
| TTS | `POST /audio/speech` | JSON | `200 audio/pcm;rate=…;channels=…`, raw s16le |
| STT fallback | `POST /audio/transcriptions` | JSON with base64 WAV | `200 application/json` `{text, usage}` |
| Model lists | `GET /models?...` | none | `200` `{data[], total_count, links.next}` |
| Single-model check | `GET /model/{author}/{slug}` (singular `model`) | none | `200` `{data:{...}}`, about 1.6 KB for gemini-3.1-flash-lite and 4.3 KB for MAI. Unknown id: `404 {"error":{"message":"Model not found: google/does-not-exist","code":404}}` |
| Key / limits | `GET /key` | none | `200` `{data:{...}}`, about 1 KB |

Live probes:

- POST without a key: `401 {"error":{"message":"No cookie auth credentials found","code":401}}`.
- Invalid key: `401 {"error":{"message":"User not found.","code":401}}`.
- `GET /models` without a key: 200. Send the key anyway, because the spec declares bearer auth.

### 1.2 Request headers

```
Authorization: Bearer sk-or-v1-<key>               every request, GETs included
Content-Type: application/json                     POST only
Accept: application/json                           chat (non-stream), STT, GETs
Accept: text/event-stream                          chat with "stream":true (v1.1)
                                                   TTS: no Accept header
User-Agent: RoundAssistant/1.0 (ESP32-S3; ESP-IDF 6.1)
X-OpenRouter-Title: ESP32 Round Assistant          ASCII only
HTTP-Referer: https://github.com/<owner>/esp32-ai-assistant    optional, Kconfig; empty = not sent
X-OpenRouter-App-Visibility: hidden                send only together with HTTP-Referer
```

Header rules:

- `HTTP-Referer` is the only header that creates an app page and appears in rankings. `X-OpenRouter-Title` alone does nothing. `X-Title` is the legacy alias; don't send both.
- `hidden` only takes effect on the request that first creates the app.
- Send the attribution headers only when the host is `openrouter.ai`. Strip them for custom OpenAI-compatible base URLs.
- **Never send `Accept-Encoding`**: `esp_http_client` cannot gunzip. Checked: without it, Cloudflare answered the 96 KB `/models` response with no `Content-Encoding`, `Transfer-Encoding: chunked` and `Connection: keep-alive`.
- Ignore the `Set-Cookie: __cf_bm=…` header (about 430 bytes).
- Never log the key. Mask it as `sk-or-v1-…abcd`.

### 1.3 `esp_http_client` configuration (field names checked in IDF v6.1)

```c
esp_http_client_config_t cfg = {
    .url = "https://openrouter.ai/api/v1/chat/completions",
    .crt_bundle_attach = esp_crt_bundle_attach,   // CMN bundle contains GTS Root R4 (openrouter.ai: leaf <- WE1 <- GTS Root R4)
    .timeout_ms = 10000,                          // connect + TLS; changed per phase (see §9.2)
    .buffer_size = 4096,                          // RX
    .buffer_size_tx = 2048,                       // TX; STRICT_HEADER_BUFFER=y rejects a header longer than this
    .user_agent = "RoundAssistant/1.0 (ESP32-S3; ESP-IDF 6.1)",
    .disable_auto_redirect = true,
    .keep_alive_enable = true, .keep_alive_idle = 15, .keep_alive_interval = 5, .keep_alive_count = 3, // TCP keepalive
    .event_handler = or_evt,                      // captures response headers
};
```

sdkconfig:

- `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`
- `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y`
- `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`

cJSON: use `espressif/cjson` and `cJSON_InitHooks()` with `heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`.

**Response headers.** Read them in `or_evt` on `HTTP_EVENT_ON_HEADER` (compare names case-insensitively). Copy:

| Header | Buffer |
|---|---|
| `Content-Type` | 64 B |
| `Retry-After` | 16 B |
| `X-Generation-Id` | 64 B |
| `X-Provider-Name` | 32 B |
| `X-RateLimit-Reset` | 24 B |

Do not use `esp_http_client_get_header()`: it returns *request* headers. `esp_http_client_get_response_header()` needs `CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS` and keeps only the first 10 headers of at most 128 bytes, while Cloudflare sends about 15. Ignore `HTTP_EVENT_ON_DATA` in the handler; body bytes come from `esp_http_client_read()`.

**Connection reuse:**

- Use one handle per host. All OpenRouter calls share one handle, behind a mutex, strictly in sequence: chat, then TTS chunk 1, then TTS chunk 2, and so on.
- After a body has been read to the end (`esp_http_client_read()` returned 0 and `esp_http_client_is_complete_data_received()` is true), **do not** call `esp_http_client_close()`. Call `set_url`, `set_method` and `set_header`, then `esp_http_client_open()` again; the TLS session is reused.
- After any error, timeout or abort, call `esp_http_client_close()`.
- If the first write on a reused connection fails (the server closed an idle socket), reconnect once silently. This does not count as a retry.

**TLS pre-warm:** when the push-to-talk button goes down and there is no live connection, send `GET /api/v1/key` (about 1 KB, no cost). The TLS handshake then overlaps the user's speech, and the chat POST reuses the connection.

### 1.4 Streaming the request body (chat and STT)

The JSON is built with cJSON. A random placeholder string stands in for the base64 data. Print the JSON, split it at the placeholder, then write the prefix, the base64 stream and the suffix under an exact `Content-Length`. This avoids holding the base64 (1.3 MB for 30 s) and a cJSON copy of it in RAM.

```c
char ph[24]; snprintf(ph, sizeof ph, "B64_%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
cJSON_AddStringToObject(input_audio, "data", ph);
cJSON_AddStringToObject(input_audio, "format", "wav");
char *js = cJSON_PrintUnformatted(root);          // cJSON leaves UTF-8 (Cyrillic) unescaped
char *p  = strstr(js, ph);
size_t pre = p - js; const char *suf = p + strlen(ph); size_t suf_len = strlen(suf);
size_t wav_len = 44 + pcm_len;
size_t b64_len = 4 * ((wav_len + 2) / 3);
esp_http_client_set_method(c, HTTP_METHOD_POST);
esp_http_client_open(c, (int)(pre + b64_len + suf_len));   // sends Content-Length
write_all(c, js, pre);
b64_write_stream(c, wav_hdr, 44, pcm, pcm_len);  // 3072 raw bytes -> 4096 chars per write; carry 0-2 bytes across the header/PCM boundary
write_all(c, suf, suf_len);
```

- `write_all` loops on `esp_http_client_write()`. A return of -1 means `ESP_ERR_HTTP_WRITE_DATA`.
- Use a 20-line local base64 encoder (no dependency on the MbedTLS 4 API).
- **WAV header** (44 bytes, little-endian): `"RIFF"`, `36+pcm_len`, `"WAVE"`, `"fmt "`, `16`, `1` (PCM), `1` channel, `16000`, `32000`, `2`, `16`, `"data"`, `pcm_len`.
- **Sizes:** 1 s is 32,000 B of PCM and 42,667 B of base64. 10 s is 426,728 base64 characters.
- **Length cap:** UI hard cap 30 s; default 20 s. Before encoding, trim leading and trailing silence (energy VAD, keeping 200 ms margins).
- Keep the PCM in PSRAM until the turn succeeds, because retries re-stream it.

### 1.5 Reading JSON responses

```c
esp_http_client_set_timeout_ms(c, FIRST_BYTE_MS);
int64_t clen  = esp_http_client_fetch_headers(c);   // <=0 when chunked
int     status = esp_http_client_get_status_code(c);
esp_http_client_set_timeout_ms(c, STALL_MS);
// loop esp_http_client_read() until it returns 0; <0 = error (-ESP_ERR_HTTP_EAGAIN = timeout);
// then require esp_http_client_is_complete_data_received(c)
```

**Body size caps** (allocate in PSRAM):

| Body | Cap |
|---|---|
| Chat response | 64 KB |
| STT response | 16 KB |
| Error body | 16 KB, then drain the rest with `esp_http_client_flush_response()` |
| `/key` | 8 KB |
| One models page | 32 KB |

**Error detection rule (all JSON endpoints).** If the root has an `"error"` object, it is a failure **even with HTTP 200**. OpenRouter commits the 200 status as soon as the provider accepts the request, so a later failure arrives inside the body; per the docs, non-streaming bodies then hold only `error` and no `choices`.

Error shape:

```json
{"error":{"code":429,"message":"Rate limit exceeded",
          "metadata":{"error_type":"rate_limit_exceeded","provider_code":"rate_limited"}},
 "user_id":"user_..."}
```

`metadata` may also contain `limit_source`, `reason`, `remedy_hint`, `scope`, `limit_usd`, `spend_usd`, `resets_at`, `reasons`, `flagged_input`, `provider_name` or `raw`. Branch on `error.code` plus `metadata.error_type` / `limit_source`, never on the message text, with one exception: the region check in §9.1.

---

## 2. Assistant turn: `POST /chat/completions`

### 2.1 Full request body (voice input)

```json
{
  "models": ["google/gemini-3.1-flash-lite", "google/gemini-3.5-flash-lite"],
  "messages": [
    {"role": "system", "content": "<SYSTEM_ASSISTANT_RU from §4.1, {WEB} and {DATETIME} substituted>"},
    {"role": "user", "content": "Сколько будет двадцать пять процентов от восьмисот?"},
    {"role": "assistant", "content": "Двадцать пять процентов от восьмисот — это двести."},
    {"role": "user", "content": [
      {"type": "text", "text": "Голосовое сообщение пользователя."},
      {"type": "input_audio", "input_audio": {"data": "UklGRiTiBABXQVZFZm10IBAAAAABAAEAgD4AAAB9AAACABAAZGF0YQDiBA...", "format": "wav"}}
    ]}
  ],
  "response_format": {
    "type": "json_schema",
    "json_schema": {
      "name": "assistant_turn",
      "strict": true,
      "schema": {
        "type": "object",
        "properties": {
          "transcript": {"type": "string", "description": "Verbatim transcript of the user's latest message in its original language. Empty string if there is no intelligible speech."},
          "needs_web": {"type": "boolean", "description": "true when a correct answer needs current or live information: weather, news, exchange rates, prices, schedules, sports results, today's events, anything after your knowledge cutoff. Not for the current date or time when the system prompt gives them. Then reply is only a very short holding phrase: the device searches the web and asks again."},
          "reply": {"type": "string", "description": "Answer shown on screen and read aloud: plain speakable text, no markdown, no emoji, numbers written as words. Empty string if transcript is empty."},
          "lang": {"type": "string", "enum": ["ru","en","de","fr","es","it","pt","pl","tr","uk","zh","ja","ko","ar","other"], "description": "ISO 639-1 code of the language of reply."}
        },
        "required": ["transcript", "needs_web", "reply", "lang"],
        "additionalProperties": false
      }
    }
  },
  "reasoning": {"effort": "minimal", "exclude": true},
  "max_tokens": 2048,
  "stream": false,
  "provider": {"require_parameters": true}
}
```

The base64 prefix shown is the real start of a 10 s, 16 kHz mono 16-bit WAV (`pcm_len` = 320000). This is the default **auto** web mode (§2.8): with `ai_web` off, on another provider, and on requests that already carry the web plugin, `needs_web` is not in the schema (neither in `properties` nor in `required`). It comes before `reply` so the model decides first and then writes either the answer or the holding phrase.

**Body rules:**

1. **`model` vs `models`.**
   - Fallback enabled (OpenRouter host): send `"models":[primary, fallback]` and **no** `"model"`. This is the documented form. The request is billed at the model actually used, and the response `model` field names it.
   - Fallback disabled: send `"model": primary`.
   - Non-OpenRouter base URL: send only `"model"`, and drop `models`, `provider`, the `reasoning` object and `plugins`. Use `"reasoning_effort":"minimal"` only if that provider documents it.
2. **Choosing the reasoning effort** from the selected model's `/models` entry:
   - `supported_efforts` contains `minimal`: send `minimal`.
   - Otherwise, if it is non-empty: send its last (lowest) element. For example, gemini-3.7-flash and 3.8-flash bottom out at `low`.
   - If `reasoning` is null or `supported_efforts` is null: omit `reasoning`.
   - Never send `none` to a model with `mandatory:true`.
3. **Typed input.** The last user message is a plain string (no audio part), and the schema stays the same. The prompt tells the model to copy typed text into `transcript`.
4. **`provider`.** Do not set `sort`. Default routing load-balances standard-tier endpoints by price. Flex and priority tiers are used only with `:floor`/`:nitro` or `service_tier`. Don't use those for voice: `google-vertex/global/flex` showed status −5 and 25% uptime over the last 30 minutes during this check.
5. **Do not send** `temperature` (assistant; the translator sends `"temperature":0`, §3), `top_k`, `stream_options` (deprecated, no effect), `usage` or `transforms`. With `require_parameters:true`, every parameter you send must be in the endpoint's `supported_parameters`.
6. **Content-Length** is computed as in §1.4. For a 10 s clip the body is about 430 KB plus the prefix (system prompt and history, at most about 16 KB).
7. **Web search** (§2.8, OpenRouter hosts only). Always mode, a forced search (console `ai ask -w`) and the auto-mode re-run add `"plugins":[{"id":"web","max_results":<ai_web_n>}]`. Nothing else in the body changes: `response_format` (strict), `reasoning` and `provider.require_parameters` stay.
8. **`max_tokens`** is max(`ai_maxtok`, the floor of the reply length `ai_len`: 1024 / 2048 / 4096, §4.1). The default (`ai_len` 1, `ai_maxtok` 1024) sends 2048, so a 1200-character answer with its transcript echo is never cut.

### 2.2 Example response (non-streaming)

This example is illustrative and built from the OpenAPI `ChatResult`/`ChatUsage` schema **[needs key]**. The cost is computed at the live prices.

```json
{
  "id": "gen-1792247503-Xk2pQ9",
  "provider": "Google AI Studio",
  "model": "google/gemini-3.1-flash-lite",
  "object": "chat.completion",
  "created": 1792247503,
  "system_fingerprint": null,
  "choices": [{
    "index": 0,
    "logprobs": null,
    "finish_reason": "stop",
    "native_finish_reason": "STOP",
    "message": {
      "role": "assistant",
      "content": "{\"transcript\":\"А сколько это будет в долларах, если курс девяносто два рубля?\",\"reply\":\"Двести рублей — это примерно два доллара и семнадцать центов.\",\"lang\":\"ru\"}",
      "refusal": null,
      "reasoning": null
    }
  }],
  "usage": {
    "prompt_tokens": 842,
    "completion_tokens": 64,
    "total_tokens": 906,
    "cost": 0.000347,
    "is_byok": false,
    "prompt_tokens_details": {"cached_tokens": 0, "audio_tokens": 160},
    "completion_tokens_details": {"reasoning_tokens": 0}
  }
}
```

`prompt_tokens` includes the audio tokens: Gemini counts 32 per second, so 5 s gives 160. The cost breaks down as 160 audio tokens × $0.50/M, plus 682 text tokens × $0.25/M, plus 64 output tokens × $1.50/M, which is about $0.000347.

### 2.3 Parsing rules (non-streaming)

1. `status != 200`: error path (§9), body parsed as in §1.5.
2. The root has `error`: error path using `error.code`.
3. `ch = choices[0]`. Read `finish_reason`:
   - `stop`: OK.
   - `length`: the JSON may be truncated. Salvage `reply` with the extractor in §2.7 rule 7, show it with "(ответ обрезан)", don't store it in history.
   - `content_filter` (or `message.refusal` is non-null): message P5 in §9.1.
   - `error`: error path.
4. `content = message.content`, a string (null means failure). Trim whitespace. If it starts with a code fence, drop the first line and the trailing fence. Take the substring from the first `{` to the last `}`. This is defensive only; strict mode should prevent it.
5. Parse with `cJSON_Parse(content)`. `transcript` and `reply` must be strings. `lang` is normalised to lowercase; anything outside the enum becomes `other`. `needs_web` counts only as JSON `true` (absent = false, §2.8). The first `url_citation` of `message.annotations` gives the source host.
6. `transcript == ""`: show "Не расслышал. Повторите, пожалуйста." No TTS, no history entry.
7. Add `usage.cost` (a double, in USD) to the session and day counters. Log `X-Generation-Id`, `provider` and `model`.
8. If parsing fails, retry once with the same body, then show error P2.

### 2.4 Why non-streaming in v1

- **Little time to save.** Replies were capped at about 300 characters, roughly 100 output tokens. Google's model card gives 363 tok/s for Flash-Lite, so generation is about 0.3 s of the turn. Most of the latency is upload, audio prefill and TTS time-to-first-byte. Streaming would save under 0.5 s of perceived latency.
- **Longer replies (`ai_len`, §4.1) change this.** TTS starts only after the whole JSON has arrived (the first TTS chunk is ≤ 200 characters, so its own time-to-first-byte does not grow). Russian runs at roughly 3–4 characters per token, so the usual length of "Обычно" (600 characters, ~200 tokens) adds about 0.3–0.6 s over the old 300, a 1200-character answer ("Подробно", or "Обычно" asked for details) about 1–2 s, and the 2500-character ceiling about 3–5 s (estimates at 200–360 tok/s, not measured). Streaming (v1.1) is the fix if that becomes noticeable.
- **Simpler.** Non-streaming needs one `cJSON_Parse`. Streaming needs SSE framing plus an incremental partial-JSON string decoder before TTS could start early.
- **Cancelling doesn't save money.** The streaming docs list Google / Google AI Studio under "Not Currently Supported" for stream cancellation, so aborting a stream does not stop billing.
- **Upgrade path (v1.1).** Add `"stream": true` and `Accept: text/event-stream`, then start TTS for the first finished sentence of `reply` while the rest is still streaming. The body is otherwise identical. The docs confirm that structured outputs stream as valid partial JSON.

### 2.5 Body sizes and memory

| Item | Size |
|---|---|
| Upload, 10 s clip | about 430 KB |
| Response | 1–4 KB |
| Peak RAM | PCM (≤ 960 KB in PSRAM) + printed JSON prefix/suffix (≤ 12 KB) + 4 KB I/O buffers + 64 KB response buffer |

### 2.6 Content of the last user message (voice)

The text part `"Голосовое сообщение пользователя."` anchors the audio for the model. Keep it this short.

### 2.7 SSE parsing rules (v1.1, also valid for any OpenAI-compatible stream)

1. **Status first.** If the HTTP status is not 200, the body is a plain JSON error, not SSE; parse it as in §1.5. If it is 200 but `Content-Type` is `application/json`, parse it as a non-streaming body.
2. **Framing.** Split the bytes into lines on `\n` and strip one trailing `\r`. Work on bytes, because UTF-8 sequences can be split across reads. Maximum line length is 32 KB; a longer line is a protocol error, so abort.
3. **Line kinds:**
   - Empty line: end of event. Dispatch the data buffer if it is non-empty, then clear it.
   - Starts with `:`: a comment. OpenRouter sends `: OPENROUTER PROCESSING` as a keep-alive. Never JSON-parse it. Reset the stall timer and keep the "thinking" animation.
   - `data:` prefix: strip one optional leading space and append the value to the data buffer (join several data lines with `\n`).
   - `event:`, `id:`, `retry:` or anything else: ignore.
4. **Dispatching data:**
   - `[DONE]`: end of stream. It is a success only if a terminal `finish_reason` was seen and no error.
   - Otherwise run `cJSON_Parse`. If that fails, log the event and skip it, as the official examples do.
   - **Top-level `error` object.** This is a mid-stream failure. Read `error.code` (an HTTP-like integer), `error.message` and `error.metadata.error_type`. `choices[0].finish_reason` will be `"error"`, and the stream ends after this event. A 200 stream whose first and only event is an error is a failure.
   - `choices[0].delta.content` (string, possibly `""`): append it to the content accumulator (cap 16 KB) and feed the extractor in rule 7. Ignore `delta.reasoning`.
   - `choices[0].finish_reason` non-null: remember it (`stop`, `length`, `content_filter` or `error`). Ignore `native_finish_reason`.
   - **Usage chunk.** It is the last chunk before `[DONE]`, carries `usage` (read `usage.cost`), and has one choice with an empty delta that **repeats** the finish_reason. It is an accounting frame, not a second terminal event.
5. **End of stream:**
   - After `[DONE]`: done.
   - Connection closed without `[DONE]`: accept only if `finish_reason=="stop"` and the accumulated JSON parses. Otherwise it is an incomplete-data error.
   - Stall: no bytes, comments included, for 20 s. Abort with `esp_http_client_close()`.
6. **Validate.** `cJSON_Parse` the accumulated content and apply the same checks as §2.3.
7. **Incremental `reply` extractor** (lets TTS start early). Run a tiny tokenizer over the accumulated content that tracks `depth`, `in_string` and `escape`.
   - When a depth-1 key string equal to `reply` is followed by `:` and `"`, switch to emit mode.
   - In emit mode, decode `\" \\ \/ \b \f \n \r \t` and `\uXXXX` (including surrogate pairs) to UTF-8, and push the decoded bytes into the TTS sentence splitter (§5.4) until the closing unescaped `"`.
   - Keys arrive in schema order (`transcript`, `reply`, `lang`), so the transcript can be shown first.

### 2.8 Web search (OpenRouter `web` plugin)

Without fresh data the model either says it has no internet (the old prompt rule) or invents an answer. Measured on 2026-10-18 through the relay with `google/gemini-3.1-flash-lite`, one typed question:

| Request | Cost | Time | Answer |
|---|---|---|---|
| Normal turn | about $0.00012 | 1.1 s | invented, or "no internet" |
| Same body + `"plugins":[{"id":"web","max_results":3}]` | about $0.014 | 2.0 s | real current data |

The plugin works together with `response_format` `json_schema` `strict:true` and `provider.require_parameters:true`. Its price is mostly the search results (about $0.004 each), so `max_results` stays small.

**Settings.** `ai_web`: 0 off, 1 auto (default), 2 always. `ai_web_n`: `max_results`, 1–5, default 3. UI: P → "Поиск в сети" (ui.md §3.7). Console: `get ai_web`, `set ai_web 2`, `ai ask -w <text>` forces a search on one turn.

**Scope.** Only for OpenRouter and the relays that keep its `/api/v1` path (`or_is_openrouter`). Other providers ignore the setting and get the "no internet" rule. Translator turns never search.

**Request shape per mode:**

| Mode | First request | Second request |
|---|---|---|
| Off (or another provider) | no `needs_web`, no plugin; `{WEB}` = "нет доступа к интернету" rule | never |
| Auto | `needs_web` in the schema (§2.1); `{WEB}` = the `needs_web` rule; no plugin | when the first answer has `needs_web:true` |
| Always, or `ai ask -w` | plugin attached (voice included); no `needs_web`; `{WEB}` = "results attached" rule | never |

**Auto-mode re-run:**

1. **Trigger.** The first answer parsed (§2.3), `needs_web` is `true`, it is not truncated (P3), and the transcript is not empty. The model's `reply` is then only a holding phrase ("Сейчас посмотрю.").
2. The ai task emits `AI_EV_WEB` (i = 1). The status line shows "Ищу в интернете…" (ui.md §5.3), and the app's turn deadline gets 35 s from this moment on top of its 75 s. A turn cancelled by then (barge-in, PWR during the first request) sends no search: it would only be paid for.
3. **Body.** Same `models`/`model`, `reasoning`, `max_tokens`, `provider` and history as the first request. The system prompt has the "results attached" `{WEB}` line. The last user message is the transcript as a plain string: the typed text, the Whisper text (§6), or the model's `transcript` of the clip. **The clip is not uploaded again.** The schema has no `needs_web`. The plugin is attached with `max_results` = `ai_web_n`.
4. **One attempt**, with the "web" timeouts of §9.2 (30 s in total). No retries: a failure falls back at once.
5. **Success** (a reply that parses and is not empty): the reply, `lang`, `finish_reason`, `model`, `provider` and generation id come from the re-run; the transcript stays the first one. `usage.cost` and the token counts of **both** requests are added to the turn and the session. The history stores the transcript with the web reply.
6. **Failure** (any transport or HTTP error, a 200 with `error`, P2, an empty reply): the first answer is used. Because it is only the holding phrase, which alone would promise an answer that never comes, the sentence "Не получилось найти это в интернете, попробуйте ещё раз." follows it ("I couldn't search the web just now, please try again." for a reply in any other language), with a full stop added after the holding phrase when it has none. That reply is shown and spoken as it is (no separate note) and not stored in the history. No error card. The cost of a re-run that was billed but did not parse is still counted. Only when the first request fails too is there an error, through the normal §9 path. A cancel (barge-in, PWR) during the re-run ends the turn silently (AB); the requests already made still go into the session cost.

**Always mode** sends the plugin on the normal request, retried by the normal policy (§9.1). If that request gets a 400 or 404 whose message mentions `plugin` or `web search`, it is sent again once without the plugin and with the "no internet" rule. This is an automatic fix like the reasoning and schema fixes, not a retry. The ai task emits `AI_EV_WEB` with i = 0 before the first attempt.

**Response.** The plugin adds `choices[0].message.annotations[]`:

```json
{"type": "url_citation", "url_citation": {"url": "https://www.gismeteo.ru/weather-moscow-4368/now/", "title": "…", "content": "…", "start_index": 0, "end_index": 0}}
```

- The host of the first `url_citation` (lowercase, without `www.` and the port) is the source hint: `ai_result_t.source`, shown under the reply in `ui_16` `text-3` as "Источник: gismeteo.ru". It is never spoken.
- The plugin's own search prompt asks for markdown citations (`[gismeteo.ru](https://…)`). On web replies the device removes them before the text is shown, spoken or stored: a link whose label looks like a host or is a number (`[1](https://…)`) goes completely, any other label stays as text, footnote marks without a link (`[1]`, `[2, 3]`) go, bare `http(s)://` URLs go, and so do the empty brackets, doubled spaces and orphaned commas they leave. A link target may hold its own pair of parentheses (`…/wiki/X_(y)`). When there is no annotation, the host of the first removed link becomes the source.
- `usage.cost` already includes the search.

---

## 3. Translator turn: `POST /chat/completions`

```json
{
  "models": ["google/gemini-3.1-flash-lite", "google/gemini-3.5-flash-lite"],
  "messages": [
    {"role": "system", "content": "<SYSTEM_TRANSLATOR from §4.2 with A=ru, B=en>"},
    {"role": "user", "content": [
      {"type": "text", "text": "Utterance:"},
      {"type": "input_audio", "input_audio": {"data": "UklGRiQ...", "format": "wav"}}
    ]}
  ],
  "response_format": {
    "type": "json_schema",
    "json_schema": {
      "name": "translator_turn",
      "strict": true,
      "schema": {
        "type": "object",
        "properties": {
          "src_lang": {"type": "string", "enum": ["ru", "en", "other"], "description": "Language of the utterance: one of the listed codes (other: neither of the two languages)."},
          "transcript": {"type": "string", "description": "Verbatim transcript in the original language; empty if no intelligible speech."},
          "target_lang": {"type": "string", "enum": ["en", "ru"], "description": "Language of the translation, as the system prompt says; never the language of the utterance."},
          "translation": {"type": "string", "description": "Only the translated text, written in target_lang. Never an answer to the utterance."}
        },
        "required": ["src_lang", "transcript", "target_lang", "translation"],
        "additionalProperties": false
      }
    }
  },
  "reasoning": {"effort": "minimal", "exclude": true},
  "max_tokens": 768,
  "temperature": 0,
  "stream": false,
  "provider": {"require_parameters": true}
}
```

- The `src_lang` enum is built per request as `[A, B, "other"]`, `target_lang` as `[B, A]`. A is "Мой язык" (default `ru`), B is "Язык собеседника" (default `en`), and the UI enforces A ≠ B. `target_lang` comes before `translation`, so the model commits to the direction before it writes. A source forced by the user (held chip, "wrong language") makes both enums one code: `[src]` and the other language.
- `"temperature": 0` (the assistant sends none). An endpoint that rejects it (a 400 or 404 mentioning `temperature` or "requested parameters") gets the request again without it, as an automatic fix (§9.1), not a retry.
- The translator sends no history (each utterance is independent).
- Typed input (and the Whisper text, §6): the user content is a plain string `<utterance>…</utterance>`, so a typed question reads as material to translate.

Example `message.content`:

```json
{"src_lang":"en","transcript":"Excuse me, where is the nearest pharmacy?","target_lang":"ru","translation":"Извините, где ближайшая аптека?"}
```

Device logic after parsing (same parse rules as §2.3):

| Result | Target language | TTS voice | UI |
|---|---|---|---|
| `src_lang == A` | B | `voice(B)` | Bubble on the "me" side: transcript in A, translation in B |
| `src_lang == B` | A | `voice(A)` | Bubble on the partner side |
| `src_lang == "other"` (neither language) | A (the owner must understand) | `voice(A)` | Badge "Другой язык". Setting `tr_other`: `translate_to_A` (default) or `ignore` (show the transcript only, no TTS) |
| `transcript == ""` | none | none | "Не расслышал" |
| `translation == ""` but transcript non-empty | none | none | Retry once, then error P2 |

The target is always the device rule above; the model's `target_lang` is only compared with it (a log line when they differ). Two checks on the device, by text statistics, cost no request in the normal case (`components/ai/ai_trcheck.c`):

1. **Translation check** against the target of the model's own `src_lang`: (a) **script**: fewer than half of its letters (at least 2) are in the target's scripts (Latin; Cyrillic; Han for zh; Han + kana for ja; hangul for ko; Arabic). A Han, kana or hangul character weighs 2 letters (it is a word or a syllable), and in a non-Latin target a Latin name is not counted: mixed case (iPhone, USB), digits or `@`, or a capital that does not start a sentence ("我在Google工作", "Открой WhatsApp на iPhone"). Japanese of 5+ CJK characters without any kana fails too (a Chinese answer). (b) **copy**: letters and digits, lower-cased, equal those of the transcript, with two or more words and 4+ letters, or 6+ CJK characters. A copy of two or more capitalised words is a name ("New York") and passes, except in a pair with German (nouns are capitalised: "Guten Morgen"); so does a translation without letters (numbers). (c) **loop**: cut at `max_tokens` and longer than twice the utterance plus 40 characters.
2. **The retry's direction.** When the check fails, the source is taken from the script of typed or Whisper text if A and B are written in different scripts (ru-en, ru-zh, zh-ko, en-ja; not en-de, ru-uk or zh-ja) and at least three quarters of its letters (same weights) are in one of them. A clip's transcript is never judged this way: it is the model's echo and may already be translated.

A failed check gets **one direction retry**, in one short attempt (`OR_TO_TRDIR`: 15 s first byte, 25 s total; no backoff retries; skipped once the turn is 40 s old, so it ends before the app's 75 s watchdog):

| Case | Request | Fixed |
|---|---|---|
| user-forced source | transcript as text | source and target as before |
| source by script (typed text) | transcript as text | that source, the other language as `target_lang` |
| copy (same-script pair, or a clip) | text; a clip goes again (the text cannot tell its language) | nothing; prompt rule "A previous answer repeated the utterance unchanged, which is wrong: decide again which of the two languages it is in and translate it into the other one." |
| wrong script | transcript as text | `target_lang` = the target; the source unless `"other"` (then `src_lang` is `[other language, "other"]`) |
| loop | the same request | nothing; sent without `temperature` |

A fixed target adds the prompt rule "Translate this utterance into {TARGET_NAME} only: target_lang is "{T}"." The retry's answer is used whatever it is; its target (the fixed one, else by its `src_lang`) sets `lang` and the TTS voice; the shown transcript is the first one unless the clip went again; both costs are counted. If the retry fails (network, HTTP, P2, timeout), the first translation is shown at once, without an error card. No `AI_EV_RETRY` is emitted; `ai_result_t.tr_retry` and the console line "(direction retry)" tell it happened. `tools/tr_eval.py` runs the same prompt, schema, checks and retry rules against the relay.

---

## 4. Prompts and conversation memory

### 4.1 Assistant system prompt (default, Russian, user-editable)

Stored in NVS key `sys_prompt`, at most 4000 bytes including the NUL; the default is about 2.3 KB in UTF-8. It is written in Russian because the user may edit it.

```
Ты — голосовой ассистент в маленьком круглом устройстве. Твой ответ покажут на крошечном экране и прочитают вслух синтезатором речи.

На входе — запись голоса пользователя или набранный им текст, а также, возможно, несколько предыдущих реплик разговора.

Заполни поля:
- transcript — дословно, что сказал пользователь, на языке оригинала, с пунктуацией. Если пользователь набрал текст, перепиши его без изменений. Если разборчивой речи нет, оставь transcript и reply пустыми.
- reply — твой ответ.
- lang — код языка ответа.

Как отвечать:
- На языке пользователя; если язык неясен — по-русски.
- Длина ответа: {LEN_HOW}, до {LEN} символов. Если прямо просят подробнее — до {LEN_MAX} символов. На простые вопросы (время, да или нет, посчитать) отвечай одной фразой.
- Только простой разговорный текст для чтения вслух: без markdown, списков, заголовков, таблиц, ссылок, кода, эмодзи и спецсимволов.
- Числа, даты, время, проценты, деньги и единицы измерения пиши словами, как их произносят: «двадцать пятого октября», «пятнадцать градусов», «три с половиной километра».
- Не используй сокращения вроде «т. е.», «г.», «руб.» — пиши слова полностью. Латиницу — только в именах и названиях.
{WEB}
- Если просьба неясна, переспроси одной короткой фразой.

Сейчас {DATETIME}.
```

`{DATETIME}` is filled in as, for example, `суббота, 17 октября 2026 года, 17:31 (UTC+3)`. If SNTP is not synced, remove the whole last line.

`{WEB}` is replaced by one line that depends on the web search shape of the request (§2.8). A user prompt (`sys_prompt` / `ai_sys`) gets the same substitution; without `{WEB}` in it nothing is added, and the `needs_web` field description still tells the model what to do.

| Request | `{WEB}` line |
|---|---|
| Off, or another provider | `- У тебя нет доступа к интернету: если спрашивают о свежих новостях, погоде или курсах, скажи об этом одной фразой.` |
| Auto (first request) | `- Если для точного ответа нужны свежие или текущие данные — погода, новости, курсы валют, цены, расписания, результаты матчей, события сегодняшнего дня или то, что могло измениться после даты твоих знаний, — поставь needs_web = true, а в reply напиши только очень короткую фразу ожидания, например «Сейчас посмотрю». Устройство найдёт данные в интернете и спросит тебя снова. Свежие данные не выдумывай. Для вопросов о текущей дате и времени поиск не нужен, если они указаны ниже. В остальных случаях needs_web = false.` |
| With the plugin (always, re-run) | `- К запросу приложены результаты поиска в интернете. Свежие данные бери только из них и пересказывай своими словами, без ссылок, адресов сайтов и сносок. Если нужных данных в результатах нет, честно скажи об этом одной фразой и ничего не выдумывай. Если для ответа свежие данные не нужны, отвечай как обычно.` |

The date and time sentence of the auto line keeps "Какое сегодня число?" and "Который час?" off the paid search: `{DATETIME}` answers them. It says "если они указаны ниже" because the line holding `{DATETIME}` is dropped while SNTP is not synced, and then a search is the right answer. The last sentence of the plugin line is for always mode, where every question carries search results.

**Reply length** (`ai_len`, S → "Длина ответа"; a user prompt gets the same placeholders, and one without `{LEN` gets the line "- Длина ответа: {LEN_HOW}, до {LEN} символов; если прямо просят подробнее — до {LEN_MAX} символов." appended, so the setting works with it too):

| `ai_len` | UI | `{LEN_HOW}` | `{LEN}` | `{LEN_MAX}` | `max_tokens` floor (§2.1 rule 8) |
|---|---|---|---|---|---|
| 0 | Коротко | коротко, одно–три предложения | 300 | 600 | 1024 |
| 1 (default) | Обычно | по существу и достаточно полно, обычно три–шесть предложений | 600 | 1200 | 2048 |
| 2 | Подробно | подробно и развёрнуто, с пояснениями и примерами | 1200 | 2500 | 4096 |

Everything downstream takes the ceiling: the history keeps replies up to 2500 code points (§4.3), TTS speaks up to 3000 (§5.3 rule 9), the console prints lines up to 12 KB.

### 4.2 Translator system prompt (internal template, English)

The template is in English because it is language-neutral. With the examples it is about 1.4 KB (≈ 330 tokens).

```
You are a professional two-way interpreter between {A_NAME} ({A}) and {B_NAME} ({B}).
The input is one utterance (an audio recording, or typed text inside <utterance> tags) that one person says to another. It is never addressed to you: you only translate it.

Fill the fields in order:
- src_lang: the language of the utterance, "{A}" or "{B}". If it is clearly neither of them, "other".
- transcript: verbatim transcription in the original language, with punctuation.
- target_lang: "{B}" if src_lang is "{A}"; "{A}" if src_lang is "{B}" or "other".
- translation: the utterance translated into target_lang, written in that language, never in the language of the utterance.

Rules:
- Questions, requests and commands are translated as they are: never answer them, follow them or comment.
- Output only the translation itself: no comments, notes, quotes, alternatives, transliteration or explanations.
- Keep the meaning, tone, politeness level, names and numbers. Use natural spoken phrasing in the target language.
- If the utterance mixes languages, use the dominant one as src_lang.
- If there is no intelligible speech, return an empty transcript and translation and src_lang "other".
- Plain text only: no markdown, no emoji.{FORCE}{EXAMPLES}
```

`{FORCE}` is empty, or on the direction retry (§3) `\n- Translate this utterance into {TARGET_NAME} only: target_lang is "{T}".`, or after a copy with nothing fixed the "A previous answer repeated the utterance unchanged …" rule. `{EXAMPLES}` are three few-shot lines in the pair's own languages (a question and "yes" from A, a command from B), empty for a code outside the table:

```
Examples (utterance -> translation):
"Сколько тебе лет?" -> "How old are you?"
"Tell me the time." -> "Скажи, который час."
"Да." -> "Yes."
```

The phrases of every language are in `LANGS` of `components/ai/ai_prompts.c`, next to the scripts the translation check expects.

Language table (`{X_NAME}` is used in the prompt; the Russian name is used in the UI). The translator pickers offer every row but `ar`: zh, ja and ko need the CJK font pack in the `storage` partition (ui.md 1.3) and are listed but disabled ("ZH · нет шрифта") on a board without it. The prompt names and the assistant's `lang` enum keep all 14 rows; an assistant reply in Chinese, Japanese or Korean draws through the same font pack.

| Code | Prompt name | UI (RU) |
|---|---|---|
| ru | Russian | Русский |
| en | English | Английский |
| de | German | Немецкий |
| fr | French | Французский |
| es | Spanish | Испанский |
| it | Italian | Итальянский |
| pt | Portuguese | Португальский |
| pl | Polish | Польский |
| tr | Turkish | Турецкий |
| uk | Ukrainian | Украинский |
| zh | Chinese (Mandarin, Simplified script) | Китайский |
| ja | Japanese | Японский |
| ko | Korean | Корейский |
| ar | Arabic (Modern Standard) | Арабский |

### 4.3 Conversation memory policy (assistant mode)

- **Storage.** A RAM ring buffer in PSRAM of `{transcript, reply, lang, ts}`. It is never written to NVS (privacy, flash wear) and is lost on reboot.
- **Selecting history.** Walk newest to oldest and include up to **N = 6** exchanges (12 messages), stopping before the total of `len(transcript)+len(reply)` would exceed **6000 Unicode code points** (two detailed exchanges, `ai_len` 2). Emit them oldest to newest as `user` (string = transcript) and `assistant` (string = reply, plain text, not JSON) messages, between the system message and the new user message.
- **Truncating stored entries.** Transcript to 600 code points, reply to 2500 (the longest reply length), with "…" appended.
- **Not stored.** Failed turns, empty transcripts and `finish_reason:"length"` turns.
- **Reset when:** idle for more than 10 min (setting `hist_idle_min`, 0 = never), the mode is switched, the chat model changes, "Новый разговор" is long-pressed, or there is a 400 with `context_length_exceeded`.
- **Translator mode.** N = 0.
- **Budget.** The worst case is 6000 code points (about 2000 tokens) plus about 450 system tokens, which is under $0.0007 of input per turn at $0.25/M (the old 3000 code points: $0.00035).

---

## 5. TTS: `POST /audio/speech`

### 5.1 Request

```json
{
  "model": "microsoft/mai-voice-2.1-flash",
  "input": "Двести рублей — это примерно два доллара и семнадцать центов.",
  "voice": "ru-RU-Masha:MAI-Voice-2.1-Flash",
  "response_format": "pcm"
}
```

- **`voice`.** Required for MAI. Use the full id including the `:MAI-Voice-2.1-Flash` suffix, copied exactly from `supported_voices`. MAI picks the synthesis language from the voice's locale.
- **`speed`.** Omit it when it equals 1.0. The docs say providers without speed control "either ignore it or return a 400 for a non-default value". If the user sets a speed other than 1.0 (range 0.8–1.25), send `"speed": x`. If a 400 mentions `speed`, resend without it and remember per model (in RAM) not to send it again.
- **`instructions`.** Only Gemini, OpenAI and Fish models honour it. Omit it in v1: it costs tokens, and Gemini 3.8 TTS reads `input` verbatim, so never put stage directions in the text.
- **`provider`.** Routing preferences (`order`, `only`, `ignore`) are **not applied to speech**, so don't send them.
- **Before sending `input`, sanitise it:**
  - strip `* _ # \` ~ > |`;
  - remove emoji (U+1F000–U+1FAFF, U+2600–U+27BF, U+FE0F, U+200D);
  - delete URLs;
  - collapse whitespace, keeping `\n`.

### 5.2 Response

```
HTTP/1.1 200 OK
Content-Type: audio/pcm;rate=24000;channels=1   <- exact form shown in OpenRouter's cookbook (Gemini TTS); MAI is documented as "pcm (24 kHz mono)" [header for MAI: needs key]
Transfer-Encoding: chunked                      <- expected; Content-Length may also appear
X-Generation-Id: gen-...
X-Provider-Name: Azure
<raw signed 16-bit little-endian PCM, interleaved if channels > 1, no header>
```

Errors come back as `application/json` `{"error":{...}}` with status 400, 401, 402, 403, 404, 413, 429, 500, 502, 503, 524 or 529 (OpenAPI). A probe without a key returned `401` JSON.

**Parsing `Content-Type`:**

1. Lowercase the value. The media type is the part before the first `;`.
2. Split the parameters on `;`, trim them, and split each on `=`. Read `rate` (also accept `sample_rate` and `samplerate`) and `channels`.
3. Act on the media type:
   - `audio/pcm` or `audio/l16`: raw PCM.
   - `audio/wav` or `audio/x-wav`, or the body starts with `RIFF....WAVE`: parse the `fmt ` chunk and skip to `data`.
   - `audio/mpeg`: unsupported in v1 (no MP3 decoder). Fail the chunk with error T2.
   - `application/json`: error JSON.

Default rate when `rate` is missing (the `verify:tts-stt` correction: no blind 24 kHz default):

| Model prefix | Rate | Channels |
|---|---|---|
| `microsoft/mai-` | 24000 | 1 |
| `google/gemini-*-tts*` | 24000 | 1 |
| `x-ai/grok-voice` | 24000 | 1 |
| `elevenlabs/` | 24000 (ElevenLabs-on-OpenRouter blog; unverified) | 1 |
| `heygen/` | 44100 (OpenRouter model page: "44.1 kHz mono PCM") | 1 |
| `hexgrad/kokoro` | 24000 | 1 |
| anything else | 24000, with a warning in the log | 1 |

### 5.3 Body handling (chunked, progressive)

1. Run `fetch_headers()` with the TTS first-byte timeout (15 s). Check `status == 200` and that the media type is `audio/pcm`; otherwise read the error JSON.
2. Configure I2S0 (Philips, 16-bit, stereo slots) to the parsed rate. If the rate changed: `i2s_channel_disable` → `i2s_channel_reconfig_std_clock(tx, &(i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(rate))` → `i2s_channel_enable`. The PCM5101 accepts 16–48 kHz without MCLK.
3. Read loop:
   - `esp_http_client_read(c, buf, 4096)`.
   - Keep a 1-byte carry, because a chunk may end in the middle of a sample.
   - Push only whole frames (2 bytes × channels) into a PSRAM ring buffer (FreeRTOS stream buffer, 2 MB, which holds about 43 s at 24 kHz mono).
   - The push blocks with a 2 s timeout while the buffer is full; TCP flow control throttles the server.
   - Stall timeout is 10 s.
4. Player task:
   - Start after **150 ms** of audio is buffered (jitter buffer). Write 20 ms frames (480 samples at 24 kHz), duplicate mono into both slots (the amp is stereo), and apply software volume.
   - On underrun, write zeros.
   - End of turn: the last chunk has been received, the ring buffer is empty, and 100 ms of silence has played.
5. Chunks play back to back with no added silence.
6. **Prefetch.** As soon as chunk *k*'s body has been fully received (download is faster than real time), request chunk *k+1* on the same connection while *k* is still playing.
7. **Barge-in.** If PTT is pressed during playback: stop the player, `esp_http_client_close()`, and clear the chunk queue.
8. **No speaker.** If "Голосовые ответы" is off, never call TTS.
9. Cap TTS at 3000 characters per turn (a detailed reply, `ai_len` 2, is up to 2500); the screen still shows the full text. In the firmware the push does not block inside the stream buffer: a full ring is polled every 10 ms (TCP throttles the server) and the read loop counts that wait as activity, so a 3-4 min answer streams through the 43 s ring without dropping PCM. A later chunk is requested only once at most 20 s of audio is queued, so a slow chunk never waits on the speaker into the 60 s TTS total deadline.

Whether OpenRouter delivers PCM progressively for MAI, or only after synthesis finishes, is not documented **[needs key]**. The loop above works either way.

### 5.4 Sentence-splitting rules (lengths in Unicode code points, never splitting a UTF-8 sequence)

**Boundaries:**

- one or more `\n`;
- a run of `. ! ? …` (including `?!` and `...`) followed by whitespace, end of text, or a closing `» " ' )` and then whitespace;
- CJK `。！？`, with no whitespace needed.

**Not a boundary:**

- a digit on both sides (decimal point);
- a preceding token that is a single letter (initials: "А. С. Пушкин");
- a preceding token from the abbreviation list (case-insensitive):
  - ru: `т.е т.д т.п т.к др пр г гг ул д им см стр руб коп тыс млн млрд н.э`
  - en: `mr mrs ms dr st vs etc e.g i.e no`

**Packing into requests:**

1. **Chunk 1 (low latency).** Emit as soon as the first sentence is complete. If it is shorter than 40 characters and a next sentence exists, append sentences while the total stays at or below **200**.
2. **Later chunks.** Add sentences greedily while the total stays at or below **400**. Emit once the chunk is at least **150**, or at end of text.
3. **A single sentence longer than 400** (or longer than 200 for chunk 1): cut at the last `,` `;` `:` ` — ` or ` – ` that falls between positions 150 and 400, or after a CJK `，` `、` `；` `：` there (CJK text has no spaces). Failing that, cut at the last space before 400, else at 400.
4. Never emit a piece shorter than 20 characters; merge it into the previous one. Drop chunks that are empty or contain only punctuation.

### 5.5 Default voice per language for `microsoft/mai-voice-2.1-flash`

The voices come from the live `supported_voices` list (97 voices, 28 locales):

`cs-CZ, da-DK, de-DE, en-AU, en-GB, en-IN, en-US, es-ES, es-MX, fi-FI, fr-FR, hi-IN, hu-HU, id-ID, it-IT, ko-KR, nb-NO, nl-NL, pl-PL, pt-BR, pt-PT, ro-RO, ru-RU, sv-SE, th-TH, tr-TR, vi-VN, zh-CN`

**There are no uk, ja or ar voices.** Every id carries the suffix `:MAI-Voice-2.1-Flash`. `Grant` and `Harper` exist in most locales (multilingual voices). The F/M column is inferred from the names; verify by ear.

| Lang | Route | Default (F) | Alternative (M) | Other voices in this locale |
|---|---|---|---|---|
| ru | MAI | `ru-RU-Masha:MAI-Voice-2.1-Flash` | `ru-RU-Lev:…` | Grant, Harper |
| en | MAI | `en-US-Harper:…` | `en-US-Ethan:…` | en-US Grant, Iris, Jasper, Olivia, Sage; en-GB Emily, Harry; en-AU Isla; en-IN Dhruv, Priya |
| de | MAI | `de-DE-Mia:…` | `de-DE-Klaus:…` | Grant, Harper |
| fr | MAI | `fr-FR-Soleil:…` | `fr-FR-Marc:…` | Grant, Harper |
| es | MAI | `es-ES-Marta:…` | `es-MX-Alejo:…` | es-MX Valeria, Grant, Harper |
| it | MAI | `it-IT-Rosa:…` | `it-IT-Luca:…` | Grant, Harper |
| pt | MAI | `pt-BR-Luana:…` | `pt-BR-Pedro:…` | pt-BR Caio, Rafael; pt-PT Rui, Grant, Harper |
| pl | MAI | `pl-PL-Harper:…` | `pl-PL-Grant:…` | none |
| tr | MAI | `tr-TR-Elif:…` | `tr-TR-Aydin:…` | Grant, Harper |
| zh | MAI | `zh-CN-Mei:…` | `zh-CN-Wei:…` | Bo, Lan, Grant, Harper |
| ko | MAI | `ko-KR-Haena:…` | `ko-KR-Junho:…` | Grant, Harper |
| ja | **Gemini TTS** | `google/gemini-3.8-flash-lite-tts`, `Kore` | `Puck` | Grok `eve`/`rex` when Google is unavailable |
| ar | **Gemini TTS** | `google/gemini-3.8-flash-lite-tts`, `Kore` | `Charon` | Grok `ara`/`rex` when Google is unavailable |
| uk | **ElevenLabs** | `elevenlabs/eleven-flash-v2.5`, `sarah` | `daniel` | Gemini 3.8 TTS as a last resort (Ukrainian is **not** in Google's supported-language table) |
| other | **Gemini TTS** | `google/gemini-3.8-flash-lite-tts`, `Kore` | `Puck` | Gemini auto-detects 100+ languages |

**Gemini 3.8 Flash-Lite TTS voices** (30; descriptors from Google's page): Zephyr (Bright), Puck (Upbeat), Charon (Informative), Kore (Firm), Fenrir (Excitable), Leda (Youthful), Orus (Firm), Aoede (Breezy), Callirrhoe (Easy-going), Autonoe (Bright), Enceladus (Breathy), Iapetus (Clear), Umbriel (Easy-going), Algieba (Smooth), Despina (Smooth), Erinome (Clear), Algenib (Gravelly), Rasalgethi (Informative), Laomedeia (Upbeat), Achernar (Soft), Alnilam (Firm), Schedar (Even), Gacrux (Mature), Pulcherrima (Forward), Achird (Friendly), Zubenelgenubi (Casual), Vindemiatrix (Gentle), Sadachbia (Lively), Sadaltager (Knowledgeable), Sulafat (Warm).

Gemini TTS facts:

- Any voice speaks any supported language; Google: "The TTS models detect the input language automatically".
- `pcm` only. An `mp3` request returns 400 `Gemini TTS only supports response_format="pcm"`.
- Output is 24 kHz mono.

**Grok voices:** `eve`, `ara`, `rex`, `sal`, `leo`. Language is auto-detected; ja and ar are supported, uk is not.

**ElevenLabs voices:** `george, sarah, adam, alice, bella, bill, brian, callum, charlie, chris, daniel, eric, harry, jessica, laura, liam, lily, matilda, river, roger, will`. Flash v2.5 lists 32 languages, uk and ru included.

### 5.6 Languages MAI does not cover: routing rule

1. **Resolve the route.** Look up the language in the table above, after applying any per-language user override (Settings → Голос → per-language model/voice).
2. **On failure, try the next route.** If the call fails with 400, 403 or 404, move to the next route in this order: Gemini 3.8 Flash-Lite TTS, then Grok, then ElevenLabs. Remember the failure in RAM for 1 h.
   - A 403 whose message contains "region" means Google, OpenAI and Anthropic models are blocked for a Russian billing address. MAI (Microsoft) and Grok remain available in that case (§9.1).
3. **If every route fails,** show the text only, with the toast "Озвучка на этом языке недоступна".
4. **Assistant mode** picks the voice from `lang` in the reply. **Translator mode** picks it from the target language.

---

## 6. STT fallback: `POST /audio/transcriptions`

Used when:

- the selected chat model has no `audio` in `architecture.input_modalities`;
- the chat call returns 404 or 400 because no endpoint accepts audio;
- the user sets "Распознавание: Whisper".

```json
{
  "model": "openai/whisper-large-v3-turbo",
  "input_audio": {"data": "UklGRiQ...", "format": "wav"},
  "language": "ru",
  "temperature": 0
}
```

**Response** (OpenAPI `STTResponse`):

```json
{"text": "Сколько будет двадцать пять процентов от восьмисот?", "usage": {"seconds": 6.4, "cost": 0.0000213}}
```

**Fields and limits:**

- `language`: ISO-639-1, auto-detected if omitted.
  - Assistant mode: send the UI language (`ru`).
  - Translator mode: **omit it**; the chat model then decides `src_lang` from the text.
- `response_format`: `json` by default. `verbose_json` adds `language`, `duration` and `segments`, but only on OpenAI-compatible providers; v1 doesn't need it.
- `input_audio.format` `"pcm"` means headerless s16le, 16 kHz mono. v1 still sends `wav`.
- Provider routing preferences are **not applied** to transcription, so you cannot force the cheaper provider. Live endpoint prices: `deepinfra/us` $0.00000333/s and `groq` $0.0000111/s.
- Upstream providers time out after about 60 s (irrelevant for clips of 30 s or less).
- Same-price alternatives: `nvidia/nemotron-3.5-asr-streaming-multilingual-0.6b`, `qwen/qwen3-asr-0.6b`.

**Next step:** send `text` to `/chat/completions` as a plain-string user message, with no `input_audio` and the same schema and prompt. Show the Whisper text as the transcript and ignore the model's echo. The pipeline then has three round trips (STT, then chat, then TTS).

---

## 7. Model pickers

### 7.1 Chat models that accept audio and support structured output

**Query** (paginated so each page stays small):

```
GET https://openrouter.ai/api/v1/models?input_modalities=audio&output_modalities=text&sort=pricing-low-to-high&max_price=1&limit=10&offset=0
-> data[10], total_count 34, links.next "/api/v1/models?...&offset=10&limit=10"
```

- `links.next` starts with `/api/v1`, so join it with `https://openrouter.ai`, not with the base URL.
- **Sizes measured on 2026-10-17:**

  | Query | Size |
  |---|---|
  | One page of 10 | 16–21 KB |
  | All 34 entries with `max_price=1` | 70.7 KB |
  | Full audio list (48 entries) | 96 KB |

  `Cache-Control: public, max-age=120`.
- **Gotcha found during this check.** Adding the `supported_parameters=response_format,structured_outputs` query filter **changes the returned prices to the cheapest (flex) tier**. For example, gemini-3.1-flash-lite showed $0.125/$0.75/audio $0.25 instead of $0.25/$1.50/$0.50, and 3.5-flash-lite showed $0.15/$1.25 instead of $0.30/$2.50. It also **drops `expiration_date`** (xiaomi/mimo-v2.5 showed `null` instead of `2026-10-21`). So **do not use that query filter; check `supported_parameters` on the device.** `max_price` (prompt $/M) did not distort anything.

**Client-side filter**, applied in order (drop the entry when a rule matches):

1. `id` ends with `:batch` (async Batch API, 24 h window).
2. `id` ends with `:free` (the free audio models list neither `response_format` nor `structured_outputs`).
3. `id` starts with `~` (aliases such as `~google/gemini-flash-latest`) or `openrouter/`.
4. `pricing.prompt < 0` or `pricing.completion < 0` (routers priced at −1: `openrouter/auto`, `openrouter/auto-beta`, `typesafe/jev-router`).
5. `expiration_date != null`. Exception: if it is the currently selected model, keep it and show "истекает YYYY-MM-DD" in red.
6. `supported_parameters` lacks either `response_format` or `structured_outputs`. This drops `perceptron/perceptron-mk1.5`, `thinkingmachines/inkling*` and the `:free` models.
7. `id` ends with `-preview` and the same id without `-preview` is in the list (drops `google/gemini-3.1-flash-lite-preview`).
8. `architecture.input_modalities` lacks `audio` (a safety net).

**Rank** by estimated cost per reference turn (10 s audio = 320 audio tokens, 600 text tokens in, 180 out), not by OpenRouter's sort:

`est = 320·(pricing.audio || pricing.prompt) + 600·pricing.prompt + (180 + (mandatory && no "minimal" ? 200 : 0))·pricing.completion`

**Fields to parse per entry** (everything else is skipped). Store each in a 160-byte struct:

| Field | Use |
|---|---|
| `id` (≤ 64) | API id |
| `name` (≤ 48) | display |
| `pricing.prompt`, `pricing.completion`, `pricing.audio` | strings with USD per token; `strtod`, ×1e6 for $/M |
| `supported_parameters[]` | `response_format`, `structured_outputs` checks |
| `reasoning.mandatory`, `reasoning.supported_efforts[]` | effort rule (§2.1) |
| `expiration_date` | filter / warning |
| `architecture.input_modalities[]` | audio check |

**Live result** (2026-10-17, after the filters, standard-tier prices in $ per 1M tokens):

| # | id | in | out | audio | Lowest effort | Est. $ per turn | Notes |
|---|---|---|---|---|---|---|---|
| 1 | `xiaomi/mimo-v2.6-flash` | 0.140 | 0.280 | (billed as input) | not exposed | 0.00018* | 8 providers incl. fp4/fp8 quantisations; audio support per provider and Russian quality unverified; non-Google |
| 2 | `qwen/qwen3.8-omni-flash` | 0.150 | 0.470 | (billed as input) | not exposed | 0.00022* | Alibaba only |
| 3 | `xiaomi/mimo-v2.6-pro` | 0.435 | 0.870 | (billed as input) | not exposed | 0.00056* | |
| 4 | **`google/gemini-3.1-flash-lite`** | 0.250 | 1.500 | 0.500 | minimal | **0.00058** | **Default** |
| 5 | `google/gemini-3.5-flash-lite` | 0.300 | 2.500 | 0.300 | minimal (mandatory) | 0.00073 | Fallback |
| 6 | `openai/gpt-audio-mini` | 0.600 | 2.400 | 0.600 | n/a | 0.00098 | OpenAI's own audio price is $10/$20, so the listing is suspect; OpenAI is blocked for RU billing |
| 7 | `google/gemini-3-flash-preview` | 0.500 | 3.000 | 1.000 | minimal | 0.00116 | preview |
| 8 | `google/gemini-3.6-flash` | 0.750 | 3.750 | 0.750 | minimal (mandatory) | 0.00136 | |
| 9 | `google/gemini-3.7-flash` | 0.750 | 3.750 | 0.750 | **low** (mandatory) | 0.00212 | |
| 10 | `google/gemini-3.8-flash` | 0.825 | 4.125 | 0.825 | **low** (mandatory) | 0.00233 | |
| 11 | `mistralai/voxtral-small-24b-2507` | 0.100 | 0.300 | 100.000 (unit unclear) | n/a | 0.032 | Russian not in its language list; sorts last |

\* The audio-token rate for MiMo and Qwen is unknown. The estimate assumes 32 tokens/s at the prompt price.

**Boot check of the selected model.** Run `GET /api/v1/model/{author}/{slug}` (1.6 KB):

- 404: model gone. Open the picker.
- `expiration_date` non-null: warn with the date.
- Re-check `input_modalities` and `supported_parameters`.
- Cache the prices for cost display.

Expiry notices appear only about 3 days ahead: `google/gemini-2.5-flash-lite` expires 2026-10-20 and `xiaomi/mimo-v2.5` on 2026-10-21.

### 7.2 TTS models and voices

**Query:**

```
GET https://openrouter.ai/api/v1/models?output_modalities=speech
```

It returns 35 models and 47.9 KB in one response. Pages of 12 are about 13 KB; MAI entries with 97 voices are about 4 KB each.

**Client-side filter:**

1. Drop entries where `supported_voices` is null or empty: `fish-audio/*` (needs reference ids), `fish-audio/drama-3`, `bytedance-seed/seed-audio-1-0` (prompt-driven).
2. Drop ids ending in `:free` (`fish-audio/s2.1-pro-free:free` is a free offer that ends 2026-11-30). A developer setting can re-enable them.
3. Drop entries where `expiration_date != null`.
4. Drop known PCM-incompatible models: `mistralai/voxtral-mini-tts-*`. OpenRouter's tutorial says it is MP3-only and a PCM request returns 400; the API-reference example contradicts this, so it stays excluded until tested.
5. Drop English-only models: `canopylabs/orpheus-3b-0.1-ft`, `sesame/csm-1b`, `deepgram/flux-tts`, `elevenlabs/eleven-turbo-v2`, `elevenlabs/eleven-flash-v2`.

**Fields to parse:** `id`, `name`, `pricing.prompt`, `pricing.completion`, `architecture.tokenizer`, `supported_voices[]` (strings), `expiration_date`.

**Price normalisation** (estimated $ per 1000 Russian characters, assuming about 14 chars/s of speech and about 3.5 chars per text token):

- `tokenizer == "Gemini"` (token-priced): `1000/3.5·prompt + (1000/14)·25·completion` (25 audio tokens per second).
- `prompt == 0 && completion > 0` (per-second pricing): `(1000/14)·completion`.
- Otherwise (per character): `1000·prompt`.

**Voice UI.** For `microsoft/*`, group `supported_voices` by locale: the part before `:`, split at its last `-`, for example `ru-RU` + `Masha`. Show only the locale matching the language being configured. For other models show a flat list.

**Live result** (2026-10-17), Russian-capable candidates ordered by price:

| id | Price (OpenRouter) | ≈ $/1000 RU chars | Voices | Notes |
|---|---|---|---|---|
| `google/gemini-3.8-flash-lite-tts` | $0.50/M text tokens + $6/M audio tokens | 0.011 | 30 | pcm only; no uk; Google (RU-billing block); Google doubles both prices on 2027-01-01 |
| **`microsoft/mai-voice-2.1-flash`** | $15/M chars | **0.015** | 97 / 28 locales | **Default**; Azure only; no uk, ja, ar |
| `x-ai/grok-voice-tts-1.0` | $15/M chars | 0.015 | 5 | auto language detection, has ja and ar, no uk |
| `google/gemini-3.8-flash-tts` | $0.50 + $9/M | 0.016 | 30 | |
| `elevenlabs/eleven-flash-v2.5` | $20/M chars | 0.020 | 21 | has uk; 50% promo reportedly ends 2026-10-19, after which about $40/M |
| `elevenlabs/eleven-v4-turbo` | $20/M chars | 0.020 | 21 | 90+ languages |
| `microsoft/mai-voice-2.1` | $22/M chars | 0.022 | 97 | higher fidelity |
| `google/gemini-3.1-flash-tts-preview` | $1 + $20/M | 0.036 | 30 | preview |
| `elevenlabs/eleven-v4`, `eleven-v3`, `eleven-multilingual-v2` | $40/M chars | 0.040 | 21 | |
| `minimax/speech-2.8-turbo` / `-hd` | $60 / $100 per M | 0.06 / 0.10 | 45 (English-named) | |

Not Russian-capable or not confirmed:

- `hexgrad/kokoro-82m`: $0.62/M (DeepInfra) or $4/M (Together, and the provider can't be pinned). 8 languages including ja and zh, no ru.
- `heygen/heygen-voice-1-turbo`: $4/M. Languages unknown; 44.1 kHz.
- `qwen/qwen-audio-3.0-tts-flash`: $15/M, 2 voices, languages unknown.
- `microsoft/mai-voice-2-flash`: 4 voices (en, es, fr, de).
- `deepgram/aura-2`: $30/M, Russian unconfirmed.

---

## 8. Key / account check: `GET /api/v1/key`

The response is about 1 KB. Parse only these fields from `data`:

| Field | Type | Meaning / UI |
|---|---|---|
| `label` | string | masked key, e.g. `sk-or-v1-au7...890` |
| `usage`, `usage_daily`, `usage_weekly`, `usage_monthly` | number (USD) | spend through this key (daily, weekly and monthly windows are UTC; the week starts Monday) |
| `limit` | number or null | the key's own credit cap |
| `limit_remaining` | number or null | the remaining part of that cap. **Not the account balance.** Null when the key has no limit |
| `limit_reset` | string or null | `daily`, `weekly`, `monthly` or null |
| `effective_budget` | object or null | `{remaining_usd, limit_usd, spend_usd, reset_interval, resets_at, scope}`: the budget that would block first (guardrail, workspace or key limit) |
| `is_free_tier` | bool | the account has never bought credits |
| `free_model_daily_requests` | `{used, limit, remaining}` | only relevant for `:free` models |
| `expires_at` | ISO string or null | key expiry |
| `rate_limit` | deprecated | ignore |

**Display logic (Settings → "Баланс"):**

- **Remaining** = the minimum of the non-null values among `limit_remaining` and `effective_budget.remaining_usd`.
  - If either is non-null: «Осталось $3.80 из $5.00 · сброс 1 ноября».
  - If both are null: «Остаток недоступен: задайте лимит для ключа на openrouter.ai → Keys».
- **Spend line:** «Потрачено: сегодня $0.04 · месяц $1.20 · всего $2.35».
- **Warnings:**
  - remaining below $1: yellow «Мало средств — ответы могут замедлиться». The docs say low balances (single-digit dollars) and keys near their limit add billing checks and latency, so recommend keeping at least $10.
  - remaining at or below 0: red «Лимит исчерпан».
  - `is_free_tier`: «Бесплатный аккаунт — пополните баланс».
- **On-device session counter:** the sum of `usage.cost` from chat and STT responses, plus TTS estimated as characters × the model's price. TTS responses carry no cost JSON.
- **When to refresh:** at boot after Wi-Fi and SNTP, when Settings is opened, every 25 turns, after any 402 or budget-403, and as the TLS pre-warm on PTT (§1.3).
- The true account balance (`GET /api/v1/credits` → `total_credits`, `total_usage`) **requires a management key**. Never store a management key on the device.

Example (OpenAPI example trimmed to the parsed fields):

```json
{"data":{"label":"sk-or-v1-au7...890","limit":100,"limit_remaining":74.5,"limit_reset":"monthly",
 "usage":25.5,"usage_daily":25.5,"usage_weekly":25.5,"usage_monthly":25.5,"is_free_tier":false,
 "expires_at":"2027-12-31T23:59:59Z",
 "effective_budget":{"scope":"member","limit_usd":50,"spend_usd":32.5,"remaining_usd":17.5,"reset_interval":"weekly","resets_at":"2026-10-12T00:00:00.000Z"},
 "free_model_daily_requests":{"limit":50,"remaining":38,"used":12}}}
```

---

## 9. Error handling

### 9.1 Table

How to read the "Detection" column:

- For HTTP rows it is the HTTP status, or `error.code` when the error comes in a 200 body or mid-stream.
- For N rows it is the `esp_err_t` returned by `open()`, `write()` or `read()`, plus `esp_http_client_get_and_clear_last_tls_error(c, &tls_err, &tls_flags)`.

| ID | Condition / detection | Retry | Message (RU) | Device action |
|---|---|---|---|---|
| N0 | Wi-Fi not connected (checked before the request) | no | «Нет Wi-Fi. Подключитесь в Настройках.» | Tap opens Wi-Fi |
| N1 | DNS: `tls_err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME` | 2× (1 s, 3 s) | «Сервер не найден. Проверьте интернет.» | |
| N2 | TCP: `ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST`, `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`, `ESP_ERR_HTTP_CONNECTING` / `ESP_ERR_HTTP_CONNECT` | 2× (1 s, 3 s) | «Нет связи с сервером. Повторяю…» → «Сервер недоступен. Попробуйте позже.» | |
| N3 | TLS certificate: `ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED` with `tls_flags != 0` (X.509 verify flags) | no | «Ошибка сертификата сервера. Нужно обновить прошивку.» | Log the flags. If `MBEDTLS_HAVE_TIME_DATE` is enabled, SNTP must be synced first |
| N4 | TLS handshake failure or timeout without certificate flags (`ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT`) | 1× (2 s) | «Не удалось установить защищённое соединение.» | |
| N5 | Upload write failed: `esp_http_client_write` returned −1 (`ESP_ERR_HTTP_WRITE_DATA`) | 1× on a fresh connection, plus 1 silent retry if the connection was reused | «Связь оборвалась при отправке. Повторяю…» | Re-stream from the PSRAM PCM |
| N6 | First-byte timeout: `fetch_headers` or first `read` returns `-ESP_ERR_HTTP_EAGAIN` / `ESP_ERR_HTTP_READ_TIMEOUT` | chat 1×, TTS 1×, GETs 2× | «ИИ долго не отвечает…» → «Нет ответа от ИИ. Попробуйте ещё раз.» | `esp_http_client_close()` |
| N7 | Body cut: `read < 0` mid-body, `ESP_ERR_HTTP_INCOMPLETE_DATA` / `ESP_ERR_HTTP_CONNECTION_CLOSED`, or `!is_complete_data_received()` | 1× | «Ответ оборвался. Повторяю…» | TTS re-requests only the failed chunk |
| N8 | Total deadline exceeded (§9.2) | no | «Слишком долго. Попробуйте ещё раз.» | close |
| 400 | Bad request (`invalid_request`, `invalid_prompt`, `string_too_long`) | no, except automatic fixes: TTS message mentions `speed` → resend without it; mentions `voice` → resend with the model's default voice; chat mentions `reasoning`/`effort` → resend without `reasoning`; mentions `response_format`/`schema` → resend with `{"type":"json_object"}` and the schema pasted into the system prompt | «Сервер отклонил запрос. Проверьте модель и голос в Настройках.» | Log `error.message` |
| 400 ctx | `error_type == context_length_exceeded` | 1× with history cleared | «Разговор слишком длинный — начинаю новый.» | Clear history |
| 401 | Invalid, disabled or missing key | no | «Неверный API-ключ. Введите ключ заново в Настройках.» | Open key entry |
| 402 | `limit_source == openrouter_in_flight_budget` with `Retry-After ≤ 10 s` | 1× after Retry-After | «Подождите пару секунд…» | |
| 402 | `openrouter_key_limit` | no | «Исчерпан лимит этого ключа. Увеличьте его на openrouter.ai.» | Refresh /key |
| 402 | `openrouter_credits` or no metadata | no | «Закончились средства на OpenRouter. Пополните баланс.» | Refresh /key |
| 403 | Budget: `limit_source` is `openrouter_guardrail_budget`, `openrouter_workspace_budget` or `openrouter_key_limit` | no | «Исчерпан бюджет ключа до {resets_at}.» | |
| 403 | Region or billing block: message contains "region" (e.g. "billing address is in a region that does not have access to models from OpenAI, Anthropic, and Google", "Not available in your region") | no | «Модель недоступна для вашего региона. Выберите модель другого производителя.» | Offer a one-tap switch: chat → `xiaomi/mimo-v2.6-flash`, TTS → MAI or Grok |
| 403 | `content_policy_violation` / `refusal` / moderation (`metadata.reasons`) | no | «Запрос отклонён фильтром безопасности.» | |
| 403 | Other | no | «Доступ запрещён (403).» | |
| 404 | Model not found, or no endpoint meets the routing requirements (e.g. `require_parameters` + audio + schema) | no | «Модель не найдена или не поддерживает голос и JSON. Выберите другую.» | Open the picker; STT fallback if the model has no audio |
| 408 | Request timeout | 1× (1 s) | «Сервер не дождался запроса. Повторяю…» | |
| 413 | Payload too large | no | «Запись слишком длинная. Говорите короче.» | Halve the maximum record length for this session |
| 422 | Unprocessable | no | «Сервер не смог обработать запрос.» | |
| 429 | Rate limited (OpenRouter platform or provider). Read `Retry-After` (seconds or HTTP-date) or `X-RateLimit-Reset` | Wait ≤ 10 s: up to 2× honouring it, otherwise 2 s then 5 s. Wait > 10 s: no retry | «Слишком много запросов. Подождите…» → «Лимит запросов. Попробуйте через минуту.» | For `:free` models suggest a paid one |
| 500 | `server` / `unmapped` (message masked) | 1× (1.5 s) | «Ошибка сервера OpenRouter. Повторяю…» | |
| 502 | `provider_unavailable` | 2× (1.5 s, 4 s) | «Модель временно недоступна. Повторяю…» | OpenRouter has already tried the other providers and the `models` fallback |
| 503 | No provider meets the routing, or `provider_overloaded`. `Retry-After` may be present | 1× (Retry-After or 3 s) | «Нет доступных серверов для модели. Попробуйте позже.» | |
| 504 / 524 | Provider or edge timeout | 1× (2 s) | «Модель не ответила вовремя. Повторяю…» | |
| 529 | Provider overloaded | 2× (3 s, 6 s) | «Сервис перегружен. Повторяю…» | |
| P1 | 200 with root `error`, or an SSE error chunk / `finish_reason:"error"` | per `error.code` row above | as that row | |
| P2 | 200 but the content does not parse into the schema | 1× | «Не удалось разобрать ответ ИИ.» | |
| P3 | `finish_reason:"length"` | no | partial text + «(ответ обрезан)» | not stored in history |
| P4 | `transcript == ""` | no | «Не расслышал. Повторите, пожалуйста.» | no TTS |
| P5 | `finish_reason:"content_filter"` or `message.refusal` | no | «ИИ отказался отвечать на этот запрос.» | |
| W1 | Auto-mode web re-run (§2.8) failed for any reason above, the first request succeeded | no | none: the first answer (the holding phrase) + «Не получилось найти это в интернете, попробуйте ещё раз.», shown and spoken | No card; not stored in history. Always mode: a 400/404 about the plugin resends once without it |
| T1 | TTS still failing after retries and fallback routes | none | text stays + toast «Озвучка недоступна» | continue text-only |
| T2 | TTS response is not PCM (`audio/mpeg` etc.) | no | «Этот голос отдаёт неподдерживаемый формат.» | try the next route (§5.6) |

**Retry policy:**

- **Backoff:** `delay = min(8 s, 1 s·2^k)·U(0.8, 1.2)`.
- **Retry-After** is honoured when it is 10 s or less. It may be integer seconds or an HTTP-date, which is converted with SNTP time.
- **Wall-clock budget:** at most **20 s** of retries per user action. Show «Повтор 1/2…» on the status ring.
- **POSTs aren't idempotent for billing.** A repeated chat call may be charged twice, which is acceptable at about $0.0006.
- **Silent retries:** background GETs (`/models`, `/key`) retry 3× with 2, 4 and 8 s delays and never show a message.
- **Never retry a whole turn after TTS playback has started.** Only the failing chunk is retried (N7).

### 9.2 Timeouts

`esp_http_client`'s `timeout_ms` applies to each socket operation. Set it with `esp_http_client_set_timeout_ms()`:

1. before `open()`: the connect value;
2. before `fetch_headers()`: the first-byte value;
3. after the headers: the stall value.

Enforce the total with `esp_timer_get_time()` after every read and write, and abort with `esp_http_client_close()`.

| Phase | Chat (non-stream) | Web re-run (§2.8) | Chat SSE (v1.1) | TTS chunk | STT | GET models / key |
|---|---|---|---|---|---|---|
| Connect (DNS + TCP + TLS) | 10 s | 10 s | 10 s | 10 s | 10 s | 10 s |
| Each upload write | 10 s | 10 s | 10 s | 10 s | 10 s | n/a |
| First byte after the body is sent | 30 s | 30 s | 30 s (comments count) | 15 s | 30 s | 10 s |
| Stall between reads | 15 s | 15 s | 20 s (comments reset it) | 10 s | 15 s | 10 s |
| Total wall clock, upload included | 45 s | 30 s, one attempt | 60 s | 60 s | 45 s | 20 s |

The web re-run runs after the first request (up to 45 s per attempt plus 20 s of retries), so a searched turn can take up to about 95 s. The app's "no result" deadline (75 s from the submit) is therefore extended to 35 s after `AI_EV_WEB`. Always mode uses the chat column: the search adds about 1 s to the request.

---

## 10. Estimated cost per turn

**Assumptions:**

- Live standard-tier prices on 2026-10-17. Gemini audio is 32 tokens/s.
- Russian is about 3.5 characters per Gemini text token. This is an estimate, so use `usage.cost` for real numbers.
- Gemini TTS is 25 audio tokens/s at about 14 characters per second of speech.
- **Reference assistant turn:** 10 s of speech, 600 text input tokens (system prompt plus 2–3 turns of history), 180 output tokens (transcript, reply, JSON and minimal reasoning), and a 250-character reply.
- **Reference translator turn:** 5 s of speech, 300 input tokens, 100 output tokens, an 80-character translation.

| Turn | LLM | TTS with MAI 2.1 Flash ($15/M chars) | TTS with Gemini 3.8 Flash-Lite TTS | **Total (MAI)** | **Total (Gemini TTS)** |
|---|---|---|---|---|---|
| Assistant, voice in, text-only reply | $0.00058 (audio $0.00016 + text $0.00015 + out $0.00027) | n/a | n/a | **$0.00058** | same |
| Assistant, voice in + spoken reply | $0.00058 | $0.00375 | $0.0027 (446 audio tokens × $6/M + text) | **$0.0043** | **$0.0033** |
| Assistant, typed in + spoken reply | $0.00042 | $0.00375 | $0.0027 | $0.0042 | $0.0031 |
| Assistant via STT fallback (Whisper + text chat), spoken | $0.00003–0.00011 (Whisper, 10 s, DeepInfra or Groq) + $0.00042 | $0.00375 | $0.0027 | ≈ $0.0043 | ≈ $0.0032 |
| Translator utterance (spoken translation) | $0.00031 | $0.0012 | $0.00087 | **$0.0015** | **$0.0012** |
| Translator, text-only | $0.00031 | n/a | n/a | $0.00031 | same |
| Worst-case assistant LLM (30 s audio, full 3000-char history, 800-char reply) | $0.0015 | $0.012 | $0.008 | $0.014 | $0.010 |
| Assistant with web search (§2.8, auto: first answer + re-run with 3 results), typed, text-only | about $0.014 (measured: $0.00012 + about $0.0136) | n/a | n/a | about $0.014 | same |

**Monthly figures:**

- 30 spoken assistant turns per day: about **$3.9/month** with MAI, or about $3.0 with Gemini TTS.
- The same turns text-only: about **$0.52/month**.
- 50 translated utterances per day: about **$2.3/month**.

TTS is 85–90% of the cost of a spoken turn, so keep replies short. That is the reason for the prompt's 300-character cap.

**Price risks:**

- Google doubles Gemini TTS input ($0.50 → $1.00/M) and output ($6 → $12/M) on 2027-01-01. Whether OpenRouter passes this on is unknown.
- The ElevenLabs promo reportedly ends 2026-10-19.
- **In-flight budget:** with `max_tokens` 1024, the up-front hold is about $0.0015 per request, so a small balance is not a problem.

---

## 11. First-run verification with a real key (to close the [needs key] items)

1. **Chat with a 5 s Russian WAV.**
   - Save the full response.
   - Check `prompt_tokens_details.audio_tokens` ≈ 32/s and that `usage.cost` matches §10.
   - Check that the strict schema with `enum` and `additionalProperties:false` is accepted on the Google AI Studio and Vertex endpoints (no 400).
   - Check that `reasoning.exclude` with `require_parameters` routes fine.
   - Check that `models` without `model` works.
2. **TTS with MAI** (`ru-RU-Masha`):
   - Dump the response headers: exact `Content-Type`, whether `rate=`/`channels=` are present, chunked or not.
   - Measure time to first byte and the arrival cadence (progressive or one burst).
   - Test 150-, 400- and 1500-character inputs.
3. **Gemini TTS:** Ukrainian, Japanese and Arabic samples; check `speed` handling (400 or ignored).
4. **ElevenLabs Flash v2.5:** a Ukrainian sample; confirm the PCM rate in `Content-Type`.
5. **`GET /api/v1/key`:** the real body. Is `limit` null when no limit is set?
6. **Region and billing:** does the user's account get a 403 on `google/*` models? If so, switch the defaults to `xiaomi/mimo-v2.6-flash` plus MAI and re-test MiMo's audio input and Russian ASR.
7. **ESP32-S3 upload throughput** for a 430 KB body over TLS. It decides the maximum record length and whether to add AAC (`format:"aac"`, esp_audio_codec) in v2.


## Open questions

- MAI-Voice-2.1-Flash pcm response header: is it exactly 'audio/pcm;rate=24000;channels=1' (as shown for Gemini in the OpenRouter cookbook) or bare 'audio/pcm'? The spec falls back to a per-model rate table either way. Needs a key.
- Does /api/v1/audio/speech stream PCM progressively for MAI and Gemini, or send it in one burst at the end? This sets real time-to-first-audio and whether the 200-character first chunk is worth it. Needs a key.
- Does OpenRouter's Google endpoint accept the strict json_schema as written (enum + additionalProperties:false + descriptions)? If it returns 400, the spec falls back to json_object plus the schema in the prompt. Needs a key.
- Is sending 'models' without 'model' the right form, and is sending both allowed? The fallback guide shows only 'models'. Needs a key.
- With require_parameters:true, is max_completion_tokens treated as an alias of max_tokens? The spec sends max_tokens because that is the name in every model's supported_parameters.
- Is the user's OpenRouter account Russia-billed or used from a Russian IP? Reports say Google/OpenAI/Anthropic models, or all access, are blocked for such accounts. That would make the Gemini defaults fail with 403 and force the non-Google chain (xiaomi/mimo-v2.6-flash + MAI/Grok).
- Ukrainian TTS: MAI and Grok have no uk, and Google's Gemini 3.8 TTS language table does not list Ukrainian. Is ElevenLabs Flash v2.5 via OpenRouter acceptable for uk (price about $20/M chars now, reportedly about $40/M after the promo ends 2026-10-19)? Does it need provider.options to force the language?
- Audio tokenisation and Russian ASR quality of the cheaper non-Google candidates (xiaomi/mimo-v2.6-flash, qwen/qwen3.8-omni-flash). Their listings have no audio price, and some MiMo providers are fp4/fp8 quantised; per-provider audio support is unverified.
- The supported_parameters query filter on GET /models returned flex-tier prices and null expiration_date (observed 2026-10-17). Is this a temporary OpenRouter bug? The spec avoids it and filters on the device.
- The tokens-per-character ratio for Russian with the Gemini tokenizer (assumed about 3.5 characters per token) and the speech rate of MAI and Gemini voices (assumed about 14 characters per second) drive the cost estimates. They should be replaced with usage data from the first real turns.
- Gender and naturalness of the MAI voices picked per language (inferred from names: Masha, Mia, Soleil, Rosa, Luana, Elif, Mei, Haena) need a listening check.