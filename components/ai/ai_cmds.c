// "ai" console command: typed/voice assistant and translator turns, TTS, pickers, key, timings.
#include "ai_cmds.h"
#include "ai_priv.h"
#include "audio_in.h"
#include "audio_out.h"
#include "devconsole.h"
#include "stt_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define BIT_RESULT   BIT0
#define BIT_TTS      BIT1
#define TURN_WAIT_MS 120000
#define TTS_WAIT_MS  180000
#define DRAIN_WAIT_MS 60000

// What the blocking command waits for, filled by the listener (ai / tts task). In PSRAM:
// about 1.5 KB that only the console uses.
typedef struct {
    uint32_t turn;
    bool ok;
    ai_result_t res;              // strings duplicated into PSRAM
    or_err_t err;
    bool tts_ok;
    tts_stats_t tts;
    or_err_t tts_err;
} cap_t;
static cap_t *s_cap;
static EventGroupHandle_t s_eg;
static bool s_log = true;
static bool s_registered;

static void flat(char *s)
{
    for (; s && *s; s++) {
        if (*s == '\n' || *s == '\r') {
            *s = ' ';
        }
    }
}

static void listener(const ai_event_t *ev, void *ctx)
{
    bool waited = ev->turn && ev->turn == s_cap->turn;
    if (s_log) {
        switch (ev->type) {
        case AI_EV_UPLOAD_DONE:
            printf("ai: turn=%" PRIu32 " upload_done ms=%" PRId32 "\n", ev->turn, ev->i);
            break;
        case AI_EV_RETRY:
            printf("ai: turn=%" PRIu32 " retry=%" PRId32 " %s %s\n", ev->turn, ev->i, ev->err->id,
                   ev->err->msg_retry_ru[0] ? ev->err->msg_retry_ru : ev->err->msg_ru);
            break;
        case AI_EV_WEB:
            printf("ai: turn=%" PRIu32 " web_search %s\n", ev->turn, ev->i ? "rerun (needs_web)" : "first request");
            break;
        case AI_EV_RESULT:
            printf("ai: turn=%" PRIu32 " result ms=%" PRIu32 " lang=%s cost=%.6f\n", ev->turn, ev->result->t.ai_ms,
                   ev->result->lang, ev->result->cost);
            if (!waited) {
                char *t = ai_strdup(ev->result->transcript), *x = ai_strdup(ev->result->text);
                flat(t);
                flat(x);
                printf("ai: transcript: %s\nai: text: %s\n", t ? t : "", x ? x : "");
                free(t);
                free(x);
            }
            break;
        case AI_EV_ERROR:
            printf("ai: turn=%" PRIu32 " error %s %s\n", ev->turn, ev->err->id, ev->err->msg_ru);
            break;
        case AI_EV_RELEASED:
            printf("ai: turn=%" PRIu32 " released\n", ev->turn);
            break;
        case AI_EV_TTS_FIRST_AUDIO:
            printf("tts: turn=%" PRIu32 " first_audio ttfb_ms=%" PRId32 " rate=%" PRIu32 "\n", ev->turn, ev->i,
                   ev->tts->rate);
            break;
        case AI_EV_TTS_DONE:
            printf("tts: turn=%" PRIu32 " done chunks=%" PRIu32 " bytes=%" PRIu32 "\n", ev->turn, ev->tts->chunks_done,
                   ev->tts->bytes);
            break;
        case AI_EV_TTS_ERROR:
            printf("tts: turn=%" PRIu32 " error %s %s (%s)\n", ev->turn, ev->err->id, ev->err->msg_ru,
                   ev->err->message);
            break;
        }
    }
    if (!waited) {
        return;
    }
    switch (ev->type) {
    case AI_EV_RESULT:
        s_cap->res = *ev->result;
        s_cap->res.transcript = ai_strdup(ev->result->transcript);
        s_cap->res.text = ai_strdup(ev->result->text);
        s_cap->ok = s_cap->res.transcript && s_cap->res.text;
        if (!s_cap->ok) {
            or_err_local(&s_cap->err, "M0");
        }
        xEventGroupSetBits(s_eg, BIT_RESULT);
        break;
    case AI_EV_ERROR:
        s_cap->err = *ev->err;
        s_cap->ok = false;
        xEventGroupSetBits(s_eg, BIT_RESULT);
        break;
    case AI_EV_TTS_DONE:
        s_cap->tts = *ev->tts;
        s_cap->tts_ok = true;
        xEventGroupSetBits(s_eg, BIT_TTS);
        break;
    case AI_EV_TTS_ERROR:
        s_cap->tts = *ev->tts;
        s_cap->tts_err = *ev->err;
        s_cap->tts_ok = false;
        xEventGroupSetBits(s_eg, BIT_TTS);
        break;
    default:
        break;
    }
}

static void cap_reset(uint32_t turn)
{
    free(s_cap->res.transcript);
    free(s_cap->res.text);
    memset(s_cap, 0, sizeof(*s_cap));
    s_cap->turn = turn;
    xEventGroupClearBits(s_eg, BIT_RESULT | BIT_TTS);
}

// argv[from..] joined with spaces (PSRAM).
static char *join_args(int argc, char **argv, int from)
{
    size_t n = 1;
    for (int i = from; i < argc; i++) {
        n += strlen(argv[i]) + 1;
    }
    char *s = ai_malloc(n);
    if (!s) {
        return NULL;
    }
    s[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) {
            strcat(s, " ");
        }
        strcat(s, argv[i]);
    }
    return s;
}

static void print_field(const char *name, const char *v)
{
    char *c = ai_strdup(v ? v : "");
    flat(c);
    printf("%s: %s\n", name, c ? c : "");
    free(c);
}

static void print_err(const char *what, const or_err_t *e)
{
    printf("%s: id=%s http=%d msg=%s\n", what, e->id, e->http, e->msg_ru);
    if (e->message[0] || e->error_type[0]) {
        printf("detail: type=%s source=%s message=%s\n", e->error_type, e->limit_source, e->message);
    }
}

static void print_tts(const tts_stats_t *t)
{
    printf("tts: rate=%" PRIu32 " ch=%u bytes=%" PRIu32 " ttfb_ms=%" PRIu32 " chunks=%" PRIu32 "/%" PRIu32
           " chars=%" PRIu32 " total_ms=%" PRIu32 " retries=%" PRIu32 " switches=%" PRIu32 " cost=%.6f\n",
           t->rate, t->channels, t->bytes, t->ttfb_ms, t->chunks_done, t->chunks, t->chars, t->total_ms, t->retries,
           t->route_switches, t->cost_est);
    printf("tts: model=%s voice=%s\n", t->model, t->voice);
}

// Speaks text on the speaker and waits until it has been queued and played out.
static int speak_and_wait(uint32_t turn, const char *text, const char *lang)
{
    xEventGroupClearBits(s_eg, BIT_TTS);
    s_cap->turn = turn;
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = tts_client_speak(turn, text, lang);
    if (e != ESP_OK) {
        return dc_fail("tts: %s", esp_err_to_name(e));
    }
    if (!(xEventGroupWaitBits(s_eg, BIT_TTS, pdTRUE, pdFALSE, pdMS_TO_TICKS(TTS_WAIT_MS)) & BIT_TTS)) {
        return dc_fail("tts: no answer (timeout)");
    }
    print_tts(&s_cap->tts);
    if (!s_cap->tts_ok) {
        print_err("tts error", &s_cap->tts_err);
        return dc_fail("%s %s", s_cap->tts_err.id, s_cap->tts_err.msg_ru);
    }
    int64_t t1 = esp_timer_get_time();
    while (audio_out_busy() && esp_timer_get_time() - t1 < (int64_t)DRAIN_WAIT_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("tts: drained after %" PRId64 " ms (total %" PRId64 " ms)\n", (esp_timer_get_time() - t1) / 1000,
           (esp_timer_get_time() - t0) / 1000);
    return 0;
}

static int run_turn_cmd(ai_mode_t mode, bool voice, int argc, char **argv)
{
    if (!ai_client_ready()) {
        return dc_fail("ai not started");
    }
    int from = 2;
    bool async = false, web = false;
    for (; argc > from && (!strcmp(argv[from], "-a") || !strcmp(argv[from], "-w")); from++) {
        async |= argv[from][1] == 'a';
        web |= argv[from][1] == 'w';       // assistant only: the translator never searches
    }
    const audio_clip_t *clip = NULL;
    char *text = NULL;
    if (voice) {
        clip = audio_in_last();
        if (!clip || !clip->samples) {
            return dc_fail("no clip: run \"mic rec <ms>\" first");
        }
    } else {
        if (argc <= from) {
            return dc_fail("usage: ai %s [-a] [-w] <text...>", argv[1]);
        }
        text = join_args(argc, argv, from);
        if (!text) {
            return dc_fail("no memory");
        }
    }
    uint32_t turn = ai_client_new_turn();
    cap_reset(async ? 0 : turn);
    ai_request_t r = {
        .turn = turn, .mode = mode, .pcm = clip ? clip->pcm : NULL, .samples = clip ? clip->samples : 0,
        .rate = clip ? clip->rate : 16000, .text = text, .web = web,
    };
    esp_err_t e = ai_client_submit(&r);
    free(text);
    if (e != ESP_OK) {
        return dc_fail("submit: %s", esp_err_to_name(e));
    }
    printf("turn=%" PRIu32 " submitted%s\n", turn, clip ? " (voice)" : "");
    if (async) {
        return 0;
    }
    if (!(xEventGroupWaitBits(s_eg, BIT_RESULT, pdTRUE, pdFALSE, pdMS_TO_TICKS(TURN_WAIT_MS)) & BIT_RESULT)) {
        return dc_fail("no result (timeout)");
    }
    if (!s_cap->ok) {
        print_err("error", &s_cap->err);
        return dc_fail("%s %s", s_cap->err.id, s_cap->err.msg_ru);
    }
    const ai_result_t *res = &s_cap->res;
    print_field("transcript", res->transcript);
    print_field(mode == AI_MODE_ASSISTANT ? "reply" : "translation", res->text);
    if (mode == AI_MODE_ASSISTANT) {
        printf("lang: %s\n", res->lang);
    } else {
        printf("src_lang: %s target: %s speak: %s%s\n", res->src_lang, res->lang, res->tts_lang[0] ? res->tts_lang : "-",
               res->other_lang ? " (Другой язык)" : "");
    }
    if (res->notice_ru) {
        printf("notice: %s\n", res->notice_ru);
    }
    printf("finish=%s truncated=%d unheard=%d stt=%d model=%s provider=%s gen=%s\n", res->finish, res->truncated,
           res->unheard, res->stt_used, res->model, res->provider, res->gen_id);
    printf("web=%d web_failed=%d source=%s web_ms=%" PRIu32 "\n", res->web, res->web_failed,
           res->source[0] ? res->source : "-", res->t.web_ms);
    printf("timings: clip_ms=%" PRIu32 " upload_ms=%" PRIu32 " first_byte_ms=%" PRIu32 " ai_ms=%" PRIu32
           " stt_ms=%" PRIu32 " retries=%" PRIu32 " bytes_up=%" PRIu32 " http=%d reused=%d\n",
           res->t.clip_ms, res->t.upload_ms, res->t.first_byte_ms, res->t.ai_ms, res->t.stt_ms, res->t.retries,
           res->t.bytes_up, res->t.http_status, res->t.reused);
    printf("usage: prompt=%d completion=%d audio=%d reasoning=%d cost=%.6f session=%.6f\n", res->prompt_tokens,
           res->completion_tokens, res->audio_tokens, res->reasoning_tokens, res->cost, ai_client_session_cost());
    printf("history: %d exchanges\n", ai_client_history_len());
    if (!res->tts_lang[0] || !res->text[0]) {
        return 0;
    }
    if (cfg_int("text_only", 1)) {
        printf("tts: skipped (text_only=1)\n");
        return 0;
    }
    char *say = ai_strdup(res->text);
    char lang[8];
    strlcpy(lang, res->tts_lang, sizeof(lang));
    int rc = say ? speak_and_wait(turn, say, lang) : dc_fail("no memory");
    free(say);
    return rc;
}

static int cmd_say(int argc, char **argv)
{
    int from = 2;
    char lang[8];
    cfg_str("ui_lang", lang, sizeof(lang), "ru");
    if (argc > from + 1 && !strcmp(argv[from], "-l")) {
        strlcpy(lang, argv[from + 1], sizeof(lang));
        from += 2;
    }
    if (argc <= from) {
        return dc_fail("usage: ai say [-l <lang>] <text...>");
    }
    char *text = join_args(argc, argv, from);
    if (!text) {
        return dc_fail("no memory");
    }
    uint32_t turn = ai_client_new_turn();
    cap_reset(turn);
    int rc = speak_and_wait(turn, text, lang);
    free(text);
    return rc;
}

// Test fixtures: synthesise into memory and print a WAV blob in the devconsole BLOB format.
static int cmd_fetch(int argc, char **argv)
{
    int from = 2;
    char lang[8];
    cfg_str("ui_lang", lang, sizeof(lang), "ru");
    if (argc > from + 1 && !strcmp(argv[from], "-l")) {
        strlcpy(lang, argv[from + 1], sizeof(lang));
        from += 2;
    }
    if (argc <= from) {
        return dc_fail("usage: ai fetch [-l <lang>] <text...>");
    }
    char *text = join_args(argc, argv, from);
    if (!text) {
        return dc_fail("no memory");
    }
    int16_t *pcm = NULL;
    size_t samples = 0;
    uint32_t rate = 0;
    esp_err_t e = tts_client_fetch(text, lang, &pcm, &samples, &rate);
    free(text);
    if (e != ESP_OK) {
        tts_stats_t t;
        tts_client_last_stats(&t);
        print_tts(&t);
        return dc_fail("fetch: %s", esp_err_to_name(e));
    }
    size_t len = 44 + samples * 2;
    uint8_t *wav = ai_malloc(len);
    if (!wav) {
        free(pcm);
        return dc_fail("no memory");
    }
    or_wav_header(wav, rate, samples);
    memcpy(wav + 44, pcm, samples * 2);
    free(pcm);
    printf("BLOB:BEGIN wav %u %08lx %u 1 s16le\n", (unsigned)len, (unsigned long)esp_rom_crc32_le(0, wav, len),
           (unsigned)rate);
    char line[8 + 128 + 2];
    for (size_t i = 0; i < len; i += 96) {
        size_t n = len - i < 96 ? len - i : 96;
        memcpy(line, "BLOB:", 5);
        size_t o = 5 + or_b64_encode(wav + i, n, line + 5);
        line[o++] = '\n';
        line[o] = 0;
        fputs(line, stdout);
    }
    printf("BLOB:END\n");
    fflush(stdout);
    free(wav);
    return 0;
}

static int cmd_models_tts(void)
{
    ai_tts_model_t *l = ai_calloc(12, sizeof(ai_tts_model_t));   // PSRAM: the console stack is 6 KB
    if (!l) {
        return dc_fail("no memory");
    }
    int n = 0, total = 0, rc = 0;
    for (int page = 0;; page++) {
        esp_err_t e = ai_client_list_tts_models(page, l, 12, &n, &total);
        if (e != ESP_OK) {
            rc = dc_fail("models: %s", esp_err_to_name(e));
            break;
        }
        for (int i = 0; i < n; i++) {
            printf("%2d %-44s $%.4f/1k_ru voices=%u first=%s\n", page * 12 + i + 1, l[i].id, l[i].usd_1k_ru,
                   l[i].n_voices, l[i].first_voice);
        }
        if (n < 12) {
            printf("tts models: %d\n", total);
            break;
        }
    }
    free(l);
    return rc;
}

static int cmd_models(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[2], "tts")) {
        return cmd_models_tts();
    }
    ai_model_t *l = ai_calloc(10, sizeof(ai_model_t));
    if (!l) {
        return dc_fail("no memory");
    }
    int n = 0, total = 0, rc = 0;
    char cur[100];
    cfg_str("ai_model", cur, sizeof(cur), "");
    for (int page = 0;; page++) {
        esp_err_t e = ai_client_list_models(page, l, 10, &n, &total);
        if (e != ESP_OK) {
            rc = dc_fail("models: %s", esp_err_to_name(e));
            break;
        }
        for (int i = 0; i < n; i++) {
            printf("%2d %c %-44s est=$%.5f in=%.3f out=%.3f audio=%.3f effort=%s%s%s%s\n", page * 10 + i + 1,
                   strcmp(l[i].id, cur) ? ' ' : '*', l[i].id, l[i].est_turn_usd, l[i].usd_in_m, l[i].usd_out_m,
                   l[i].usd_audio_m, l[i].min_effort[0] ? l[i].min_effort : "-", l[i].mandatory ? " mandatory" : "",
                   l[i].expires[0] ? " истекает " : "", l[i].expires);
        }
        if (n < 10) {
            printf("chat models: %d\n", total);
            break;
        }
    }
    free(l);
    return rc;
}

static int cmd_voices(int argc, char **argv)
{
    if (argc < 3) {
        return dc_fail("usage: ai voices <tts-model> [locale]");
    }
    ai_tts_model_t l[1];
    int n, total;
    ai_client_list_tts_models(0, l, 1, &n, &total);    // makes sure the list is loaded
    char *buf = ai_malloc(8192);
    if (!buf) {
        return dc_fail("no memory");
    }
    size_t len = ai_client_tts_voices(argv[2], argc > 3 ? argv[3] : NULL, buf, 8192);
    char *p = buf;
    int count = 0;
    while (len && *p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = 0;
        }
        printf("%s\n", p);
        count++;
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    free(buf);
    printf("voices: %d\n", count);
    return count ? 0 : dc_fail("model not in the TTS list");
}

static int cmd_check(int argc, char **argv)
{
    ai_model_t m;
    or_err_t err;
    esp_err_t e = ai_client_check_model(argc > 2 ? argv[2] : NULL, &m, &err);
    if (e == ESP_ERR_NOT_FOUND) {
        print_err("check", &err);
        return dc_fail("model not found");
    }
    if (e != ESP_OK) {
        print_err("check", &err);
        return dc_fail("%s %s", err.id, err.msg_ru);
    }
    printf("model: %s (%s)\n", m.id, m.name);
    printf("price: in=%.3f out=%.3f audio=%.3f $/M, est=$%.5f/turn\n", m.usd_in_m, m.usd_out_m, m.usd_audio_m,
           m.est_turn_usd);
    printf("audio=%d structured=%d mandatory=%d effort=%s expires=%s\n", m.audio, m.structured, m.mandatory,
           m.min_effort[0] ? m.min_effort : "-", m.expires[0] ? m.expires : "-");
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    ai_key_info_t k;
    or_err_t err;
    if (ai_client_key_info(&k, &err) != ESP_OK) {
        print_err("key", &err);
        return dc_fail("%s %s", err.id, err.msg_ru);
    }
    printf("label=%s free_tier=%d expires=%s\n", k.label, k.free_tier, k.expires_at[0] ? k.expires_at : "-");
    printf("limit=%s%.4f remaining=%s%.4f reset=%s\n", k.has_limit ? "" : "null/", k.limit,
           k.has_limit_remaining ? "" : "null/", k.limit_remaining, k.limit_reset[0] ? k.limit_reset : "-");
    if (k.has_budget) {
        printf("budget: remaining=%.4f limit=%.4f spend=%.4f resets_at=%s\n", k.budget_remaining, k.budget_limit,
               k.budget_spend, k.budget_resets_at);
    }
    printf("usage: day=%.4f week=%.4f month=%.4f total=%.4f session=%.6f\n", k.usage_daily, k.usage_weekly,
           k.usage_monthly, k.usage, ai_client_session_cost());
    char lines[512];
    ai_key_info_lines(&k, lines, sizeof(lines));
    char *p = lines;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = 0;
        }
        printf("ui: %s\n", p);
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return 0;
}

static int cmd_last(int argc, char **argv)
{
    ai_last_t l;
    if (ai_client_last(&l)) {
        printf("turn=%" PRIu32 " mode=%s ok=%d err=%s %s\n", l.turn, l.mode == AI_MODE_ASSISTANT ? "assistant" : "translator",
               l.ok, l.err_id[0] ? l.err_id : "-", l.err_msg);
        printf("model=%s provider=%s gen=%s finish=%s truncated=%d unheard=%d stt=%d web=%d web_failed=%d "
               "web_ms=%" PRIu32 "\n", l.model, l.provider, l.gen_id, l.finish, l.truncated, l.unheard, l.stt_used,
               l.web, l.web_failed, l.t.web_ms);
        printf("timings: clip_ms=%" PRIu32 " upload_ms=%" PRIu32 " first_byte_ms=%" PRIu32 " ai_ms=%" PRIu32
               " stt_ms=%" PRIu32 " retries=%" PRIu32 " bytes_up=%" PRIu32 " http=%d reused=%d\n",
               l.t.clip_ms, l.t.upload_ms, l.t.first_byte_ms, l.t.ai_ms, l.t.stt_ms, l.t.retries, l.t.bytes_up,
               l.t.http_status, l.t.reused);
        printf("usage: prompt=%d completion=%d audio=%d cost=%.6f\n", l.prompt_tokens, l.completion_tokens,
               l.audio_tokens, l.cost);
    } else {
        printf("no turn yet\n");
    }
    tts_stats_t t;
    tts_client_last_stats(&t);
    if (t.turn) {
        printf("tts turn=%" PRIu32 "\n", t.turn);
        print_tts(&t);
    }
    printf("session cost=%.6f history=%d\n", ai_client_session_cost(), ai_client_history_len());
    // stack high-water marks (PSRAM stacks of 12 KB) and heap, for tuning
    printf("stack_free ai=%" PRIu32 " tts=%" PRIu32 " heap int_free=%u int_min=%u psram_free=%u\n",
           ai_task_stack_free(), tts_task_stack_free(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_req(int argc, char **argv)
{
    const size_t cap = 16 * 1024;
    char *buf = ai_malloc(cap);
    if (!buf) {
        return dc_fail("no memory");
    }
    size_t n = ai_client_last_request_redacted(buf, cap);
    if (!n) {
        free(buf);
        return dc_fail("no request yet");
    }
    // one console line per 240 bytes, never splitting a UTF-8 sequence
    const char *p = buf;
    while (*p) {
        size_t l = strlen(p) > 240 ? 240 : strlen(p);
        while (l < strlen(p) && ((unsigned char)p[l] & 0xC0) == 0x80) {
            l--;
        }
        printf("req: %.*s\n", (int)l, p);
        p += l;
    }
    free(buf);
    return 0;
}

typedef struct {
    const audio_clip_t *clip;
    const char *lang;
    char text[640];
} stt_ctx_t;

static esp_err_t stt_job(void *ctx)
{
    stt_ctx_t *c = ctx;
    return stt_client_transcribe(0, c->clip, c->lang, c->text, sizeof(c->text));
}

// Whisper fallback on the last recorded clip (runs in the ai task).
static int cmd_stt(int argc, char **argv)
{
    const audio_clip_t *clip = audio_in_last();
    if (!clip || !clip->samples) {
        return dc_fail("no clip: run \"mic rec <ms>\" first");
    }
    stt_ctx_t *c = ai_calloc(1, sizeof(stt_ctx_t));
    if (!c) {
        return dc_fail("no memory");
    }
    c->clip = clip;
    c->lang = argc > 2 ? argv[2] : NULL;
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = ai_run_sync(stt_job, c);
    int rc = 0;
    if (e == ESP_OK) {
        print_field("text", c->text);
        printf("stt: ms=%" PRId64 " clip_ms=%" PRIu32 " session=%.6f\n", (esp_timer_get_time() - t0) / 1000,
               clip->ms, ai_client_session_cost());
    } else {
        rc = dc_fail("stt: %s", esp_err_to_name(e));
    }
    free(c);
    return rc;
}

static int cmd_ai(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "ask")) {
        return run_turn_cmd(AI_MODE_ASSISTANT, false, argc, argv);
    }
    if (!strcmp(sub, "voice")) {
        return run_turn_cmd(AI_MODE_ASSISTANT, true, argc, argv);
    }
    if (!strcmp(sub, "tr")) {
        return run_turn_cmd(AI_MODE_TRANSLATOR, false, argc, argv);
    }
    if (!strcmp(sub, "trvoice")) {
        return run_turn_cmd(AI_MODE_TRANSLATOR, true, argc, argv);
    }
    if (!strcmp(sub, "say")) {
        return cmd_say(argc, argv);
    }
    if (!strcmp(sub, "models")) {
        return cmd_models(argc, argv);
    }
    if (!strcmp(sub, "fetch")) {
        return cmd_fetch(argc, argv);
    }
    if (!strcmp(sub, "voices")) {
        return cmd_voices(argc, argv);
    }
    if (!strcmp(sub, "check")) {
        return cmd_check(argc, argv);
    }
    if (!strcmp(sub, "key")) {
        return cmd_key(argc, argv);
    }
    if (!strcmp(sub, "last")) {
        return cmd_last(argc, argv);
    }
    if (!strcmp(sub, "req")) {
        return cmd_req(argc, argv);
    }
    if (!strcmp(sub, "cancel")) {
        ai_client_cancel(0);
        printf("cancelled every turn issued so far\n");
        return 0;
    }
    if (!strcmp(sub, "stt")) {
        return cmd_stt(argc, argv);
    }
    if (!strcmp(sub, "hist")) {
        if (argc > 2 && !strcmp(argv[2], "clear")) {
            ai_client_history_clear();
        }
        printf("history: %d exchanges\n", ai_client_history_len());
        return 0;
    }
    if (!strcmp(sub, "prewarm")) {
        esp_err_t e = ai_client_prewarm();
        return e == ESP_OK ? 0 : dc_fail("%s", esp_err_to_name(e));
    }
    if (!strcmp(sub, "log")) {
        s_log = argc > 2 ? atoi(argv[2]) != 0 : !s_log;
        printf("event log %s\n", s_log ? "on" : "off");
        return 0;
    }
    return dc_fail("usage: ai ask|tr [-a] [-w] <text...> | voice|trvoice [-a] [-w] | say|fetch [-l xx] <text...> | "
                   "models [tts] | voices <model> [locale] | check [model] | key | last | req | stt [lang] | "
                   "hist [clear] | cancel | prewarm | log 0|1");
}

void ai_console_register(void)
{
    if (s_registered) {
        return;
    }
    s_cap = ai_calloc(1, sizeof(cap_t));
    s_eg = xEventGroupCreate();
    if (!s_cap || !s_eg) {
        return;
    }
    s_registered = true;
    ai_add_listener(listener, NULL);
    devconsole_register("ai",
                        "ai ask|tr [-a] [-w] <text> | voice|trvoice [-a] [-w] | say|fetch [-l xx] <text> | "
                        "models [tts] | voices <m> [loc] | check [m] | key | last | req | stt [lang] | hist [clear] | "
                        "cancel | prewarm | log 0|1",
                        cmd_ai);
}
