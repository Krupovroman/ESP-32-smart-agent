// Host stand-ins: libc gaps, FreeRTOS, settings, net, audio, and a scripted esp_http_client.
#include "mocks.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "net.h"
#include "settings.h"
#include "audio_in.h"
#include "audio_out.h"

int g_fails, g_checks;
int host_fail(const char *file, int line, const char *what)
{
    g_fails++;
    printf("  FAIL %s:%d: %s\n", file, line, what);
    return 0;
}

// ---- libc ----
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}
size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t d = strlen(dst);
    return d + strlcpy(dst + d, src, size > d ? size - d : 0);
}
struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }
int setenv(const char *k, const char *v, int o) { (void)o; return _putenv_s(k, v); }
char *strtok_r(char *s, const char *d, char **save) { return strtok_s(s, d, save); }

int g_quiet_log = 1;
void host_log(const char *lvl, const char *tag, const char *fmt, ...)
{
    if (g_quiet_log && lvl[0] == 'I') {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    printf("    [%s %s] ", lvl, tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
const char *esp_err_to_name(esp_err_t e)
{
    static char b[16];
    snprintf(b, sizeof(b), "0x%x", (unsigned)e);
    return b;
}
int64_t g_now_us = 1000000;
int64_t esp_timer_get_time(void) { return g_now_us; }
void vTaskDelay(TickType_t t) { g_now_us += (int64_t)(t ? t : 1) * 1000; }
uint32_t esp_random(void) { return (uint32_t)rand() * 2654435761u; }
esp_err_t esp_crt_bundle_attach(void *c) { (void)c; return ESP_OK; }

// ---- FreeRTOS ----
static int s_dummy;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_dummy; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b) { return b; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *b) { return b; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t sz) { (void)n; (void)sz; return &s_dummy; }
BaseType_t xQueueSend(QueueHandle_t q, const void *i, TickType_t t) { (void)q; (void)i; (void)t; return pdTRUE; }
BaseType_t xQueueReceive(QueueHandle_t q, void *i, TickType_t t) { (void)q; (void)i; (void)t; return pdFALSE; }
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *n, uint32_t s, void *a, UBaseType_t p,
                                           TaskHandle_t *h, BaseType_t c, uint32_t caps)
{
    (void)fn; (void)n; (void)s; (void)a; (void)p; (void)c; (void)caps;
    if (h) *h = &s_dummy;
    return pdPASS;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_dummy; }   // tests run "in" the worker task
EventGroupHandle_t xEventGroupCreate(void) { return &s_dummy; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b) { (void)g; return b; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b) { (void)g; return b; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t b, BaseType_t c, BaseType_t a, TickType_t t)
{
    (void)g; (void)c; (void)a; (void)t;
    return b;
}

// ---- settings (table from components/settings, values in RAM) ----
#define S(k, t, dn, ds, mn, mx, ml, fl) { k, t, dn, ds, mn, mx, ml, fl },
static const setting_desc_t s_table[] = {
#include "settings_table.inc"
    // keys of the architecture table that the current table lacks, to exercise them
    S("hist_idle_min", ST_U8, 10, NULL, 0, 255, 0, 0)
    S("tr_other", ST_U8, 0, NULL, 0, 1, 0, 0)
    S("tz", ST_STR, 0, "MSK-3", 0, 0, 32, 0)
    S("vo_uk", ST_STR, 0, "", 0, 0, 128, 0)
};
#undef S
static struct {
    char key[16];
    char *val;
} s_over[64];
static int s_n_over;

void host_settings_reset(void)
{
    for (int i = 0; i < s_n_over; i++) {
        free(s_over[i].val);
    }
    s_n_over = 0;
}
void host_set(const char *key, const char *val)
{
    for (int i = 0; i < s_n_over; i++) {
        if (!strcmp(s_over[i].key, key)) {
            free(s_over[i].val);
            s_over[i].val = _strdup(val);
            return;
        }
    }
    strlcpy(s_over[s_n_over].key, key, sizeof(s_over[0].key));
    s_over[s_n_over++].val = _strdup(val);
}
static const char *over(const char *key)
{
    for (int i = 0; i < s_n_over; i++) {
        if (!strcmp(s_over[i].key, key)) return s_over[i].val;
    }
    return NULL;
}
const setting_desc_t *settings_find(const char *key)
{
    for (size_t i = 0; i < sizeof(s_table) / sizeof(s_table[0]); i++) {
        if (!strcmp(s_table[i].key, key)) return &s_table[i];
    }
    return NULL;
}
int32_t settings_get_int(const char *key)
{
    const setting_desc_t *d = settings_find(key);
    const char *o = over(key);
    return o ? atoi(o) : (d ? (int32_t)d->def_num : 0);
}
size_t settings_get_str(const char *key, char *buf, size_t len)
{
    const setting_desc_t *d = settings_find(key);
    const char *o = over(key);
    strlcpy(buf, o ? o : (d && d->def_str ? d->def_str : ""), len);
    return strlen(buf);
}

// ---- net / audio ----
bool g_net_up = true;
void net_status(net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = g_net_up ? NET_UP : NET_OFF;
}
uint32_t g_out_rate, g_out_begins;
uint8_t g_out_ch;
size_t g_out_bytes, g_out_stops;
uint8_t *g_out_buf;
static size_t s_out_cap;
esp_err_t audio_out_begin(uint32_t rate, uint8_t ch)
{
    g_out_rate = rate;
    g_out_ch = ch;
    g_out_begins++;
    return ESP_OK;
}
size_t audio_out_write(const void *p, size_t n, TickType_t w)
{
    (void)w;
    if (g_out_bytes + n > s_out_cap) {
        s_out_cap = (g_out_bytes + n) * 2;
        g_out_buf = realloc(g_out_buf, s_out_cap);
    }
    memcpy(g_out_buf + g_out_bytes, p, n);
    g_out_bytes += n;
    return n;
}
void audio_out_stop(void) { g_out_stops++; }
bool audio_out_busy(void) { return false; }
const audio_clip_t *audio_in_last(void) { return NULL; }

// ---- esp_http_client mock ----
mock_reply_t g_script[MOCK_MAX];
int g_script_n, g_script_pos;
mock_req_t g_reqs[MOCK_MAX];
int g_nreq, g_connects, g_len_mismatch;

struct mock_client {
    http_event_handle_cb cb;
    void *user;
    char url[300];
    char host[128];
    esp_http_client_method_t method;
    struct { char k[40]; char v[256]; } h[16];
    int nh;
    int timeout_ms;
    bool connected;
    mock_reply_t *rep;
    mock_req_t *req;
    size_t pos;
    int reads;
    int tls, tls_flags;
    bool cut;
};

void mock_reset(void)
{
    for (int i = 0; i < g_nreq; i++) free(g_reqs[i].body);
    memset(g_reqs, 0, sizeof(g_reqs));
    memset(g_script, 0, sizeof(g_script));
    g_script_n = g_script_pos = g_nreq = g_connects = g_len_mismatch = 0;
    g_out_bytes = 0;
    g_out_begins = 0;
    g_out_stops = 0;
}
void mock_push(const mock_reply_t *r) { g_script[g_script_n++] = *r; }
void mock_push_json(int status, const char *json)
{
    mock_reply_t r = { .status = status, .content_type = "application/json", .body = (const uint8_t *)json,
                       .body_len = strlen(json), .chunked = true };
    mock_push(&r);
}

static void evt(struct mock_client *c, esp_http_client_event_id_t id, const char *k, const char *v)
{
    if (!c->cb) return;
    esp_http_client_event_t e = { .event_id = id, .client = c, .user_data = c->user };
    e.header_key = (char *)k;
    e.header_value = (char *)v;
    c->cb(&e);
}
static const char *hget(struct mock_client *c, const char *k)
{
    for (int i = 0; i < c->nh; i++) if (!_stricmp(c->h[i].k, k)) return c->h[i].v;
    return "";
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    struct mock_client *c = calloc(1, sizeof(*c));
    c->cb = cfg->event_handler;
    c->user = cfg->user_data;
    c->timeout_ms = cfg->timeout_ms;
    return c;
}
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url)
{
    const char *h = strstr(url, "://");
    char host[128];
    h = h ? h + 3 : url;
    size_t n = strcspn(h, "/");
    snprintf(host, sizeof(host), "%.*s", (int)n, h);
    if (c->host[0] && strcmp(host, c->host)) esp_http_client_close(c);
    strlcpy(c->host, host, sizeof(c->host));
    strlcpy(c->url, url, sizeof(c->url));
    return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v)
{
    for (int i = 0; i < c->nh; i++) {
        if (!_stricmp(c->h[i].k, k)) { strlcpy(c->h[i].v, v, sizeof(c->h[i].v)); return ESP_OK; }
    }
    strlcpy(c->h[c->nh].k, k, sizeof(c->h[0].k));
    strlcpy(c->h[c->nh++].v, v, sizeof(c->h[0].v));
    return ESP_OK;
}
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *k)
{
    for (int i = 0; i < c->nh; i++) {
        if (!_stricmp(c->h[i].k, k)) { c->h[i] = c->h[--c->nh]; return ESP_OK; }
    }
    return ESP_OK;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m) { c->method = m; return ESP_OK; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int t) { c->timeout_ms = t; return ESP_OK; }

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    if (g_script_pos >= g_script_n) {
        printf("  mock: no scripted reply left\n");
        return ESP_ERR_HTTP_CONNECT;
    }
    c->rep = &g_script[g_script_pos];
    mock_req_t *q = &g_reqs[g_nreq++];
    c->req = q;
    strlcpy(q->method, c->method == HTTP_METHOD_POST ? "POST" : "GET", sizeof(q->method));
    strlcpy(q->url, c->url, sizeof(q->url));
    strlcpy(q->auth, hget(c, "Authorization"), sizeof(q->auth));
    strlcpy(q->accept, hget(c, "Accept"), sizeof(q->accept));
    strlcpy(q->ctype, hget(c, "Content-Type"), sizeof(q->ctype));
    strlcpy(q->title, hget(c, "X-OpenRouter-Title"), sizeof(q->title));
    q->content_length = write_len;
    q->reused = c->connected;
    q->body = calloc(1, (size_t)write_len + 1);
    if (!c->connected) {
        g_now_us += 300000;        // handshake
        if (c->rep->fail_connect) {
            c->tls = c->rep->tls_err;
            c->tls_flags = c->rep->tls_flags;
            g_script_pos++;
            return ESP_ERR_HTTP_CONNECT;
        }
        c->connected = true;
        g_connects++;
    }
    if (c->rep->fail_write_at_open) {
        g_script_pos++;
        esp_http_client_close(c);
        return ESP_ERR_HTTP_WRITE_DATA;
    }
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *b, int len)
{
    mock_req_t *q = c->req;
    if (q->len + (size_t)len > (size_t)q->content_length) {
        g_len_mismatch++;
        printf("  mock: body longer than Content-Length (%lld)\n", (long long)q->content_length);
        return -1;
    }
    memcpy(q->body + q->len, b, (size_t)len);
    q->len += (size_t)len;
    g_now_us += 200;
    return len;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    mock_reply_t *r = c->rep;
    if (c->req->len != (size_t)c->req->content_length && r->eagain_before_headers == 0) {
        g_len_mismatch++;
        printf("  mock: wrote %zu of Content-Length %lld\n", c->req->len, (long long)c->req->content_length);
    }
    if (r->eagain_before_headers > 0) {
        r->eagain_before_headers--;
        g_now_us += (int64_t)c->timeout_ms * 1000;
        return -ESP_ERR_HTTP_EAGAIN;
    }
    g_script_pos++;
    if (r->fin_before_headers) {
        return ESP_FAIL;
    }
    if (r->content_type) evt(c, HTTP_EVENT_ON_HEADER, "content-type", r->content_type);
    for (int i = 0; i < 4 && r->hdr_k[i]; i++) evt(c, HTTP_EVENT_ON_HEADER, r->hdr_k[i], r->hdr_v[i]);
    if (r->conn_close) evt(c, HTTP_EVENT_ON_HEADER, "Connection", "close");
    c->pos = 0;
    c->reads = 0;
    c->cut = false;
    g_now_us += 50000;
    return r->chunked ? 0 : (int64_t)r->body_len;
}
bool esp_http_client_is_chunked_response(esp_http_client_handle_t c) { return c->rep->chunked; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->rep->status; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) { return c->rep->chunked ? -1 : (int64_t)c->rep->body_len; }
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    mock_reply_t *r = c->rep;
    if (r->cut_after && c->pos >= r->cut_after) {
        c->cut = true;
        return ESP_FAIL;
    }
    if (r->eagain_every && (++c->reads % r->eagain_every) == 0) {
        g_now_us += (int64_t)c->timeout_ms * 1000;
        return -ESP_ERR_HTTP_EAGAIN;
    }
    size_t left = r->body_len - c->pos;
    if (!left) return 0;
    size_t n = (size_t)len < left ? (size_t)len : left;
    size_t piece = r->read_piece ? (size_t)r->read_piece : 700;
    if (n > piece) n = piece;
    if (r->cut_after && c->pos + n > r->cut_after) n = r->cut_after - c->pos;
    memcpy(buf, r->body + c->pos, n);
    c->pos += n;
    g_now_us += 1000;
    return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{
    return c->rep && !c->cut && c->pos == c->rep->body_len;
}
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t c) { return !c->rep->conn_close; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    // an abandoned request (timeout, abort) consumes its scripted reply
    if (c->rep && g_script_pos < g_script_n && c->rep == &g_script[g_script_pos]) {
        g_script_pos++;
    }
    if (c->connected) {
        c->connected = false;
        evt(c, HTTP_EVENT_DISCONNECTED, NULL, NULL);
    }
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { free(c); return ESP_OK; }
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c, int *code, int *flags)
{
    int e = c->tls;
    *code = 0;
    *flags = c->tls_flags;
    c->tls = 0;
    c->tls_flags = 0;
    return e;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4096; }
