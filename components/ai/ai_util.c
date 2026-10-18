// Small helpers: PSRAM allocation, settings with defaults, URLs, UTF-8, JSON.
#include "ai_priv.h"
#include "net.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void *ai_malloc(size_t n)
{
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
}

void *ai_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n ? n : 1, sz ? sz : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(n ? n : 1, sz ? sz : 1);
}

char *ai_strndup(const char *s, size_t n)
{
    if (!s) {
        s = "";
    }
    size_t l = strnlen(s, n);
    char *d = ai_malloc(l + 1);
    if (d) {
        memcpy(d, s, l);
        d[l] = 0;
    }
    return d;
}

char *ai_strdup(const char *s)
{
    return ai_strndup(s, s ? strlen(s) : 0);
}

int64_t ai_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

int32_t cfg_int(const char *key, int32_t def)
{
    const setting_desc_t *d = settings_find(key);
    if (!d || d->type == ST_STR) {
        return def;
    }
    return settings_get_int(key);
}

void cfg_str(const char *key, char *buf, size_t len, const char *def)
{
    const setting_desc_t *d = settings_find(key);
    if (!d || d->type != ST_STR) {
        strlcpy(buf, def ? def : "", len);
        return;
    }
    settings_get_str(key, buf, len);
}

void cfg_url(const char *key, char *buf, size_t len)
{
    cfg_str(key, buf, len, "");
    if (!buf[0]) {
        cfg_str("ai_url", buf, len, "https://openrouter.ai/api/v1");
    }
}

bool cfg_key(const char *key, const char *url, char *buf, size_t len)
{
    cfg_str(key, buf, len, "");
    if (buf[0]) {
        return true;
    }
    // ai_key belongs to the AI host: it never goes to a tts_url / stt_url on another host
    char ai[136], o_ai[100], o_url[100];
    cfg_str("ai_url", ai, sizeof(ai), "");
    if (!or_url_origin(url, o_url, sizeof(o_url)) ||
        !or_url_origin(ai[0] ? ai : "https://openrouter.ai/api/v1", o_ai, sizeof(o_ai)) || strcmp(o_url, o_ai)) {
        return false;
    }
    cfg_str("ai_key", buf, len, "");
    return true;
}

void ai_join_url(char *out, size_t len, const char *base, const char *path)
{
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') {
        bl--;
    }
    while (*path == '/') {
        path++;
    }
    snprintf(out, len, "%.*s/%s", (int)bl, base, path);
}

bool ai_net_up(void)
{
    net_status_t st;
    net_status(&st);
    return st.state == NET_UP;
}

bool ai_time_synced(void)
{
    return time(NULL) > 1735689600;    // 2025-01-01: SNTP or the RTC has set the clock
}

bool ai_sleep_ms(uint32_t ms, or_abort_fn ab, void *ctx)
{
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        if (ab && ab(ctx)) {
            return false;
        }
        int64_t left = (end - esp_timer_get_time()) / 1000;
        vTaskDelay(pdMS_TO_TICKS(left > 50 ? 50 : (left < 1 ? 1 : left)));
    }
    return !(ab && ab(ctx));
}

bool ai_contains_ci(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; s && *s; s++) {
        if (!strncasecmp(s, w, n)) {
            return true;
        }
    }
    return false;
}

const char *ai_json_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

double ai_json_num(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) {
        return v->valuedouble;
    }
    if (cJSON_IsString(v) && v->valuestring[0]) {
        return strtod(v->valuestring, NULL);   // OpenRouter prices are strings
    }
    return def;
}

// ---- UTF-8 ------------------------------------------------------------------------------------

uint32_t utf8_next(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t c = p[0];
    if (!c) {
        return 0;
    }
    int n;
    uint32_t cp;
    if (c < 0x80) {
        *pp += 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else {
        *pp += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *pp += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp += n + 1;
    return cp;
}

size_t utf8_put(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_cp_len(const char *s)
{
    size_t n = 0;
    while (s && utf8_next(&s)) {
        n++;
    }
    return n;
}

char *utf8_trunc_dup(const char *s, size_t max_cp)
{
    const char *p = s ? s : "";
    size_t n = 0;
    while (n < max_cp && utf8_next(&p)) {
        n++;
    }
    if (!*p) {
        return ai_strdup(s);
    }
    size_t bytes = (size_t)(p - s);
    char *d = ai_malloc(bytes + 4);
    if (d) {
        memcpy(d, s, bytes);
        memcpy(d + bytes, "\xE2\x80\xA6", 4);   // "…" + NUL
    }
    return d;
}

// ---- partial JSON string extraction -----------------------------------------------------------

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

// Decodes a JSON string body starting after the opening quote, up to the closing quote or the end.
static char *decode_partial(const char *s)
{
    char *out = ai_malloc(strlen(s) + 1);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    while (*s && *s != '"') {
        if (*s != '\\') {
            out[o++] = *s++;
            continue;
        }
        char e = s[1];
        if (!e) {
            break;
        }
        s += 2;
        switch (e) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'r': out[o++] = '\r'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'u': {
            int v = strnlen(s, 4) == 4 ? hex4(s) : -1;
            if (v < 0) {
                s += strnlen(s, 4);
                break;
            }
            s += 4;
            uint32_t cp = (uint32_t)v;
            if (cp >= 0xD800 && cp < 0xDC00 && s[0] == '\\' && s[1] == 'u' && strnlen(s + 2, 4) == 4) {
                int lo = hex4(s + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
                    s += 6;
                }
            }
            o += utf8_put(cp, out + o);
            break;
        }
        default: out[o++] = e; break;    // \" \\ \/
        }
    }
    out[o] = 0;
    return out;
}

char *ai_json_salvage(const char *js, const char *key)
{
    int depth = 0;
    size_t klen = strlen(key);
    const char *p = js;
    while (p && *p) {
        if (*p == '"') {
            const char *s = p + 1, *q = s;
            while (*q && *q != '"') {
                q += (*q == '\\' && q[1]) ? 2 : 1;
            }
            if (!*q) {
                return NULL;
            }
            p = q + 1;
            if (depth == 1 && (size_t)(q - s) == klen && !memcmp(s, key, klen)) {
                const char *r = p;
                while (*r == ' ' || *r == '\n' || *r == '\r' || *r == '\t') r++;
                if (*r != ':') continue;
                r++;
                while (*r == ' ' || *r == '\n' || *r == '\r' || *r == '\t') r++;
                if (*r == '"') {
                    return decode_partial(r + 1);
                }
            }
            continue;
        }
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            depth--;
        }
        p++;
    }
    return NULL;
}
