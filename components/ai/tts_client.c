// TTS: POST {tts_url}/audio/speech, PCM streamed into audio_out as it arrives (protocol §5).
#include "tts_client.h"
#include "ai_priv.h"
#include "audio_out.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "tts";

#define TTS_STACK     12288
#define TTS_PRIO      5
#define TTS_CORE      0
#define TTS_Q_LEN     16
#define READ_BUF      4096
#define OUT_POLL_MS   10            // ring full: wait this long, then retry (TCP throttles the server)
#define OUT_STALL_MS  10000
#define AHEAD_S       20            // a later chunk is requested once at most this much audio is queued
#define MAX_ROUTES    6

typedef struct {
    uint32_t turn;
    char *text;
    char lang[8];
    bool fetch;                     // collect PCM for test fixtures instead of playing it
    SemaphoreHandle_t done;
    esp_err_t rc;
    uint8_t *pcm;
    size_t pcm_len, pcm_cap;
    uint32_t rate;
} tts_job_t;

// Incremental parser of one response body: sniff RIFF, skip a WAV header, keep whole frames.
typedef struct {
    int mode;                       // 0 sniff, 1 raw PCM, 2 WAV header, 3 WAV data
    uint8_t hdr[512];
    size_t hdr_n;
    uint8_t carry[4];
    size_t carry_n;
    uint32_t rate;
    uint8_t ch;
    size_t skip;                    // bytes already played before a retry of this chunk
    size_t delivered;               // PCM bytes of this chunk seen so far (played or skipped)
    bool bad_format;
} pcm_parser_t;

typedef struct {
    tts_job_t *job;
    tts_stats_t *st;
    int64_t t0_us;
    pcm_parser_t *pp;
    bool ttfb_sent;
    bool out_failed;
} sink_t;

static QueueHandle_t s_q;
static TaskHandle_t s_task;
static uint32_t s_cancel_upto;
static uint32_t s_max_turn;         // highest turn ever queued
static int s_pending;
static uint32_t s_out_turn, s_out_rate;
static uint8_t s_out_ch;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static tts_stats_t s_last;

static bool cancelled(uint32_t turn)
{
    return turn && turn <= __atomic_load_n(&s_cancel_upto, __ATOMIC_RELAXED);
}

static bool abort_job(void *ctx)
{
    return cancelled(((tts_job_t *)ctx)->turn);
}

void tts_client_cancel(uint32_t turn)
{
    if (turn == 0) {
        turn = __atomic_load_n(&s_max_turn, __ATOMIC_RELAXED);   // everything queued so far
    }
    uint32_t cur = __atomic_load_n(&s_cancel_upto, __ATOMIC_RELAXED);
    while (turn > cur && !__atomic_compare_exchange_n(&s_cancel_upto, &cur, turn, false, __ATOMIC_RELAXED,
                                                      __ATOMIC_RELAXED)) {
    }
    if (s_out_turn && s_out_turn <= turn) {
        audio_out_stop();           // barge-in: drop what is queued for the speaker
        s_out_turn = 0;
    }
}

uint32_t tts_task_stack_free(void)
{
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}

bool tts_client_busy(void)
{
    return __atomic_load_n(&s_pending, __ATOMIC_RELAXED) > 0;
}

void tts_client_last_stats(tts_stats_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_last;
    taskEXIT_CRITICAL(&s_mux);
}

// ---- PCM sink ---------------------------------------------------------------------------------

static void emit(ai_ev_type_t type, uint32_t turn, int32_t i, const or_err_t *err, const tts_stats_t *st)
{
    ai_event_t ev = { .type = type, .turn = turn, .i = i, .err = err, .tts = st };
    ai_emit(&ev);
}

// The speaker keeps one stream per turn; begin() (which waits for a drain) only on a format change.
static esp_err_t out_begin(uint32_t turn, uint32_t rate, uint8_t ch)
{
    if (s_out_turn == turn && s_out_rate == rate && s_out_ch == ch) {
        return ESP_OK;
    }
    esp_err_t e = audio_out_begin(rate, ch);
    if (e == ESP_OK) {
        s_out_turn = turn;
        s_out_rate = rate;
        s_out_ch = ch;
    }
    return e;
}

// Never blocks inside the stream buffer: audio_out_stop() drains the ring, and a writer blocked on a
// full ring would refill it with the old reply as fast as the drain frees space.
static bool out_write(sink_t *s, const uint8_t *p, size_t n)
{
    int64_t last = esp_timer_get_time();
    while (n) {
        if (abort_job(s->job)) {
            return false;
        }
        size_t w = audio_out_write(p, n, 0);
        if (w) {
            p += w;
            n -= w;
            last = esp_timer_get_time();
        } else if (esp_timer_get_time() - last > (int64_t)OUT_STALL_MS * 1000) {
            ESP_LOGE(TAG, "speaker ring stalled");
            return false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(OUT_POLL_MS));
        }
    }
    return true;
}

// Before a later chunk of a long reply: with the 2 MB ring (~43 s) full, a chunk would download only as fast as it
// plays, and a slow 400-character chunk could run into the 60 s OR_TO_TTS total. So the request waits until at most
// AHEAD_S of audio is queued (still far more than the next chunk's first byte takes). False when cancelled.
static bool wait_ahead(tts_job_t *j)
{
    if (s_out_turn != j->turn || !s_out_rate) {
        return true;
    }
    size_t ahead = (size_t)s_out_rate * s_out_ch * 2 * AHEAD_S;
    size_t last = SIZE_MAX;
    int64_t t_last = esp_timer_get_time();
    for (;;) {
        if (abort_job(j)) {
            return false;
        }
        audio_out_stats_t st;
        audio_out_get_stats(&st);
        if (st.queued_bytes <= ahead) {
            return true;
        }
        if (st.queued_bytes < last) {
            last = st.queued_bytes;
            t_last = esp_timer_get_time();
        } else if (esp_timer_get_time() - t_last > (int64_t)OUT_STALL_MS * 1000) {
            return true;            // the speaker does not drain: out_write() reports it
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool push_pcm(sink_t *s, const uint8_t *p, size_t n)
{
    pcm_parser_t *pp = s->pp;
    tts_job_t *j = s->job;
    if (!n) {
        return true;
    }
    if (j->fetch) {
        if (!j->rate) {
            j->rate = pp->rate;
        }
        if (j->pcm_len + n > j->pcm_cap) {
            size_t cap = (j->pcm_cap ? j->pcm_cap * 2 : 256 * 1024);
            while (cap < j->pcm_len + n) {
                cap *= 2;
            }
            uint8_t *b = heap_caps_realloc(j->pcm, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!b) {
                return false;
            }
            j->pcm = b;
            j->pcm_cap = cap;
        }
        memcpy(j->pcm + j->pcm_len, p, n);
        j->pcm_len += n;
    } else {
        if (out_begin(j->turn, pp->rate, pp->ch) != ESP_OK || !out_write(s, p, n)) {
            s->out_failed = true;
            return false;
        }
    }
    s->st->bytes += (uint32_t)n;
    if (!s->ttfb_sent) {
        s->ttfb_sent = true;
        s->st->ttfb_ms = (uint32_t)((esp_timer_get_time() - s->t0_us) / 1000);
        s->st->rate = pp->rate;
        s->st->channels = pp->ch;
        emit(AI_EV_TTS_FIRST_AUDIO, j->turn, (int32_t)s->st->ttfb_ms, NULL, s->st);
    }
    return true;
}

// Whole frames only (1-byte carry for mono s16); bytes already played before a retry are skipped.
static bool deliver(sink_t *s, const uint8_t *p, size_t n)
{
    pcm_parser_t *pp = s->pp;
    size_t frame = (size_t)pp->ch * 2;
    uint8_t buf[512];
    while (n) {
        size_t take = n < sizeof(buf) - pp->carry_n ? n : sizeof(buf) - pp->carry_n;
        memcpy(buf, pp->carry, pp->carry_n);
        memcpy(buf + pp->carry_n, p, take);
        size_t have = pp->carry_n + take;
        size_t whole = have - have % frame;
        pp->carry_n = have - whole;
        memcpy(pp->carry, buf + whole, pp->carry_n);
        p += take;
        n -= take;
        size_t off = 0;
        if (pp->delivered < pp->skip) {
            size_t sk = pp->skip - pp->delivered;
            off = sk < whole ? sk : whole;
        }
        pp->delivered += whole;
        if (!push_pcm(s, buf + off, whole - off)) {
            return false;
        }
    }
    return true;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Looks for "fmt " and "data" in the buffered WAV header; true once the data chunk starts.
static bool wav_header(pcm_parser_t *pp, size_t *data_at)
{
    size_t o = 12;
    while (o + 8 <= pp->hdr_n) {
        uint32_t sz = rd32(pp->hdr + o + 4);
        if (!memcmp(pp->hdr + o, "fmt ", 4) && o + 8 + 16 <= pp->hdr_n) {
            uint16_t ch = (uint16_t)(pp->hdr[o + 10] | pp->hdr[o + 11] << 8);
            uint32_t rate = rd32(pp->hdr + o + 12);
            uint16_t bits = (uint16_t)(pp->hdr[o + 22] | pp->hdr[o + 23] << 8);
            if (bits != 16 || ch < 1 || ch > 2 || rate < 8000 || rate > 48000) {
                pp->bad_format = true;
                return false;
            }
            pp->ch = (uint8_t)ch;
            pp->rate = rate;
        } else if (!memcmp(pp->hdr + o, "data", 4)) {
            *data_at = o + 8;
            return true;
        }
        if (sz > sizeof(pp->hdr)) {
            pp->bad_format = true;    // cannot be skipped inside the header buffer (and would wrap o)
            return false;
        }
        o += 8 + sz + (sz & 1);
    }
    return false;
}

static bool feed(sink_t *s, const uint8_t *p, size_t n)
{
    pcm_parser_t *pp = s->pp;
    if (pp->mode == 1 || pp->mode == 3) {
        return deliver(s, p, n);
    }
    size_t take = n < sizeof(pp->hdr) - pp->hdr_n ? n : sizeof(pp->hdr) - pp->hdr_n;
    memcpy(pp->hdr + pp->hdr_n, p, take);
    pp->hdr_n += take;
    p += take;
    n -= take;
    if (pp->mode == 0) {
        if (pp->hdr_n < 12 && n == 0) {
            return true;              // wait for enough bytes to sniff
        }
        if (pp->hdr_n >= 12 && !memcmp(pp->hdr, "RIFF", 4) && !memcmp(pp->hdr + 8, "WAVE", 4)) {
            pp->mode = 2;
        } else {
            pp->mode = 1;
            size_t h = pp->hdr_n;
            pp->hdr_n = 0;
            return deliver(s, pp->hdr, h) && deliver(s, p, n);
        }
    }
    size_t at;
    if (wav_header(pp, &at)) {
        pp->mode = 3;
        size_t h = pp->hdr_n;
        pp->hdr_n = 0;
        return deliver(s, pp->hdr + at, h - at) && deliver(s, p, n);
    }
    if (pp->bad_format || pp->hdr_n == sizeof(pp->hdr)) {
        pp->bad_format = true;
        return false;
    }
    return n == 0 ? true : feed(s, p, n);
}

// ---- one chunk --------------------------------------------------------------------------------

typedef struct {
    char *js;
    size_t len;
} body_t;

static esp_err_t write_body(or_conn_t *c, void *ctx)
{
    body_t *b = ctx;
    return or_write_all(c, b->js, b->len);
}

static char *chunk_body(const char *model, const char *voice, const char *text, bool *speed_sent)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "model", model);
    cJSON_AddStringToObject(o, "input", text);
    if (voice && voice[0]) {
        cJSON_AddStringToObject(o, "voice", voice);
    }
    cJSON_AddStringToObject(o, "response_format", "pcm");
    // speed only when it is not 1.0 (range 0.8-1.25) and the model has not rejected it
    int sp = (int)cfg_int("tts_speed", 100);
    sp = sp < 80 ? 80 : (sp > 125 ? 125 : sp);
    *speed_sent = sp != 100 && !tts_speed_unsupported(model);
    if (*speed_sent) {
        cJSON_AddNumberToObject(o, "speed", sp / 100.0);
    }
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return js;
}

// Reads one response into the sink. *progress: in = PCM bytes of this chunk already played,
// out = bytes delivered so far, so a retry after a cut body resumes instead of repeating.
static esp_err_t stream_audio(sink_t *s, or_conn_t *c, or_resp_t *r, const char *model, size_t *progress,
                              or_err_t *err)
{
    char media[32];
    uint32_t ct_rate;
    uint8_t ct_ch;
    tts_parse_ct(r->content_type, media, sizeof(media), &ct_rate, &ct_ch);
    if (r->status != 200 || !strcmp(media, "application/json")) {
        char *eb = NULL;
        esp_err_t re = or_read_body(c, &eb, OR_CAP_ERROR, NULL);
        or_finish(c, re == ESP_OK || re == ESP_ERR_INVALID_SIZE);
        if (r->status == 200 && !or_body_has_error(eb)) {
            or_err_local(err, "T2");
        } else {
            or_classify(OR_KIND_TTS, r, eb, err);
        }
        free(eb);
        return ESP_FAIL;
    }
    if (media[0] && strcmp(media, "audio/pcm") && strcmp(media, "audio/l16") && strcmp(media, "audio/wav") &&
        strcmp(media, "audio/x-wav")) {
        ESP_LOGW(TAG, "unsupported %s", r->content_type);   // e.g. audio/mpeg: no decoder in v1
        or_finish(c, false);
        or_err_local(err, "T2");
        return ESP_FAIL;
    }
    pcm_parser_t pp = { .skip = *progress };
    pp.rate = tts_rate_for(model, r->content_type, &pp.ch);
    uint8_t *buf = ai_malloc(READ_BUF);
    if (!buf) {
        or_finish(c, false);
        or_err_local(err, "M0");
        return ESP_ERR_NO_MEM;
    }
    s->pp = &pp;
    bool ok = true;
    for (;;) {
        int n = or_read(c, buf, READ_BUF);
        if (n < 0) {
            or_classify(OR_KIND_TTS, r, NULL, err);
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        if (!feed(s, buf, (size_t)n)) {
            if (pp.bad_format) {
                or_err_local(err, "T2");
            } else if (abort_job(s->job)) {
                or_err_local(err, "AB");
            } else {
                or_err_local(err, s->out_failed ? "T1" : "M0");
            }
            ok = false;
            break;
        }
        or_touch(c);             // time spent waiting for the speaker is not a network stall
    }
    if (ok && pp.mode == 0 && pp.hdr_n) {
        size_t h = pp.hdr_n;     // very short body: flush what was held back for sniffing
        pp.mode = 1;
        pp.hdr_n = 0;
        ok = deliver(s, pp.hdr, h);
    }
    free(buf);
    s->pp = NULL;
    or_finish(c, ok);
    if (pp.delivered > *progress) {
        *progress = pp.delivered;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t speak_chunk(sink_t *s, const char *model, const char *voice, const char *text, size_t *progress,
                             bool *speed_sent, or_err_t *err)
{
    tts_job_t *j = s->job;
    char base[136], url[200], key[208], referer[136];
    cfg_url("tts_url", base, sizeof(base));
    ai_join_url(url, sizeof(url), base, "audio/speech");
    bool need_key = cfg_key("tts_key", base, key, sizeof(key));
    cfg_str("ref_url", referer, sizeof(referer), "");
    if (!key[0] && need_key) {
        or_err_local(err, "K0");
        return ESP_FAIL;
    }
    if (!ai_net_up()) {
        or_err_local(err, "N0");
        return ESP_FAIL;
    }
    body_t b = { .js = chunk_body(model, voice, text, speed_sent) };
    if (!b.js) {
        or_err_local(err, "M0");
        return ESP_ERR_NO_MEM;
    }
    b.len = strlen(b.js);
    or_req_t q = {
        .method = HTTP_METHOD_POST, .url = url, .bearer = key, .accept = NULL, .referer = referer,
        .to = OR_TO_TTS, .body_len = (int64_t)b.len, .write_body = write_body, .body_ctx = &b,
        .abort = abort_job, .abort_ctx = j,
    };
    esp_err_t ret = ESP_FAIL;
    or_conn_t *c = or_conn_get(url);
    if (!c) {
        or_err_local(err, "M0");
    } else {
        bool locked = false;
        while (!(locked = or_conn_lock(c, 100)) && !abort_job(j)) {
        }
        if (!locked) {
            or_err_local(err, "AB");
        } else {
            or_resp_t r;
            if (or_start(c, &q, &r) != ESP_OK) {
                or_finish(c, false);
                or_classify(OR_KIND_TTS, &r, NULL, err);
            } else {
                ret = stream_audio(s, c, &r, model, progress, err);
            }
            or_conn_unlock(c);
        }
    }
    cJSON_free(b.js);
    memset(key, 0, sizeof(key));
    return ret;
}

// ---- one speak job ----------------------------------------------------------------------------

static bool route_error(const or_err_t *e)
{
    // protocol §5.6: 400, 403 and 404 (and a non-PCM answer) move on to the next route
    return !strcmp(e->id, "400") || !strncmp(e->id, "403", 3) || !strcmp(e->id, "404") || !strcmp(e->id, "T2");
}

static void run_speak(tts_job_t *j)
{
    tts_stats_t st = { .turn = j->turn };
    sink_t s = { .job = j, .st = &st, .t0_us = esp_timer_get_time() };
    or_err_t err;
    or_err_local(&err, "T1");
    bool failed = false;
    char **chunks = NULL;
    int n = 0;
    double cost = 0;
    tts_route_t routes[MAX_ROUTES];
    int nr = 0, ri = 0;
    char voice[96] = "";
    size_t tl = strlen(j->text);
    char *clean = ai_malloc(tl + 1);
    if (!clean) {
        or_err_local(&err, "M0");
        failed = true;
        goto done;
    }
    tts_sanitize(j->text, clean, tl + 1);
    n = tts_split(clean, &chunks);
    if (n < 0) {
        n = 0;
        or_err_local(&err, "M0");
        failed = true;
        goto done;
    }
    st.chunks = (uint32_t)n;
    nr = tts_routes(j->lang, routes, MAX_ROUTES);
    if (n > 0 && nr == 0) {
        or_err_local(&err, "T3");
        failed = true;
        goto done;
    }
    if (nr) {
        strlcpy(voice, routes[0].voice, sizeof(voice));
    }
    for (int k = 0; k < n && !failed; k++) {
        or_retry_t rs;
        or_retry_init(&rs, false);
        size_t progress = 0;
        bool voice_fixed = false;
        if (k > 0 && !j->fetch && !wait_ahead(j)) {
            or_err_local(&err, "AB");
            failed = true;
            break;
        }
        for (;;) {
            if (cancelled(j->turn)) {
                or_err_local(&err, "AB");
                failed = true;
                break;
            }
            bool speed_sent = false;
            const char *model = routes[ri].model;
            strlcpy(st.model, model, sizeof(st.model));
            strlcpy(st.voice, voice, sizeof(st.voice));
            if (speak_chunk(&s, model, voice, chunks[k], &progress, &speed_sent, &err) == ESP_OK) {
                size_t cp = utf8_cp_len(chunks[k]);
                st.chunks_done++;
                st.chars += (uint32_t)cp;
                cost += (double)cp * tts_price_per_char(model);
                break;
            }
            if (!strcmp(err.id, "AB")) {
                failed = true;
                break;
            }
            // automatic fixes of protocol §5.1 / §9.1
            if (!strcmp(err.id, "400") && speed_sent && ai_contains_ci(err.message, "speed")) {
                tts_speed_mark_unsupported(model);
                continue;
            }
            if (!strcmp(err.id, "400") && !voice_fixed && ai_contains_ci(err.message, "voice")) {
                voice_fixed = true;
                const char *dv = tts_model_default_voice(model, j->lang);
                if (dv && strcmp(dv, voice)) {
                    strlcpy(voice, dv, sizeof(voice));
                    continue;
                }
            }
            if (route_error(&err)) {
                tts_route_fail(model, j->lang, !strcmp(err.id, "403r"));
                if (++ri >= nr) {
                    ESP_LOGW(TAG, "every route failed for %s (last: %s %s)", j->lang, err.id, err.message);
                    or_err_local(&err, "T3");
                    failed = true;
                    break;
                }
                strlcpy(voice, routes[ri].voice, sizeof(voice));
                st.route_switches++;
                progress = 0;           // another voice: start the chunk again
                continue;
            }
            uint32_t delay;
            if (or_retry_next(&rs, &err, 0, &delay)) {
                st.retries++;
                ESP_LOGW(TAG, "chunk %d: %s (%s), retry in %" PRIu32 " ms", k + 1, err.id, err.message, delay);
                if (!ai_sleep_ms(delay, abort_job, j)) {
                    or_err_local(&err, "AB");
                    failed = true;
                    break;
                }
                continue;
            }
            failed = true;
            break;
        }
    }

done:
    st.total_ms = (uint32_t)((esp_timer_get_time() - s.t0_us) / 1000);
    st.cost_est = cost;
    ai_client_add_cost(cost);
    taskENTER_CRITICAL(&s_mux);
    s_last = st;
    taskEXIT_CRITICAL(&s_mux);
    j->rc = failed ? ESP_FAIL : ESP_OK;
    if (!failed) {
        ESP_LOGI(TAG, "turn %" PRIu32 ": %" PRIu32 " chunks, %" PRIu32 " B at %" PRIu32 " Hz, ttfb %" PRIu32 " ms",
                 j->turn, st.chunks_done, st.bytes, st.rate, st.ttfb_ms);
        emit(AI_EV_TTS_DONE, j->turn, (int32_t)st.chunks_done, NULL, &st);
    } else if (strcmp(err.id, "AB")) {
        if (strcmp(err.id, "T3") && strcmp(err.id, "T1")) {
            // the user sees "Озвучка недоступна"; the cause stays in the message for the log
            or_err_t f;
            or_err_local(&f, "T1");
            snprintf(f.message, sizeof(f.message), "%s: %.100s", err.id, err.message);
            f.http = err.http;
            err = f;
        }
        ESP_LOGW(TAG, "turn %" PRIu32 " failed: %s (%s)", j->turn, err.id, err.message);
        emit(AI_EV_TTS_ERROR, j->turn, 0, &err, &st);
    }
    tts_split_free(chunks, n);
    free(clean);
}

// ---- task and API -----------------------------------------------------------------------------

static void tts_task(void *arg)
{
    for (;;) {
        tts_job_t *j;
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cancelled(j->turn)) {
            j->rc = ESP_ERR_INVALID_STATE;
        } else {
            run_speak(j);
        }
        __atomic_sub_fetch(&s_pending, 1, __ATOMIC_RELAXED);
        if (j->done) {
            xSemaphoreGive(j->done);    // fetch job: lives on the caller's stack
        } else {
            free(j->text);
            free(j);
        }
    }
}

esp_err_t tts_client_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_q = xQueueCreate(TTS_Q_LEN, sizeof(tts_job_t *));
    ESP_RETURN_ON_FALSE(s_q, ESP_ERR_NO_MEM, TAG, "queue");
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(tts_task, "tts", TTS_STACK, NULL, TTS_PRIO, &s_task, TTS_CORE,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t tts_client_speak(uint32_t turn, const char *text, const char *lang)
{
    ESP_RETURN_ON_FALSE(s_task, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(turn && text, ESP_ERR_INVALID_ARG, TAG, "args");
    tts_job_t *j = ai_calloc(1, sizeof(tts_job_t));
    ESP_RETURN_ON_FALSE(j, ESP_ERR_NO_MEM, TAG, "job");
    j->turn = turn;
    j->text = ai_strdup(text);
    strlcpy(j->lang, lang && lang[0] ? lang : "ru", sizeof(j->lang));
    if (!j->text) {
        free(j);
        return ESP_ERR_NO_MEM;
    }
    uint32_t cur = __atomic_load_n(&s_max_turn, __ATOMIC_RELAXED);
    while (turn > cur && !__atomic_compare_exchange_n(&s_max_turn, &cur, turn, false, __ATOMIC_RELAXED,
                                                      __ATOMIC_RELAXED)) {
    }
    __atomic_add_fetch(&s_pending, 1, __ATOMIC_RELAXED);
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        __atomic_sub_fetch(&s_pending, 1, __ATOMIC_RELAXED);
        free(j->text);
        free(j);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t tts_client_fetch(const char *text, const char *lang, int16_t **pcm, size_t *samples, uint32_t *rate)
{
    ESP_RETURN_ON_FALSE(s_task, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(xTaskGetCurrentTaskHandle() != s_task, ESP_ERR_INVALID_STATE, TAG, "own task");
    *pcm = NULL;
    *samples = 0;
    *rate = 0;
    StaticSemaphore_t sem;
    tts_job_t j = { .fetch = true, .text = ai_strdup(text) };
    ESP_RETURN_ON_FALSE(j.text, ESP_ERR_NO_MEM, TAG, "text");
    strlcpy(j.lang, lang && lang[0] ? lang : "ru", sizeof(j.lang));
    j.done = xSemaphoreCreateBinaryStatic(&sem);
    tts_job_t *p = &j;
    __atomic_add_fetch(&s_pending, 1, __ATOMIC_RELAXED);
    xQueueSend(s_q, &p, portMAX_DELAY);
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    free(j.text);
    if (j.rc != ESP_OK) {
        free(j.pcm);
        return j.rc;
    }
    *pcm = (int16_t *)j.pcm;
    *samples = j.pcm_len / sizeof(int16_t);
    *rate = j.rate;
    return ESP_OK;
}

