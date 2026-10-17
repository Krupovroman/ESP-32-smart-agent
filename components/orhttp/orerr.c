// Error table and retry policy of protocol §9.1.
#include "orhttp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"

#define RETRY_BUDGET_US  (20LL * 1000 * 1000)    // at most 20 s of retries per user action
#define RETRY_AFTER_MAX  10000                    // Retry-After is honoured up to 10 s

typedef struct {
    const char *id;
    uint8_t retries;
    uint16_t d0, d1;          // delays in ms (0 = backoff formula)
    const char *msg;          // final message
    const char *msg_retry;    // while retrying, NULL = msg
} row_t;

// Message texts are the user-facing Russian strings of protocol §9.1. Rows that only give a
// "Повторяю…" text get the same sentence with "Попробуйте ещё раз." as the final message.
static const row_t ROWS[] = {
    { "N0", 0, 0, 0, "Нет Wi-Fi. Подключитесь в Настройках.", NULL },
    { "N1", 2, 1000, 3000, "Сервер не найден. Проверьте интернет.", NULL },
    { "N2", 2, 1000, 3000, "Сервер недоступен. Попробуйте позже.", "Нет связи с сервером. Повторяю…" },
    { "N3", 0, 0, 0, "Ошибка сертификата сервера. Нужно обновить прошивку.", NULL },
    { "N4", 1, 2000, 0, "Не удалось установить защищённое соединение.", NULL },
    { "N5", 1, 0, 0, "Связь оборвалась при отправке. Попробуйте ещё раз.", "Связь оборвалась при отправке. Повторяю…" },
    { "N6", 1, 0, 0, "Нет ответа от ИИ. Попробуйте ещё раз.", "ИИ долго не отвечает…" },
    { "N7", 1, 0, 0, "Ответ оборвался. Попробуйте ещё раз.", "Ответ оборвался. Повторяю…" },
    { "N8", 0, 0, 0, "Слишком долго. Попробуйте ещё раз.", NULL },
    { "400", 0, 0, 0, "Сервер отклонил запрос. Проверьте модель и голос в Настройках.", NULL },
    { "400ctx", 1, 0, 0, "Разговор слишком длинный — начинаю новый.", NULL },
    { "401", 0, 0, 0, "Неверный API-ключ. Введите ключ заново в Настройках.", NULL },
    { "402w", 1, 0, 0, "Подождите пару секунд…", NULL },
    { "402k", 0, 0, 0, "Исчерпан лимит этого ключа. Увеличьте его на openrouter.ai.", NULL },
    { "402", 0, 0, 0, "Закончились средства на OpenRouter. Пополните баланс.", NULL },
    { "403b", 0, 0, 0, "Исчерпан бюджет ключа.", NULL },
    { "403r", 0, 0, 0, "Модель недоступна для вашего региона. Выберите модель другого производителя.", NULL },
    { "403m", 0, 0, 0, "Запрос отклонён фильтром безопасности.", NULL },
    { "403", 0, 0, 0, "Доступ запрещён (403).", NULL },
    { "404", 0, 0, 0, "Модель не найдена или не поддерживает голос и JSON. Выберите другую.", NULL },
    { "408", 1, 1000, 0, "Сервер не дождался запроса. Попробуйте ещё раз.", "Сервер не дождался запроса. Повторяю…" },
    { "413", 0, 0, 0, "Запись слишком длинная. Говорите короче.", NULL },
    { "422", 0, 0, 0, "Сервер не смог обработать запрос.", NULL },
    { "429", 2, 2000, 5000, "Лимит запросов. Попробуйте через минуту.", "Слишком много запросов. Подождите…" },
    { "500", 1, 1500, 0, "Ошибка сервера OpenRouter. Попробуйте ещё раз.", "Ошибка сервера OpenRouter. Повторяю…" },
    { "502", 2, 1500, 4000, "Модель временно недоступна. Попробуйте позже.", "Модель временно недоступна. Повторяю…" },
    { "503", 1, 3000, 0, "Нет доступных серверов для модели. Попробуйте позже.", NULL },
    { "504", 1, 2000, 0, "Модель не ответила вовремя. Попробуйте ещё раз.", "Модель не ответила вовремя. Повторяю…" },
    { "529", 2, 3000, 6000, "Сервис перегружен. Попробуйте позже.", "Сервис перегружен. Повторяю…" },
    { "HTTP", 0, 0, 0, "Сервер вернул ошибку.", NULL },
    { "P2", 1, 0, 0, "Не удалось разобрать ответ ИИ.", NULL },
    { "P4", 0, 0, 0, "Не расслышал. Повторите, пожалуйста.", NULL },
    { "P5", 0, 0, 0, "ИИ отказался отвечать на этот запрос.", NULL },
    { "T1", 0, 0, 0, "Озвучка недоступна", NULL },
    { "T2", 0, 0, 0, "Этот голос отдаёт неподдерживаемый формат.", NULL },
    { "T3", 0, 0, 0, "Озвучка на этом языке недоступна", NULL },
    { "K0", 0, 0, 0, "Нет API-ключа. Введите ключ в Настройках.", NULL },
    { "M0", 0, 0, 0, "Не хватает памяти.", NULL },
    { "AB", 0, 0, 0, "Отменено.", NULL },
};

static const row_t *row_find(const char *id)
{
    const row_t *generic = NULL;
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        if (!strcmp(ROWS[i].id, id)) {
            return &ROWS[i];
        }
        if (!strcmp(ROWS[i].id, "HTTP")) {
            generic = &ROWS[i];
        }
    }
    return generic;
}

static void apply_row(or_err_t *e, const char *id)
{
    const row_t *r = row_find(id);
    strlcpy(e->id, id, sizeof(e->id));
    e->max_retries = r->retries;
    e->retry = r->retries > 0;
    e->retry_ms[0] = r->d0;
    e->retry_ms[1] = r->d1;
    strlcpy(e->msg_ru, r->msg, sizeof(e->msg_ru));
    strlcpy(e->msg_retry_ru, r->msg_retry ? r->msg_retry : "", sizeof(e->msg_retry_ru));
}

void or_err_local(or_err_t *e, const char *id)
{
    memset(e, 0, sizeof(*e));
    e->retry_after_ms = -1;
    apply_row(e, id);
}

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

static bool time_synced(time_t t)
{
    return t > 1735689600;    // 2025-01-01: SNTP (or the RTC) has set the clock
}

static int month_index(const char *m)
{
    static const char *M[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < 12; i++) {
        if (!strncasecmp(m, M[i], 3)) {
            return i;
        }
    }
    return -1;
}

// Days since 1970-01-01 of a civil date (proleptic Gregorian).
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Retry-After is integer seconds or an HTTP-date; X-RateLimit-Reset is an epoch (s or ms).
static int32_t parse_wait_ms(const or_resp_t *r)
{
    time_t now = time(NULL);
    if (r->retry_after[0]) {
        char *end = NULL;
        long s = strtol(r->retry_after, &end, 10);
        if (end != r->retry_after && (*end == 0 || *end == ' ')) {
            return s < 0 ? 0 : (s > 86400 ? 86400000 : (int32_t)(s * 1000));
        }
        // "Sun, 06 Nov 1994 08:49:37 GMT"
        int d, y, hh, mm, ss;
        char mon[4] = "";
        const char *p = strchr(r->retry_after, ',');
        if (p && sscanf(p + 1, "%d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) == 6 && time_synced(now)) {
            int mi = month_index(mon);
            if (mi >= 0) {
                int64_t t = days_from_civil(y, mi + 1, d) * 86400 + hh * 3600 + mm * 60 + ss;
                int64_t w = (t - (int64_t)now) * 1000;
                return w < 0 ? 0 : (w > 86400000 ? 86400000 : (int32_t)w);
            }
        }
    }
    if (r->rl_reset[0] && time_synced(now)) {
        double v = strtod(r->rl_reset, NULL);
        if (v > 1e12) {
            v /= 1000.0;      // milliseconds since the epoch
        }
        if (v > 1e9) {
            double w = (v - (double)now) * 1000.0;
            return w < 0 ? 0 : (w > 86400000.0 ? 86400000 : (int32_t)w);
        }
    }
    return -1;
}

static void classify_transport(or_kind_t kind, const or_resp_t *r, or_err_t *e)
{
    const char *id;
    switch (r->phase) {
    case OR_PH_CONNECT:
        if (r->tls_err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
            id = "N1";
        } else if (r->tls_err == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED && r->tls_flags != 0) {
            id = "N3";
        } else if (r->tls_err == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED ||
                   r->tls_err == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT) {
            id = "N4";
        } else {
            id = "N2";
        }
        break;
    case OR_PH_WRITE:
        id = "N5";
        break;
    case OR_PH_FIRST_BYTE:
        id = r->err == ESP_ERR_HTTP_EAGAIN ? "N6" : "N7";
        break;
    default:
        id = r->bytes_recv == 0 && (r->err == ESP_ERR_HTTP_EAGAIN || r->err == ESP_ERR_HTTP_READ_TIMEOUT) ? "N6" : "N7";
        break;
    }
    apply_row(e, id);
    if (!strcmp(id, "N6") && kind == OR_KIND_GET) {
        e->max_retries = 2;
    }
    snprintf(e->message, sizeof(e->message), "%s tls=0x%x code=-0x%x flags=0x%x", esp_err_to_name(r->err),
             (unsigned)r->tls_err, (unsigned)(r->tls_code < 0 ? -r->tls_code : r->tls_code), (unsigned)r->tls_flags);
}

static const char *json_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// Formats an ISO date "2026-10-12T00:00:00Z" as "12.10.2026".
static void fmt_date(const char *iso, char *out, size_t len)
{
    int y, m, d;
    if (iso && sscanf(iso, "%d-%d-%d", &y, &m, &d) == 3) {
        snprintf(out, len, "%02d.%02d.%04d", d, m, y);
    } else {
        out[0] = 0;
    }
}

static void classify_http(or_kind_t kind, const or_resp_t *r, const char *body, or_err_t *e)
{
    int code = r->status;
    const char *msg = NULL, *etype = NULL, *lsrc = NULL, *resets = NULL;
    bool moderation = false;
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    const cJSON *err = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
    if (cJSON_IsObject(err)) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(err, "code");
        if (cJSON_IsNumber(c) && (code == 200 || code == 0) && c->valuedouble >= 100) {
            code = (int)c->valuedouble;     // P1: error inside a 200 body
        }
        msg = json_str(err, "message");
        const cJSON *md = cJSON_GetObjectItemCaseSensitive(err, "metadata");
        if (cJSON_IsObject(md)) {
            etype = json_str(md, "error_type");
            lsrc = json_str(md, "limit_source");
            resets = json_str(md, "resets_at");
            moderation = cJSON_GetObjectItemCaseSensitive(md, "reasons") != NULL ||
                         cJSON_GetObjectItemCaseSensitive(md, "flagged_input") != NULL;
        }
    }
    if (code == 200 || code == 0) {
        code = 500;    // a 200 "error" without a usable code
    }
    e->http = code;
    strlcpy(e->message, msg ? msg : "", sizeof(e->message));
    strlcpy(e->error_type, etype ? etype : "", sizeof(e->error_type));
    strlcpy(e->limit_source, lsrc ? lsrc : "", sizeof(e->limit_source));
    int32_t wait = parse_wait_ms(r);

    const char *id = "HTTP";
    switch (code) {
    case 400:
        id = etype && !strcmp(etype, "context_length_exceeded") ? "400ctx" : "400";
        break;
    case 401:
        id = "401";
        break;
    case 402:
        if (lsrc && !strcmp(lsrc, "openrouter_in_flight_budget")) {
            id = "402w";
        } else if (lsrc && !strcmp(lsrc, "openrouter_key_limit")) {
            id = "402k";
        } else {
            id = "402";
        }
        break;
    case 403:
        if (lsrc && (!strcmp(lsrc, "openrouter_guardrail_budget") || !strcmp(lsrc, "openrouter_workspace_budget") ||
                     !strcmp(lsrc, "openrouter_key_limit"))) {
            id = "403b";
        } else if (contains_ci(msg, "region")) {
            id = "403r";
        } else if (moderation || (etype && (!strcmp(etype, "content_policy_violation") || !strcmp(etype, "refusal") ||
                                            !strcmp(etype, "moderation")))) {
            id = "403m";
        } else {
            id = "403";
        }
        break;
    case 404:
        // OpenRouter always answers JSON; a bare 404 is a wrong base URL (e.g. the relay's secret path)
        id = cJSON_IsObject(err) ? "404" : "HTTP";
        break;
    case 408: id = "408"; break;
    case 413: id = "413"; break;
    case 422: id = "422"; break;
    case 429: id = "429"; break;
    case 500: id = "500"; break;
    case 502: id = "502"; break;
    case 503: id = "503"; break;
    case 504:
    case 524: id = "504"; break;
    case 529: id = "529"; break;
    default:
        id = code >= 500 ? "500" : "HTTP";
        break;
    }
    apply_row(e, id);
    if (!strcmp(id, "HTTP")) {
        snprintf(e->msg_ru, sizeof(e->msg_ru), "Сервер вернул ошибку %d.", code);
        snprintf(e->id, sizeof(e->id), "%u", (unsigned)code % 1000u);
    }
    if (!strcmp(id, "403b")) {
        char date[16];
        fmt_date(resets, date, sizeof(date));
        if (date[0]) {
            snprintf(e->msg_ru, sizeof(e->msg_ru), "Исчерпан бюджет ключа до %s.", date);
        }
    }
    // Retry-After: honoured when <= 10 s; a longer wait means no automatic retry.
    if (!strcmp(id, "429") || !strcmp(id, "503") || !strcmp(id, "402w")) {
        if (wait > RETRY_AFTER_MAX) {
            e->retry = false;
        } else if (wait >= 0) {
            e->retry_after_ms = wait;
        } else if (!strcmp(id, "402w")) {
            e->retry = false;        // in-flight budget retries only after a short Retry-After
        }
    }
    (void)kind;
    cJSON_Delete(root);
}

void or_classify(or_kind_t kind, const or_resp_t *r, const char *body, or_err_t *e)
{
    memset(e, 0, sizeof(*e));
    e->retry_after_ms = -1;
    e->transport = r->err;
    if (r->aborted) {
        apply_row(e, "AB");
        return;
    }
    if (r->deadline) {
        apply_row(e, "N8");
        return;
    }
    if (r->err != ESP_OK || r->status == 0) {
        classify_transport(kind, r, e);
        return;
    }
    classify_http(kind, r, body, e);
}

bool or_body_has_error(const char *body)
{
    if (!body) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    bool has = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "error"));
    cJSON_Delete(root);
    return has;
}

void or_retry_init(or_retry_t *s, bool background)
{
    memset(s, 0, sizeof(*s));
    s->background = background;
}

static uint32_t jitter(uint32_t ms)
{
    // U(0.8, 1.2)
    uint32_t r = esp_random() % 401;
    return (uint32_t)((uint64_t)ms * (800 + r) / 1000);
}

static bool bg_retryable(const char *id)
{
    static const char *ok[] = { "N1", "N2", "N4", "N5", "N6", "N7", "408", "429", "500", "502", "503", "504", "529" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        if (!strcmp(id, ok[i])) {
            return true;
        }
    }
    return false;
}

bool or_retry_next(or_retry_t *s, const or_err_t *e, int64_t deadline_us, uint32_t *delay_ms)
{
    *delay_ms = 0;
    int64_t now = esp_timer_get_time();
    if (!s->t_first_fail_us) {
        s->t_first_fail_us = now;
    }
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(s->ids[i].id, e->id)) {
            slot = i;
            break;
        }
        if (!s->ids[i].id[0]) {
            strlcpy(s->ids[i].id, e->id, sizeof(s->ids[i].id));
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return false;
    }
    uint8_t k = s->ids[slot].n;
    uint32_t d;
    if (s->background) {
        // Background GETs: 3 silent retries with 2, 4 and 8 s (protocol §9.1).
        if (!bg_retryable(e->id) || k >= 3 || (!strcmp(e->id, "429") && !e->retry)) {
            return false;
        }
        d = 2000u << k;
    } else {
        if (!e->retry || k >= e->max_retries) {
            return false;
        }
        if (e->retry_after_ms >= 0) {
            d = (uint32_t)e->retry_after_ms;
        } else if (k < 2 && e->retry_ms[k]) {
            d = jitter(e->retry_ms[k]);
        } else {
            uint32_t b = 1000u << k;
            d = jitter(b > 8000 ? 8000 : b);
        }
        if (now + (int64_t)d * 1000 - s->t_first_fail_us > RETRY_BUDGET_US) {
            return false;
        }
    }
    if (deadline_us && now + (int64_t)d * 1000 >= deadline_us) {
        return false;
    }
    s->ids[slot].n++;
    s->total++;
    *delay_ms = d;
    return true;
}
