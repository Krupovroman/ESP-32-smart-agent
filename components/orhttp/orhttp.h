#pragma once
// HTTPS transport shared by chat, TTS, STT and the GET endpoints (docs/protocol.md §1, §9;
// architecture §1.9). One esp_http_client handle per scheme+host+port, kept alive and used
// strictly in sequence behind a mutex. Request bodies are streamed with an exact
// Content-Length; responses are read in 1 s socket ticks so cancellation is quick.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_client.h"

// Request classes: they select the retry counts of the N6 row (chat 1x, TTS 1x, GETs 2x).
typedef enum { OR_KIND_CHAT, OR_KIND_TTS, OR_KIND_STT, OR_KIND_GET } or_kind_t;

// Per-phase timeouts of protocol §9.2 (ms). write_ms = 0 means "no upload".
typedef struct {
    int connect_ms, write_ms, first_byte_ms, stall_ms, total_ms;
} or_timeouts_t;
#define OR_TO_CHAT {10000, 10000, 30000, 15000, 45000}
#define OR_TO_WEB  {10000, 10000, 30000, 15000, 30000}   // auto-mode web search request (text only)
#define OR_TO_SSE  {10000, 10000, 30000, 20000, 60000}
#define OR_TO_TTS  {10000, 10000, 15000, 10000, 60000}
#define OR_TO_STT  {10000, 10000, 30000, 15000, 45000}
#define OR_TO_GET  {10000,     0, 10000, 10000, 20000}

// Body caps of protocol §1.5 (bytes).
#define OR_CAP_CHAT   (64 * 1024)
#define OR_CAP_STT    (16 * 1024)
#define OR_CAP_ERROR  (16 * 1024)
#define OR_CAP_KEY    (8 * 1024)
#define OR_CAP_MODELS (32 * 1024)

typedef enum { OR_PH_NONE, OR_PH_CONNECT, OR_PH_WRITE, OR_PH_FIRST_BYTE, OR_PH_BODY, OR_PH_DONE } or_phase_t;

// Response metadata of one request. Headers come from HTTP_EVENT_ON_HEADER (protocol §1.3).
typedef struct {
    int status;                       // HTTP status, 0 = no response
    int64_t content_length;           // -1 = chunked / unknown
    // retry_after holds an HTTP-date too ("Sun, 06 Nov 1994 08:49:37 GMT" is 29 chars)
    char content_type[64], retry_after[32], gen_id[64], provider[32], rl_reset[24];
    bool conn_close;                  // server sent "Connection: close"
    bool reused;                      // ran on a kept-alive socket
    or_phase_t phase;                 // phase reached (or the one that failed)
    esp_err_t err;                    // transport error of the failing phase, ESP_OK = none
    int tls_err, tls_code, tls_flags; // esp_http_client_get_and_clear_last_tls_error()
    bool aborted, deadline;           // abort callback fired / total deadline exceeded
    bool too_big;                     // body larger than the caller's cap
    int64_t t_start_us, t_sent_us, t_headers_us, t_done_us;
    size_t bytes_sent, bytes_recv;
} or_resp_t;

typedef struct or_conn or_conn_t;
typedef bool (*or_abort_fn)(void *ctx);
// Writes exactly body_len bytes with or_write_all() / or_write_b64_wav().
typedef esp_err_t (*or_body_fn)(or_conn_t *c, void *ctx);

typedef struct {
    esp_http_client_method_t method;
    const char *url;                  // full request URL (never logged: it may carry a relay secret)
    const char *bearer;               // API key; NULL or "" = no Authorization header
    const char *accept;               // NULL = no Accept header (TTS)
    const char *referer;              // HTTP-Referer for OpenRouter; NULL or "" = not sent
    or_timeouts_t to;
    int64_t body_len;                 // exact Content-Length, 0 for GET
    or_body_fn write_body;            // NULL = no body
    void *body_ctx;
    or_abort_fn abort;                // polled between writes and on every 1 s read tick
    void *abort_ctx;
} or_req_t;

// One row of the protocol §9.1 table, filled by or_classify().
typedef struct {
    char id[8];                       // "N2", "429", "400ctx", "P2", ...; "H<code>" = status without a row
    int http;                         // HTTP status or error.code, 0 for transport errors
    bool retry;                       // the row allows automatic retries
    uint8_t max_retries;
    uint16_t retry_ms[2];             // per-row delays, 0 = backoff formula
    int32_t retry_after_ms;           // honoured Retry-After / X-RateLimit-Reset, -1 = none
    char msg_ru[160];                 // final user-facing message
    char msg_retry_ru[96];            // shown while retrying ("" = use msg_ru)
    char message[128];                // error.message or esp_err name, for logs and auto-fixes
    char error_type[40];              // metadata.error_type
    char limit_source[40];            // metadata.limit_source
    esp_err_t transport;
} or_err_t;

// Retry bookkeeping for one user action (protocol §9.1 retry policy).
typedef struct {
    int64_t t_first_fail_us;
    bool background;                  // silent GET policy: 3x with 2, 4, 8 s
    uint8_t total;
    struct {
        char id[8];
        uint8_t n;
    } ids[4];
} or_retry_t;

// Connection keyed by scheme+host+port of base_url (lazily created, never freed).
or_conn_t *or_conn_get(const char *base_url);
// Serialises users of one connection; false after ms.
bool or_conn_lock(or_conn_t *c, uint32_t ms);
void or_conn_unlock(or_conn_t *c);
// True while a kept-alive socket is open (used for the TLS pre-warm decision).
bool or_conn_live(or_conn_t *c);
// Drops the socket (barge-in, settings change).
void or_conn_close(or_conn_t *c);

// Sets URL, method and headers and opens the request (reusing a live socket). Caller holds the lock.
esp_err_t or_begin(or_conn_t *c, const or_req_t *q, or_resp_t *r);
// Writes body bytes in 4 KB pieces, polling abort and the total deadline.
esp_err_t or_write_all(or_conn_t *c, const void *p, size_t n);
// Writes the 44-byte WAV header + PCM (16-bit mono) as one base64 stream (3072 raw -> 4096 chars per write).
esp_err_t or_write_b64_wav(or_conn_t *c, const int16_t *pcm, size_t samples, uint32_t rate);
// Waits for the response headers with the first-byte timeout (1 s ticks).
esp_err_t or_fetch_headers(or_conn_t *c);
// Reads body bytes: >0 count, 0 = end of body, <0 = error (details in the or_resp_t).
int or_read(or_conn_t *c, void *buf, int len);
// Resets the stall timer after the caller spent time elsewhere (e.g. blocked on the speaker ring).
void or_touch(or_conn_t *c);
// Reads the whole body into a NUL-terminated PSRAM buffer (caller frees). Over cap: drained, too_big set.
esp_err_t or_read_body(or_conn_t *c, char **out, size_t cap, size_t *len);
// Ends the request: ok with a complete body keeps the socket alive, anything else closes it.
esp_err_t or_finish(or_conn_t *c, bool ok);
// or_begin + body + or_fetch_headers, reconnecting once silently when a reused socket was dead.
esp_err_t or_start(or_conn_t *c, const or_req_t *q, or_resp_t *r);
// Complete JSON exchange: lock, start, read the body (cap), finish, unlock. ESP_OK = an HTTP
// response was read (any status); *body may be NULL when there was none.
esp_err_t or_exchange(const or_req_t *q, or_resp_t *r, char **body, size_t cap, size_t *len);

// Maps a failed request to its protocol §9.1 row (transport errors, HTTP status, or a 200 body
// whose root has "error"). body may be NULL.
void or_classify(or_kind_t kind, const or_resp_t *r, const char *body, or_err_t *e);
// Local rows that are not HTTP errors: "N0", "K0", "P2", "P4", "P5", "T1", "T2", "AB", "M0".
void or_err_local(or_err_t *e, const char *id);
// True when the body is JSON whose root has an "error" object (protocol §1.5 error rule).
bool or_body_has_error(const char *body);

void or_retry_init(or_retry_t *s, bool background);
// True when e allows another attempt; *delay_ms is the wait before it. deadline_us is the
// absolute end of the user action (0 = none); at most 20 s of retries per action.
bool or_retry_next(or_retry_t *s, const or_err_t *e, int64_t deadline_us, uint32_t *delay_ms);

// True for openrouter.ai and for relays that keep its "/api/v1" path (attribution headers,
// "models" fallback array, provider/reasoning objects).
bool or_is_openrouter(const char *url);
// Copies "https://host[:port]" of url into out; false when it is not an http(s) URL.
bool or_url_origin(const char *url, char *out, size_t len);
// Base64 length of n raw bytes, and of a WAV image (44-byte header) with the given samples.
size_t or_b64_len(size_t n);
size_t or_wav_b64_len(size_t samples);
// Encodes n bytes (n % 3 may be non-zero only for the last piece); returns the chars written.
size_t or_b64_encode(const uint8_t *in, size_t n, char *out);
// Fills a 44-byte WAV header for 16-bit mono PCM.
void or_wav_header(uint8_t hdr[44], uint32_t rate, size_t samples);
