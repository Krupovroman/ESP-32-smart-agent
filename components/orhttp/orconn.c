// Shared keep-alive HTTPS connections (protocol §1.2-§1.5, §9.2).
#include "orhttp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "orhttp";

#define MAX_CONNS     3
#define TICK_MS       1000                         // socket timeout while waiting: abort is polled this often
#define WRITE_PIECE   4096
#define IDLE_MAX_US   (240LL * 1000 * 1000)        // relays drop idle keep-alive sockets (Caddy: 5 min)
#define USER_AGENT    "RoundAssistant/1.0 (ESP32-S3; ESP-IDF 6.1)"
#define OR_TITLE      "ESP32 Round Assistant"

struct or_conn {
    char key[96];                 // scheme://host:port
    esp_http_client_handle_t h;
    SemaphoreHandle_t lock;
    bool live;                    // socket open after a complete response: next request reuses it
    int64_t last_used_us;
    const or_req_t *q;            // request in progress (NULL between requests)
    or_resp_t *r;
    int64_t deadline_us;          // total deadline of the request in progress
    int64_t last_data_us;         // stall timer
};

static or_conn_t s_conns[MAX_CONNS];
static int s_n_conns;
static SemaphoreHandle_t s_pool_lock;
static StaticSemaphore_t s_pool_lock_buf;
static int s_pool_state;          // 0 none, 1 being created, 2 ready

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void pool_init(void)
{
    // The ai and tts tasks may get their first connection at the same time.
    int expect = 0;
    if (__atomic_compare_exchange_n(&s_pool_state, &expect, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        s_pool_lock = xSemaphoreCreateMutexStatic(&s_pool_lock_buf);
        // fetch_headers() logs a warning on every 1 s wait tick; real errors stay visible
        esp_log_level_set("HTTP_CLIENT", ESP_LOG_ERROR);
        __atomic_store_n(&s_pool_state, 2, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&s_pool_state, __ATOMIC_ACQUIRE) != 2) {
        vTaskDelay(1);
    }
}

// Case-insensitive substring test (ASCII).
static bool contains_ci(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; s && *s; s++) {
        if (!strncasecmp(s, w, n)) {
            return true;
        }
    }
    return false;
}

// ---- URL helpers ------------------------------------------------------------------------------

bool or_url_origin(const char *url, char *out, size_t len)
{
    if (!url || (strncasecmp(url, "https://", 8) && strncasecmp(url, "http://", 7))) {
        return false;
    }
    const char *host = strstr(url, "://") + 3;
    size_t n = strcspn(host, "/?#");
    size_t pre = (size_t)(host - url);
    if (n == 0 || pre + n + 1 > len) {
        return false;
    }
    memcpy(out, url, pre + n);
    out[pre + n] = 0;
    for (size_t i = 0; i < pre + n; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] - 'A' + 'a');
        }
    }
    return true;
}

bool or_is_openrouter(const char *url)
{
    char origin[96];
    if (!or_url_origin(url, origin, sizeof(origin))) {
        return false;
    }
    const char *host = strstr(origin, "://") + 3;
    size_t hl = strcspn(host, ":");
    static const char OR_HOST[] = "openrouter.ai";
    if ((hl == sizeof(OR_HOST) - 1 && !strncmp(host, OR_HOST, hl)) ||
        (hl > sizeof(OR_HOST) && !strncmp(host + hl - sizeof(OR_HOST), ".openrouter.ai", sizeof(OR_HOST)))) {
        return true;
    }
    // A relay forwards a secret path prefix to openrouter.ai and keeps its "/api/v1" path.
    const char *path = url + strlen(origin);
    return strstr(path, "/api/v1") != NULL;
}

// ---- connection pool --------------------------------------------------------------------------

static esp_err_t or_evt(esp_http_client_event_t *e)
{
    or_conn_t *c = e->user_data;
    if (!c) {
        return ESP_OK;
    }
    if (e->event_id == HTTP_EVENT_DISCONNECTED) {
        c->live = false;
    } else if (e->event_id == HTTP_EVENT_ON_HEADER && c->r && e->header_key && e->header_value) {
        or_resp_t *r = c->r;
        const char *k = e->header_key, *v = e->header_value;
        if (!strcasecmp(k, "Content-Type")) {
            strlcpy(r->content_type, v, sizeof(r->content_type));
        } else if (!strcasecmp(k, "Retry-After")) {
            strlcpy(r->retry_after, v, sizeof(r->retry_after));
        } else if (!strcasecmp(k, "X-Generation-Id")) {
            strlcpy(r->gen_id, v, sizeof(r->gen_id));
        } else if (!strcasecmp(k, "X-Provider-Name")) {
            strlcpy(r->provider, v, sizeof(r->provider));
        } else if (!strcasecmp(k, "X-RateLimit-Reset")) {
            strlcpy(r->rl_reset, v, sizeof(r->rl_reset));
        } else if (!strcasecmp(k, "Connection") && contains_ci(v, "close")) {
            r->conn_close = true;
        }
    }
    return ESP_OK;
}

static esp_err_t conn_create(or_conn_t *c, const char *origin)
{
    esp_http_client_config_t cfg = {
        .url = origin,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = USER_AGENT,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .keep_alive_enable = true,      // TCP keepalive probes; HTTP reuse comes from not closing
        .keep_alive_idle = 15,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .event_handler = or_evt,
        .user_data = c,
    };
    c->h = esp_http_client_init(&cfg);
    if (!c->h) {
        return ESP_ERR_NO_MEM;
    }
    c->lock = xSemaphoreCreateMutex();
    if (!c->lock) {
        esp_http_client_cleanup(c->h);
        c->h = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

or_conn_t *or_conn_get(const char *base_url)
{
    char origin[96];
    if (!or_url_origin(base_url, origin, sizeof(origin))) {
        ESP_LOGE(TAG, "not an http(s) URL");
        return NULL;
    }
    pool_init();
    xSemaphoreTake(s_pool_lock, portMAX_DELAY);
    or_conn_t *found = NULL;
    for (int i = 0; i < s_n_conns; i++) {
        if (!strcmp(s_conns[i].key, origin)) {
            found = &s_conns[i];
            break;
        }
    }
    if (!found && s_n_conns < MAX_CONNS) {
        or_conn_t *c = &s_conns[s_n_conns];
        memset(c, 0, sizeof(*c));
        strlcpy(c->key, origin, sizeof(c->key));
        if (conn_create(c, origin) == ESP_OK) {
            s_n_conns++;
            found = c;
        }
    }
    if (!found) {
        // Pool full (base URLs changed in the settings): take over the least recently used idle
        // slot. Its handle follows the new host by itself: set_url() closes on a host change.
        or_conn_t *lru = NULL;
        for (int i = 0; i < s_n_conns; i++) {
            if (!lru || s_conns[i].last_used_us < lru->last_used_us) {
                lru = &s_conns[i];
            }
        }
        if (lru && or_conn_lock(lru, 0)) {
            esp_http_client_close(lru->h);   // the old host's socket must not count as reusable
            lru->live = false;
            strlcpy(lru->key, origin, sizeof(lru->key));
            or_conn_unlock(lru);
            found = lru;
        }
    }
    xSemaphoreGive(s_pool_lock);
    if (!found) {
        ESP_LOGE(TAG, "no connection slot for %s", origin);
    }
    return found;
}

bool or_conn_lock(or_conn_t *c, uint32_t ms)
{
    return c && xSemaphoreTake(c->lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void or_conn_unlock(or_conn_t *c)
{
    if (c) {
        xSemaphoreGive(c->lock);
    }
}

bool or_conn_live(or_conn_t *c)
{
    return c && c->live && now_us() - c->last_used_us < IDLE_MAX_US;
}

void or_conn_close(or_conn_t *c)
{
    if (c && or_conn_lock(c, 5000)) {
        esp_http_client_close(c->h);
        c->live = false;
        or_conn_unlock(c);
    }
}

// ---- request phases ---------------------------------------------------------------------------

static void grab_tls_error(or_conn_t *c)
{
    int code = 0, flags = 0;
    esp_err_t e = esp_http_client_get_and_clear_last_tls_error(c->h, &code, &flags);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        c->r->tls_err = e;
        c->r->tls_code = code;
        c->r->tls_flags = flags;
    }
}

// Polls abort and the total deadline; true = stop now.
static bool must_stop(or_conn_t *c)
{
    if (c->q->abort && c->q->abort(c->q->abort_ctx)) {
        c->r->aborted = true;
        return true;
    }
    if (now_us() > c->deadline_us) {
        c->r->deadline = true;
        return true;
    }
    return false;
}

static void set_header(esp_http_client_handle_t h, const char *k, const char *v)
{
    if (v && v[0]) {
        esp_http_client_set_header(h, k, v);
    } else {
        esp_http_client_delete_header(h, k);
    }
}

static void apply_headers(or_conn_t *c, const or_req_t *q)
{
    esp_http_client_handle_t h = c->h;
    if (q->bearer && q->bearer[0]) {
        char auth[224];
        snprintf(auth, sizeof(auth), "Bearer %s", q->bearer);
        esp_http_client_set_header(h, "Authorization", auth);
        memset(auth, 0, sizeof(auth));
    } else {
        esp_http_client_delete_header(h, "Authorization");
    }
    set_header(h, "Content-Type", q->method == HTTP_METHOD_POST ? "application/json" : NULL);
    set_header(h, "Accept", q->accept);
    // Attribution only for OpenRouter (directly or through the relay); never Accept-Encoding.
    bool orr = or_is_openrouter(q->url);
    set_header(h, "X-OpenRouter-Title", orr ? OR_TITLE : NULL);
    bool ref = orr && q->referer && q->referer[0];
    set_header(h, "HTTP-Referer", ref ? q->referer : NULL);
    set_header(h, "X-OpenRouter-App-Visibility", ref ? "hidden" : NULL);
}

esp_err_t or_begin(or_conn_t *c, const or_req_t *q, or_resp_t *r)
{
    memset(r, 0, sizeof(*r));
    r->content_length = -1;
    r->t_start_us = now_us();
    c->q = q;
    c->r = r;
    c->deadline_us = r->t_start_us + (int64_t)q->to.total_ms * 1000;
    if (c->live && now_us() - c->last_used_us >= IDLE_MAX_US) {
        esp_http_client_close(c->h);   // probably dropped by the server already
        c->live = false;
    }
    r->reused = c->live;
    c->live = false;

    esp_err_t err = esp_http_client_set_url(c->h, q->url);
    if (err != ESP_OK) {
        esp_http_client_close(c->h);   // a socket left open here would be reused unannounced
        r->phase = OR_PH_CONNECT;
        r->err = err;
        return err;
    }
    esp_http_client_set_method(c->h, q->method);
    apply_headers(c, q);
    // open() connects (when needed) and sends the headers with the same socket timeout
    esp_http_client_set_timeout_ms(c->h, r->reused ? (q->to.write_ms ? q->to.write_ms : q->to.connect_ms)
                                                   : q->to.connect_ms);
    r->phase = r->reused ? OR_PH_WRITE : OR_PH_CONNECT;
    err = esp_http_client_open(c->h, (int)q->body_len);
    if (err != ESP_OK) {
        if (err == ESP_ERR_HTTP_WRITE_DATA || err == ESP_ERR_HTTP_HEADER_TOO_LONG) {
            r->phase = OR_PH_WRITE;
        }
        r->err = err;
        grab_tls_error(c);
        esp_http_client_close(c->h);
        return err;
    }
    r->phase = OR_PH_WRITE;
    esp_http_client_set_timeout_ms(c->h, q->to.write_ms ? q->to.write_ms : q->to.connect_ms);
    return ESP_OK;
}

esp_err_t or_write_all(or_conn_t *c, const void *p, size_t n)
{
    const char *s = p;
    while (n > 0) {
        if (must_stop(c)) {
            return ESP_ERR_TIMEOUT;
        }
        int piece = n > WRITE_PIECE ? WRITE_PIECE : (int)n;
        int w = esp_http_client_write(c->h, s, piece);
        if (w <= 0) {
            c->r->phase = OR_PH_WRITE;
            c->r->err = ESP_ERR_HTTP_WRITE_DATA;
            grab_tls_error(c);
            return ESP_ERR_HTTP_WRITE_DATA;
        }
        s += w;
        n -= (size_t)w;
        c->r->bytes_sent += (size_t)w;
    }
    return ESP_OK;
}

esp_err_t or_write_b64_wav(or_conn_t *c, const int16_t *pcm, size_t samples, uint32_t rate)
{
    enum { RAW = 3072, OUT = 4096 };
    uint8_t *raw = heap_caps_malloc(RAW + OUT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        raw = malloc(RAW + OUT);
    }
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    char *out = (char *)raw + RAW;
    uint8_t hdr[44];
    or_wav_header(hdr, rate, samples);
    // Two segments (header, PCM) feed one base64 stream; 0-2 bytes carry over the boundary.
    const uint8_t *seg[2] = { hdr, (const uint8_t *)pcm };
    size_t seg_len[2] = { sizeof(hdr), samples * sizeof(int16_t) };
    size_t fill = 0;
    esp_err_t err = ESP_OK;
    for (int s = 0; s < 2 && err == ESP_OK; s++) {
        size_t off = 0;
        while (off < seg_len[s]) {
            size_t take = seg_len[s] - off < RAW - fill ? seg_len[s] - off : RAW - fill;
            memcpy(raw + fill, seg[s] + off, take);
            fill += take;
            off += take;
            if (fill == RAW) {
                size_t o = or_b64_encode(raw, RAW, out);
                err = or_write_all(c, out, o);
                fill = 0;
                if (err != ESP_OK) {
                    break;
                }
            }
        }
    }
    if (err == ESP_OK && fill) {
        size_t o = or_b64_encode(raw, fill, out);
        err = or_write_all(c, out, o);
    }
    free(raw);
    return err;
}

esp_err_t or_fetch_headers(or_conn_t *c)
{
    or_resp_t *r = c->r;
    r->phase = OR_PH_FIRST_BYTE;
    r->t_sent_us = now_us();
    int64_t fb_end = r->t_sent_us + (int64_t)c->q->to.first_byte_ms * 1000;
    esp_http_client_set_timeout_ms(c->h, TICK_MS);
    for (;;) {
        int64_t n = esp_http_client_fetch_headers(c->h);
        if (n >= 0) {
            break;
        }
        if (n != -ESP_ERR_HTTP_EAGAIN) {
            r->err = ESP_ERR_HTTP_FETCH_HEADER;   // closed or reset before any response
            grab_tls_error(c);
            return r->err;
        }
        if (must_stop(c)) {
            return ESP_ERR_TIMEOUT;
        }
        if (now_us() > fb_end) {
            r->err = ESP_ERR_HTTP_EAGAIN;
            return r->err;
        }
    }
    r->status = esp_http_client_get_status_code(c->h);
    r->content_length = esp_http_client_is_chunked_response(c->h) ? -1 : esp_http_client_get_content_length(c->h);
    r->t_headers_us = now_us();
    c->last_data_us = r->t_headers_us;
    r->phase = OR_PH_BODY;
    return ESP_OK;
}

void or_touch(or_conn_t *c)
{
    c->last_data_us = now_us();
}

int or_read(or_conn_t *c, void *buf, int len)
{
    or_resp_t *r = c->r;
    for (;;) {
        if (must_stop(c)) {
            return -1;
        }
        int n = esp_http_client_read(c->h, buf, len);
        if (n > 0) {
            c->last_data_us = now_us();
            r->bytes_recv += (size_t)n;
            return n;
        }
        if (n == 0) {
            if (!esp_http_client_is_complete_data_received(c->h)) {
                r->err = ESP_ERR_HTTP_INCOMPLETE_DATA;
                return -1;
            }
            r->phase = OR_PH_DONE;
            r->t_done_us = now_us();
            return 0;
        }
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (now_us() - c->last_data_us > (int64_t)c->q->to.stall_ms * 1000) {
                r->err = r->bytes_recv ? ESP_ERR_HTTP_READ_TIMEOUT : ESP_ERR_HTTP_EAGAIN;
                return -1;
            }
            continue;
        }
        r->err = ESP_ERR_HTTP_INCOMPLETE_DATA;
        grab_tls_error(c);
        return -1;
    }
}

esp_err_t or_read_body(or_conn_t *c, char **out, size_t cap, size_t *len)
{
    *out = NULL;
    if (len) {
        *len = 0;
    }
    char *b = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    for (;;) {
        int got;
        if (n < cap) {
            got = or_read(c, b + n, (int)(cap - n));
        } else {
            char sink[256];     // over the cap: drain so the socket stays reusable
            got = or_read(c, sink, sizeof(sink));
            if (got > 0) {
                c->r->too_big = true;
                continue;
            }
        }
        if (got < 0) {
            free(b);
            return c->r->err != ESP_OK ? c->r->err : ESP_FAIL;
        }
        if (got == 0) {
            break;
        }
        n += (size_t)got;
    }
    b[n] = 0;
    *out = b;
    if (len) {
        *len = n;
    }
    return c->r->too_big ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t or_finish(or_conn_t *c, bool ok)
{
    bool keep = ok && c->r && c->r->phase == OR_PH_DONE && !c->r->conn_close &&
                esp_http_client_is_complete_data_received(c->h) && esp_http_client_is_persistent_connection(c->h);
    if (keep) {
        c->live = true;
        c->last_used_us = now_us();
    } else {
        esp_http_client_close(c->h);
        c->live = false;
    }
    c->q = NULL;
    c->r = NULL;
    return ESP_OK;
}

esp_err_t or_start(or_conn_t *c, const or_req_t *q, or_resp_t *r)
{
    esp_err_t err = ESP_FAIL;
    for (int pass = 0; pass < 2; pass++) {
        err = or_begin(c, q, r);
        if (err == ESP_OK && q->write_body) {
            err = q->write_body(c, q->body_ctx);
        }
        if (err == ESP_OK) {
            err = or_fetch_headers(c);
        }
        if (err == ESP_OK) {
            return ESP_OK;
        }
        // A kept-alive socket the server already closed fails during the upload (the RST comes
        // back after one round trip, while the send buffer, up to 23 KB, still accepts data) or
        // with an immediate FIN: reconnect once without reporting a retry (protocol §1.3, N5).
        bool dead_reuse = r->reused && !r->aborted && !r->deadline &&
                          (r->phase == OR_PH_WRITE ||
                           (r->phase == OR_PH_FIRST_BYTE && r->err == ESP_ERR_HTTP_FETCH_HEADER));
        esp_http_client_close(c->h);
        c->live = false;
        if (!dead_reuse || pass) {
            break;
        }
        ESP_LOGI(TAG, "kept-alive socket was closed by the server, reconnecting");
    }
    return err;
}

esp_err_t or_exchange(const or_req_t *q, or_resp_t *r, char **body, size_t cap, size_t *len)
{
    *body = NULL;
    if (len) {
        *len = 0;
    }
    memset(r, 0, sizeof(*r));
    or_conn_t *c = or_conn_get(q->url);
    if (!c) {
        r->phase = OR_PH_CONNECT;
        r->err = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    // Wait for the connection (TTS may be streaming), still honouring abort.
    int64_t give_up = now_us() + (int64_t)q->to.total_ms * 1000;
    while (!or_conn_lock(c, 100)) {
        if ((q->abort && q->abort(q->abort_ctx)) || now_us() > give_up) {
            r->aborted = q->abort && q->abort(q->abort_ctx);
            r->deadline = !r->aborted;
            r->phase = OR_PH_CONNECT;
            r->err = ESP_ERR_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
    }
    esp_err_t err = or_start(c, q, r);
    if (err == ESP_OK) {
        err = or_read_body(c, body, r->status == 200 ? cap : (cap < OR_CAP_ERROR ? cap : OR_CAP_ERROR), len);
        if (err == ESP_ERR_INVALID_SIZE) {
            err = ESP_OK;     // truncated to the cap; the socket was drained and stays usable
        }
        or_finish(c, err == ESP_OK);
    } else {
        or_finish(c, false);
    }
    or_conn_unlock(c);
    return err;
}

// ---- base64 / WAV -----------------------------------------------------------------------------

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t or_b64_len(size_t n)
{
    return 4 * ((n + 2) / 3);
}

size_t or_wav_b64_len(size_t samples)
{
    return or_b64_len(44 + samples * sizeof(int16_t));
}

size_t or_b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    return o;
}

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

void or_wav_header(uint8_t hdr[44], uint32_t rate, size_t samples)
{
    uint32_t data = (uint32_t)(samples * sizeof(int16_t));
    memcpy(hdr, "RIFF", 4);
    put_le(hdr + 4, 36 + data, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    put_le(hdr + 16, 16, 4);
    put_le(hdr + 20, 1, 2);            // PCM
    put_le(hdr + 22, 1, 2);            // mono
    put_le(hdr + 24, rate, 4);
    put_le(hdr + 28, rate * 2, 4);
    put_le(hdr + 32, 2, 2);
    put_le(hdr + 34, 16, 2);
    memcpy(hdr + 36, "data", 4);
    put_le(hdr + 40, data, 4);
}
