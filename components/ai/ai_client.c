// Assistant and translator turns over POST {ai_url}/chat/completions (protocol §2-§4, §9).
#include "ai_priv.h"
#include "settings.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ai";

#define AI_STACK        12288          // TLS handshakes run here; PSRAM, so be generous
#define AI_PRIO         5
#define AI_CORE         0
#define AI_Q_LEN        6
#define MAX_LISTENERS   4
#define LAST_REQ_CAP    (32 * 1024)    // redacted copy; long history (up to 6000 code points) plus the prompt
#define MAX_TOK_TR      768
#define TURN_RETRY_US   (65LL * 1000 * 1000)     // 45 s per attempt + 20 s of retries
// No translator direction retry after this much of the turn: its OR_TO_TRDIR attempt (25 s) must end before the app's
// 75 s turn watchdog, which would throw the first translation away.
#define TR_RETRY_LATE_US (40LL * 1000 * 1000)
#define VOICE_TEXT_AS   "Голосовое сообщение пользователя."
#define VOICE_TEXT_TR   "Utterance:"

// The two %s take SCHEMA_NEEDS_WEB and its "required" entry in auto web mode (protocol §2.8), "" otherwise. needs_web
// comes before reply: the model decides first, then writes either the answer or the short holding phrase.
static const char SCHEMA_ASSISTANT[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"transcript\":{\"type\":\"string\",\"description\":\"Verbatim transcript of the user's latest message in its "
    "original language. Empty string if there is no intelligible speech.\"},"
    "%s"
    "\"reply\":{\"type\":\"string\",\"description\":\"Answer shown on screen and read aloud: plain speakable text, no "
    "markdown, no emoji, numbers written as words. Empty string if transcript is empty.\"},"
    "\"lang\":{\"type\":\"string\",\"enum\":[\"ru\",\"en\",\"de\",\"fr\",\"es\",\"it\",\"pt\",\"pl\",\"tr\",\"uk\","
    "\"zh\",\"ja\",\"ko\",\"ar\",\"other\"],\"description\":\"ISO 639-1 code of the language of reply.\"}"
    "},\"required\":[\"transcript\",%s\"reply\",\"lang\"],\"additionalProperties\":false}";

static const char SCHEMA_NEEDS_WEB[] =
    "\"needs_web\":{\"type\":\"boolean\",\"description\":\"true when a correct answer needs current or live "
    "information: weather, news, exchange rates, prices, schedules, sports results, today's events, anything after "
    "your knowledge cutoff. Not for the current date or time when the system prompt gives them. Then reply is only "
    "a very short holding phrase: the device searches the web and asks again.\"},";

// The two %s are the src_lang and target_lang enums: [A, B, "other"] and [B, A] normally; a source forced by the user
// (hold a language chip, "wrong language") has that code only, and so has a target fixed by the direction retry
// (protocol §3). target_lang comes before translation: the model commits to the direction first.
static const char SCHEMA_TRANSLATOR[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"src_lang\":{\"type\":\"string\",\"enum\":[%s],\"description\":\"Language of the utterance: one of the listed "
    "codes (other: neither of the two languages).\"},"
    "\"transcript\":{\"type\":\"string\",\"description\":\"Verbatim transcript in the original language; empty if "
    "no intelligible speech.\"},"
    "\"target_lang\":{\"type\":\"string\",\"enum\":[%s],\"description\":\"Language of the translation, as the system "
    "prompt says; never the language of the utterance.\"},"
    "\"translation\":{\"type\":\"string\",\"description\":\"Only the translated text, written in target_lang. Never "
    "an answer to the utterance.\"}"
    "},\"required\":[\"src_lang\",\"transcript\",\"target_lang\",\"translation\"],\"additionalProperties\":false}";

static const char NOTICE_P3[] = "(ответ обрезан)";
static const char NOTICE_P4[] = "Не расслышал. Повторите, пожалуйста.";
// W1: said after the holding phrase, which alone would promise an answer that never comes
static const char WEB_FAIL_RU[] = "Не получилось найти это в интернете, попробуйте ещё раз.";
static const char WEB_FAIL_EN[] = "I couldn't search the web just now, please try again.";

typedef enum { JOB_TURN, JOB_SYNC, JOB_PREWARM } job_type_t;

typedef struct {
    job_type_t type;
    uint32_t turn;
    ai_mode_t mode;
    const int16_t *pcm;
    size_t samples;
    uint32_t rate;
    char *text;
    char lang_a[8], lang_b[8];
    char src[8];                             // forced translator source, "" = detect
    bool web;                                // web search forced on this turn (console "ai ask -w")
    int64_t t_submit_us;
    esp_err_t (*fn)(void *ctx);
    void *ctx;
    esp_err_t rc;
    SemaphoreHandle_t done;
} job_t;

// One chat exchange: settings snapshot, request shape and the streamed body.
typedef struct {
    uint32_t turn;
    ai_mode_t mode;
    char url[200], key[208], model[100], fallback[100], effort[16], referer[136];
    int max_tokens;
    bool openrouter;
    bool no_reasoning, json_object;          // protocol §9.1 400 auto-fixes
    ai_web_t web;                            // web search shape of this request (protocol §2.8)
    int web_n;                               // its max_results
    char lang_a[8], lang_b[8];
    char src[8];                             // forced translator source, "" = detect
    char tgt[8];                             // translator target fixed by the direction retry, "" = by src_lang
    bool copy_note;                          // direction retry after a copy, direction left to the model
    bool temp0;                              // "temperature":0 (translator; dropped when the endpoint rejects it)
    const char *user_text;                   // typed or Whisper text; NULL = voice
    const int16_t *pcm;
    size_t samples;
    uint32_t rate;
    char *prefix, *suffix;
    size_t pre_len, suf_len;
    bool upload_reported;
    int64_t t_attempt_us;
} chat_t;

static QueueHandle_t s_q;
static TaskHandle_t s_task;
static struct {
    ai_event_cb_t cb;
    void *ctx;
} s_listeners[MAX_LISTENERS];
static int s_n_listeners;
static uint32_t s_turn_counter;
static uint32_t s_cancel_upto;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static double s_cost_session;
static ai_last_t s_last;
static char *s_last_req;

// ---- events, cancellation, sync jobs ----------------------------------------------------------

void ai_emit(const ai_event_t *ev)
{
    for (int i = 0; i < s_n_listeners; i++) {
        s_listeners[i].cb(ev, s_listeners[i].ctx);
    }
}

esp_err_t ai_add_listener(ai_event_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "cb");
    taskENTER_CRITICAL(&s_mux);
    bool ok = s_n_listeners < MAX_LISTENERS;
    if (ok) {
        s_listeners[s_n_listeners].cb = cb;
        s_listeners[s_n_listeners].ctx = ctx;
        s_n_listeners++;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}

uint32_t ai_client_new_turn(void)
{
    return __atomic_add_fetch(&s_turn_counter, 1, __ATOMIC_RELAXED);
}

bool ai_turn_cancelled(uint32_t turn)
{
    return turn && turn <= __atomic_load_n(&s_cancel_upto, __ATOMIC_RELAXED);
}

bool ai_abort_turn(void *ctx)
{
    return ai_turn_cancelled(*(const uint32_t *)ctx);
}

void ai_client_cancel(uint32_t turn)
{
    if (turn == 0) {
        turn = __atomic_load_n(&s_turn_counter, __ATOMIC_RELAXED);
    }
    uint32_t cur = __atomic_load_n(&s_cancel_upto, __ATOMIC_RELAXED);
    while (turn > cur && !__atomic_compare_exchange_n(&s_cancel_upto, &cur, turn, false, __ATOMIC_RELAXED,
                                                      __ATOMIC_RELAXED)) {
    }
    tts_client_cancel(turn);
}

esp_err_t ai_run_sync(esp_err_t (*fn)(void *ctx), void *ctx)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s_task) {
        return fn(ctx);
    }
    StaticSemaphore_t buf;
    job_t j = { .type = JOB_SYNC, .fn = fn, .ctx = ctx, .rc = ESP_FAIL };
    j.done = xSemaphoreCreateBinaryStatic(&buf);
    job_t *p = &j;
    xQueueSend(s_q, &p, portMAX_DELAY);
    xSemaphoreTake(j.done, portMAX_DELAY);    // every job ends within its own deadlines
    vSemaphoreDelete(j.done);
    return j.rc;
}

double ai_client_session_cost(void)
{
    taskENTER_CRITICAL(&s_mux);
    double c = s_cost_session;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

void ai_client_add_cost(double usd)
{
    if (usd > 0 && usd < 10) {
        taskENTER_CRITICAL(&s_mux);
        s_cost_session += usd;
        taskEXIT_CRITICAL(&s_mux);
    }
}

bool ai_client_last(ai_last_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_last;
    taskEXIT_CRITICAL(&s_mux);
    return out->valid;
}

size_t ai_client_last_request_redacted(char *out, size_t len)
{
    if (!len) {
        return 0;
    }
    out[0] = 0;
    taskENTER_CRITICAL(&s_mux);
    char *r = s_last_req;
    s_last_req = NULL;          // take it out while copying (no allocation inside the lock)
    taskEXIT_CRITICAL(&s_mux);
    if (r) {
        strlcpy(out, r, len);
        taskENTER_CRITICAL(&s_mux);
        if (!s_last_req) {
            s_last_req = r;
            r = NULL;
        }
        taskEXIT_CRITICAL(&s_mux);
        free(r);
    }
    return strlen(out);
}

void ai_client_history_clear(void)
{
    hist_clear();
}

int ai_client_history_len(void)
{
    return hist_count();
}

bool ai_client_ready(void)
{
    return s_task != NULL;
}

uint32_t ai_task_stack_free(void)
{
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}

// ---- request body -----------------------------------------------------------------------------

static bool valid_lang(const char *s)
{
    size_t n = strlen(s);
    if (n < 2 || n > 3) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 'a' || s[i] > 'z') {
            return false;
        }
    }
    return true;
}

static void lower_ascii(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

static void pick_effort(chat_t *ch)
{
    char e[16];
    cfg_str("ai_effort", e, sizeof(e), "auto");
    lower_ascii(e);
    if (!strcmp(e, "auto") || !strcmp(e, "minimal") || !strcmp(e, "none")) {
        // Protocol §2.1 rule 2: "minimal" if the model lists it, else its lowest effort, else omit
        // (gemini-3.7-flash bottoms out at "low"). "none" is never sent: mandatory models reject it.
        ai_model_t m;
        if (ai_model_cache_get(ch->model, &m)) {
            strlcpy(ch->effort, m.min_effort, sizeof(ch->effort));
        } else {
            strlcpy(ch->effort, "minimal", sizeof(ch->effort));
        }
    } else {
        strlcpy(ch->effort, e, sizeof(ch->effort));
    }
}

static cJSON *schema_json(const chat_t *ch)
{
    if (ch->mode == AI_MODE_ASSISTANT) {
        bool nw = ch->web == AI_WEB_AUTO;
        size_t n = sizeof(SCHEMA_ASSISTANT) + sizeof(SCHEMA_NEEDS_WEB) + 16;
        char *buf = ai_malloc(n);
        if (!buf) {
            return NULL;
        }
        snprintf(buf, n, SCHEMA_ASSISTANT, nw ? SCHEMA_NEEDS_WEB : "", nw ? "\"needs_web\"," : "");
        cJSON *s = cJSON_Parse(buf);
        free(buf);
        return s;
    }
    char src[40], tgt[24], buf[sizeof(SCHEMA_TRANSLATOR) + 64];
    if (ch->src[0]) {
        snprintf(src, sizeof(src), "\"%s\"", ch->src);
    } else if (ch->tgt[0]) {
        snprintf(src, sizeof(src), "\"%s\",\"other\"", strcmp(ch->tgt, ch->lang_a) ? ch->lang_a : ch->lang_b);
    } else {
        snprintf(src, sizeof(src), "\"%s\",\"%s\",\"other\"", ch->lang_a, ch->lang_b);
    }
    if (ch->tgt[0]) {
        snprintf(tgt, sizeof(tgt), "\"%s\"", ch->tgt);
    } else if (ch->src[0]) {
        snprintf(tgt, sizeof(tgt), "\"%s\"", strcmp(ch->src, ch->lang_a) ? ch->lang_a : ch->lang_b);
    } else {
        snprintf(tgt, sizeof(tgt), "\"%s\",\"%s\"", ch->lang_b, ch->lang_a);
    }
    snprintf(buf, sizeof(buf), SCHEMA_TRANSLATOR, src, tgt);
    return cJSON_Parse(buf);
}

static void free_body(chat_t *ch)
{
    cJSON_free(ch->prefix);    // prefix is the printed buffer; suffix points into it
    ch->prefix = ch->suffix = NULL;
    ch->pre_len = ch->suf_len = 0;
}

static void store_redacted(const chat_t *ch)
{
    char *r = ai_malloc(LAST_REQ_CAP);
    if (!r) {
        return;
    }
    if (ch->pcm) {
        snprintf(r, LAST_REQ_CAP, "%.*s<%u base64 chars of WAV>%s", (int)ch->pre_len, ch->prefix,
                 (unsigned)or_wav_b64_len(ch->samples), ch->suffix ? ch->suffix : "");
    } else {
        snprintf(r, LAST_REQ_CAP, "%s", ch->prefix);
    }
    taskENTER_CRITICAL(&s_mux);
    char *old = s_last_req;
    s_last_req = r;
    taskEXIT_CRITICAL(&s_mux);
    free(old);
}

static esp_err_t chat_build(chat_t *ch)
{
    free_body(ch);
    char ph[24] = "";
    if (ch->pcm) {
        snprintf(ph, sizeof(ph), "B64_%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    }
    cJSON *schema = schema_json(ch);
    char *sys = ch->mode == AI_MODE_ASSISTANT ? ai_prompt_assistant(ch->web)
                                              : ai_prompt_translator(ch->lang_a, ch->lang_b, ch->tgt, ch->copy_note);
    cJSON *root = cJSON_CreateObject();
    if (!schema || !sys || !root) {
        cJSON_Delete(schema);
        cJSON_Delete(root);
        free(sys);
        return ESP_ERR_NO_MEM;
    }
    // Protocol §2.1 rule 1: "models" without "model" for an OpenRouter fallback chain.
    if (ch->openrouter && ch->fallback[0] && strcmp(ch->fallback, ch->model)) {
        const char *ids[2] = { ch->model, ch->fallback };
        cJSON_AddItemToObject(root, "models", cJSON_CreateStringArray(ids, 2));
    } else {
        cJSON_AddStringToObject(root, "model", ch->model);
    }
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    if (ch->json_object) {
        // 400 auto-fix: json_object mode with the schema pasted into the system prompt.
        char *sj = cJSON_PrintUnformatted(schema);
        size_t n = strlen(sys) + (sj ? strlen(sj) : 0) + 96;
        char *s2 = ai_malloc(n);
        if (s2) {
            snprintf(s2, n, "%s\n\nReturn only a JSON object that matches this JSON schema:\n%s", sys, sj ? sj : "");
            free(sys);
            sys = s2;
        }
        cJSON_free(sj);
    }
    cJSON_AddStringToObject(m, "content", sys);
    cJSON_AddItemToArray(msgs, m);
    free(sys);
    if (ch->mode == AI_MODE_ASSISTANT) {
        hist_append_messages(msgs, cfg_int("hist_n", cfg_int("ai_hist", 6)), AI_HIST_MAX_CP);
    }
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    if (ch->pcm) {
        cJSON *parts = cJSON_AddArrayToObject(m, "content");
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "text");
        char forced[64] = "";
        if (ch->mode == AI_MODE_TRANSLATOR && ch->src[0]) {
            snprintf(forced, sizeof(forced), "Utterance in %s:", ai_lang_name(ch->src));
        }
        cJSON_AddStringToObject(t, "text", ch->mode == AI_MODE_ASSISTANT ? VOICE_TEXT_AS
                                           : ch->src[0]                  ? forced
                                                                         : VOICE_TEXT_TR);
        cJSON_AddItemToArray(parts, t);
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "type", "input_audio");
        cJSON *ia = cJSON_AddObjectToObject(a, "input_audio");
        cJSON_AddStringToObject(ia, "data", ph);
        cJSON_AddStringToObject(ia, "format", "wav");
        cJSON_AddItemToArray(parts, a);
    } else if (ch->mode == AI_MODE_TRANSLATOR) {
        // typed (or Whisper) text in tags, so that a question reads as material to translate, not as a prompt
        const char *t = ch->user_text ? ch->user_text : "";
        size_t n = strlen(t) + 32;
        char *u = ai_malloc(n);
        if (u) {
            snprintf(u, n, "<utterance>%s</utterance>", t);
        }
        cJSON_AddStringToObject(m, "content", u ? u : t);
        free(u);
    } else {
        cJSON_AddStringToObject(m, "content", ch->user_text ? ch->user_text : "");
    }
    cJSON_AddItemToArray(msgs, m);

    cJSON *rf = cJSON_AddObjectToObject(root, "response_format");
    if (ch->json_object) {
        cJSON_AddStringToObject(rf, "type", "json_object");
        cJSON_Delete(schema);
    } else {
        cJSON_AddStringToObject(rf, "type", "json_schema");
        cJSON *js = cJSON_AddObjectToObject(rf, "json_schema");
        cJSON_AddStringToObject(js, "name", ch->mode == AI_MODE_ASSISTANT ? "assistant_turn" : "translator_turn");
        cJSON_AddBoolToObject(js, "strict", true);
        cJSON_AddItemToObject(js, "schema", schema);
    }
    if (ch->openrouter && ch->effort[0] && !ch->no_reasoning) {
        cJSON *rs = cJSON_AddObjectToObject(root, "reasoning");
        cJSON_AddStringToObject(rs, "effort", ch->effort);
        cJSON_AddBoolToObject(rs, "exclude", true);
    }
    cJSON_AddNumberToObject(root, "max_tokens", ch->max_tokens);
    if (ch->temp0) {
        cJSON_AddNumberToObject(root, "temperature", 0);     // translator: the most literal reading (protocol §3)
    }
    cJSON_AddBoolToObject(root, "stream", false);
    if (ch->openrouter && ch->web == AI_WEB_ON) {
        // protocol §2.8: OpenRouter's web plugin; each result is billed (about $0.004), so keep max_results small
        cJSON *pl = cJSON_AddArrayToObject(root, "plugins");
        cJSON *wp = cJSON_CreateObject();
        cJSON_AddStringToObject(wp, "id", "web");
        cJSON_AddNumberToObject(wp, "max_results", ch->web_n);
        cJSON_AddItemToArray(pl, wp);
    }
    if (ch->openrouter) {
        cJSON *pv = cJSON_AddObjectToObject(root, "provider");
        cJSON_AddBoolToObject(pv, "require_parameters", true);
    }
    char *js = cJSON_PrintUnformatted(root);     // cJSON leaves UTF-8 unescaped
    cJSON_Delete(root);
    if (!js) {
        return ESP_ERR_NO_MEM;
    }
    ch->prefix = js;
    if (ch->pcm) {
        char *p = strstr(js, ph);
        if (!p) {
            cJSON_free(js);
            ch->prefix = NULL;
            return ESP_FAIL;
        }
        *p = 0;
        ch->pre_len = (size_t)(p - js);
        ch->suffix = p + strlen(ph);
        ch->suf_len = strlen(ch->suffix);
    } else {
        ch->pre_len = strlen(js);
    }
    store_redacted(ch);
    return ESP_OK;
}

static esp_err_t chat_write_body(or_conn_t *c, void *ctx)
{
    chat_t *ch = ctx;
    esp_err_t e = or_write_all(c, ch->prefix, ch->pre_len);
    if (e == ESP_OK && ch->pcm) {
        e = or_write_b64_wav(c, ch->pcm, ch->samples, ch->rate);
    }
    if (e == ESP_OK && ch->suffix) {
        e = or_write_all(c, ch->suffix, ch->suf_len);
    }
    if (e == ESP_OK && !ch->upload_reported) {
        ch->upload_reported = true;
        ai_event_t ev = { .type = AI_EV_UPLOAD_DONE, .turn = ch->turn,
                          .i = (int32_t)((esp_timer_get_time() - ch->t_attempt_us) / 1000) };
        ai_emit(&ev);
    }
    return e;
}

static int64_t body_len(const chat_t *ch)
{
    return (int64_t)ch->pre_len + (ch->pcm ? (int64_t)or_wav_b64_len(ch->samples) : 0) + (int64_t)ch->suf_len;
}

// ---- silence trim (protocol §1.4: energy VAD, 200 ms margins) ---------------------------------

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void trim_clip(const int16_t *pcm, size_t n, uint32_t rate, size_t *first, size_t *count)
{
    *first = 0;
    *count = n;
    size_t frame = rate / 50;                  // 20 ms
    size_t nf = frame ? n / frame : 0;
    if (nf < 10) {
        return;
    }
    int *db = ai_malloc(nf * 2 * sizeof(int));
    if (!db) {
        return;
    }
    int *sorted = db + nf;
    for (size_t f = 0; f < nf; f++) {
        uint64_t s = 0;                        // integer maths: the S3 FPU has no double
        for (size_t k = 0; k < frame; k++) {
            int32_t v = pcm[f * frame + k];
            s += (uint64_t)(v * v);
        }
        uint64_t ms = s / frame;
        // dBFS x10 with 3 dB steps: count halvings of the power below full scale (2^30)
        int d = 0;
        uint64_t ref = 1ull << 30;
        while (ms < ref && d > -960) {
            ref >>= 1;
            d -= 30;
        }
        db[f] = d;
        sorted[f] = d;
    }
    qsort(sorted, nf, sizeof(int), cmp_int);
    int floor = sorted[nf / 10];
    int thr = floor + 100 > -500 ? floor + 100 : -500;   // 10 dB over the floor, at least -50 dBFS
    size_t a = nf, b = 0;
    for (size_t f = 0; f < nf; f++) {
        if (db[f] > thr) {
            if (a == nf) {
                a = f;
            }
            b = f;
        }
    }
    free(db);
    if (a == nf) {
        return;                                // nothing above the floor: let the model decide
    }
    const size_t margin = 10;                  // 200 ms
    size_t s0 = a > margin ? a - margin : 0;
    size_t s1 = b + 1 + margin < nf ? b + 1 + margin : nf;
    *first = s0 * frame;
    *count = (s1 == nf ? n : s1 * frame) - *first;
}

// ---- response parsing (protocol §2.3) ---------------------------------------------------------

typedef struct {
    char *transcript, *text;
    char lang[8], src_lang[8], tgt_lang[8], finish[16], model[64], provider[32], gen_id[64];
    char tr_tgt[8];                            // translator: the device's target for this answer (tr_review)
    char source[48];                           // host of the first url_citation annotation
    bool truncated;
    bool needs_web;                            // assistant, auto web mode: current data was asked for
    double cost;
    int prompt_tokens, completion_tokens, audio_tokens, reasoning_tokens;
} parsed_t;

static void parsed_free(parsed_t *p)
{
    free(p->transcript);
    free(p->text);
    p->transcript = p->text = NULL;
}

// Content -> JSON object text: strip a code fence, keep the first '{' to the last '}'.
static char *content_json(const char *content)
{
    const char *s = content;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
        s++;
    }
    if (!strncmp(s, "```", 3)) {
        const char *nl = strchr(s, '\n');
        s = nl ? nl + 1 : s + 3;
    }
    const char *a = strchr(s, '{');
    const char *b = strrchr(s, '}');
    if (!a || !b || b < a) {
        return ai_strdup(s);                   // truncated: the salvage path reads it
    }
    return ai_strndup(a, (size_t)(b - a + 1));
}

// ESP_OK, or the local row id in *row ("P2", "P5") / "P1" when the body carries an error.
static esp_err_t parse_chat(const chat_t *ch, const char *body, parsed_t *out, const char **row)
{
    memset(out, 0, sizeof(*out));
    *row = "P2";
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return ESP_FAIL;
    }
    if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "error"))) {
        cJSON_Delete(root);
        *row = "P1";
        return ESP_FAIL;
    }
    strlcpy(out->model, ai_json_str(root, "model") ? ai_json_str(root, "model") : "", sizeof(out->model));
    strlcpy(out->provider, ai_json_str(root, "provider") ? ai_json_str(root, "provider") : "", sizeof(out->provider));
    strlcpy(out->gen_id, ai_json_str(root, "id") ? ai_json_str(root, "id") : "", sizeof(out->gen_id));
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (cJSON_IsObject(u)) {
        out->cost = ai_json_num(u, "cost", 0);
        out->prompt_tokens = (int)ai_json_num(u, "prompt_tokens", 0);
        out->completion_tokens = (int)ai_json_num(u, "completion_tokens", 0);
        out->audio_tokens = (int)ai_json_num(cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens_details"),
                                             "audio_tokens", 0);
        out->reasoning_tokens = (int)ai_json_num(cJSON_GetObjectItemCaseSensitive(u, "completion_tokens_details"),
                                                 "reasoning_tokens", 0);
    }
    const cJSON *ch0 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(ch0, "message");
    const char *fin = ai_json_str(ch0, "finish_reason");
    strlcpy(out->finish, fin ? fin : "", sizeof(out->finish));
    const cJSON *refusal = cJSON_GetObjectItemCaseSensitive(msg, "refusal");
    if (!strcmp(out->finish, "content_filter") || (refusal && !cJSON_IsNull(refusal))) {
        cJSON_Delete(root);
        *row = "P5";
        return ESP_FAIL;
    }
    if (!strcmp(out->finish, "error")) {
        cJSON_Delete(root);
        *row = "P1";
        return ESP_FAIL;
    }
    // web plugin: message.annotations[] = {"type":"url_citation","url_citation":{"url":…,"title":…}}
    const cJSON *an;
    cJSON_ArrayForEach(an, cJSON_GetObjectItemCaseSensitive(msg, "annotations")) {
        const char *ty = ai_json_str(an, "type");
        const char *u = ai_json_str(cJSON_GetObjectItemCaseSensitive(an, "url_citation"), "url");
        if (ty && !strcmp(ty, "url_citation") && u) {
            ai_url_host(u, out->source, sizeof(out->source));
            if (out->source[0]) {
                break;
            }
        }
    }
    const char *content = ai_json_str(msg, "content");
    if (!content) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    char *cj = content_json(content);
    cJSON_Delete(root);
    if (!cj) {
        return ESP_ERR_NO_MEM;
    }
    const char *k_text = ch->mode == AI_MODE_ASSISTANT ? "reply" : "translation";
    cJSON *o = cJSON_Parse(cj);
    if (!o) {
        if (!strcmp(out->finish, "length")) {
            // P3: salvage what was generated; it is not stored in the history
            out->text = ai_json_salvage(cj, k_text);
            out->transcript = ai_json_salvage(cj, "transcript");
            char *sl = ch->mode == AI_MODE_TRANSLATOR ? ai_json_salvage(cj, "src_lang") : NULL;
            free(cj);
            if (out->text && out->text[0]) {
                if (!out->transcript) {
                    out->transcript = ai_strdup("");
                }
                out->truncated = true;
                if (ch->mode == AI_MODE_ASSISTANT) {
                    // "lang" comes last and is cut off: assume the UI language
                    char ul[8];
                    cfg_str("ui_lang", ul, sizeof(ul), "ru");
                    lower_ascii(ul);
                    strlcpy(out->lang, ai_lang_in_enum(ul) ? ul : "ru", sizeof(out->lang));
                } else if (sl) {
                    lower_ascii(sl);
                    strlcpy(out->src_lang, (!strcmp(sl, ch->lang_a) || !strcmp(sl, ch->lang_b)) ? sl : "other",
                            sizeof(out->src_lang));
                }
                free(sl);
                return ESP_OK;
            }
            free(sl);
            parsed_free(out);
            return ESP_FAIL;
        }
        free(cj);
        return ESP_FAIL;
    }
    free(cj);
    const char *tr = ai_json_str(o, "transcript");
    const char *tx = ai_json_str(o, k_text);
    const char *lg = ai_json_str(o, ch->mode == AI_MODE_ASSISTANT ? "lang" : "src_lang");
    if (!tr || !tx) {
        cJSON_Delete(o);
        return ESP_FAIL;
    }
    out->transcript = ai_strdup(tr);
    out->text = ai_strdup(tx);
    out->needs_web = ch->mode == AI_MODE_ASSISTANT && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "needs_web"));
    if (ch->mode == AI_MODE_TRANSLATOR) {
        const char *tg = ai_json_str(o, "target_lang");
        strlcpy(out->tgt_lang, tg ? tg : "", sizeof(out->tgt_lang));
        lower_ascii(out->tgt_lang);
    }
    char l[8];
    strlcpy(l, lg ? lg : "", sizeof(l));
    lower_ascii(l);
    if (ch->mode == AI_MODE_ASSISTANT) {
        strlcpy(out->lang, ai_lang_in_enum(l) ? l : "other", sizeof(out->lang));
    } else {
        strlcpy(out->src_lang, (!strcmp(l, ch->lang_a) || !strcmp(l, ch->lang_b)) ? l : "other",
                sizeof(out->src_lang));
    }
    out->truncated = !strcmp(out->finish, "length");
    cJSON_Delete(o);
    if (!out->transcript || !out->text) {
        parsed_free(out);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---- turn -------------------------------------------------------------------------------------

static void finish_last(const ai_result_t *res, const or_err_t *err)
{
    taskENTER_CRITICAL(&s_mux);
    memset(&s_last, 0, sizeof(s_last));
    s_last.valid = true;
    s_last.turn = res->turn;
    s_last.mode = res->mode;
    s_last.ok = err == NULL;
    if (err) {
        strlcpy(s_last.err_id, err->id, sizeof(s_last.err_id));
        strlcpy(s_last.err_msg, err->msg_ru, sizeof(s_last.err_msg));
    }
    strlcpy(s_last.model, res->model, sizeof(s_last.model));
    strlcpy(s_last.provider, res->provider, sizeof(s_last.provider));
    strlcpy(s_last.gen_id, res->gen_id, sizeof(s_last.gen_id));
    strlcpy(s_last.finish, res->finish, sizeof(s_last.finish));
    s_last.cost = res->cost;
    s_last.prompt_tokens = res->prompt_tokens;
    s_last.completion_tokens = res->completion_tokens;
    s_last.audio_tokens = res->audio_tokens;
    s_last.truncated = res->truncated;
    s_last.unheard = res->unheard;
    s_last.stt_used = res->stt_used;
    s_last.web = res->web;
    s_last.web_failed = res->web_failed;
    s_last.t = res->t;
    taskEXIT_CRITICAL(&s_mux);
}

static void emit_error(const ai_result_t *res, const or_err_t *e)
{
    finish_last(res, e);
    if (!strcmp(e->id, "AB")) {
        ESP_LOGI(TAG, "turn %" PRIu32 " cancelled", res->turn);
        return;                                // cancelled turns report nothing (the app moved on)
    }
    ESP_LOGW(TAG, "turn %" PRIu32 " failed: %s (%s)", res->turn, e->id, e->message);
    ai_event_t ev = { .type = AI_EV_ERROR, .turn = res->turn, .err = e };
    ai_emit(&ev);
}

static bool mentions_any(const char *s, const char *const *words, int n)
{
    for (int i = 0; i < n; i++) {
        if (ai_contains_ci(s, words[i])) {
            return true;
        }
    }
    return false;
}

// W1 (protocol §2.8): the first answer is only the holding phrase ("Сейчас посмотрю."), which alone would promise
// an answer that never comes, so a sentence saying that the search failed follows it (shown and spoken). Russian
// for a Russian reply, English otherwise. The text stays as it is when there is no memory for the new one.
static void web_fail_reply(parsed_t *pr)
{
    const char *say = (!pr->lang[0] || !strcmp(pr->lang, "ru")) ? WEB_FAIL_RU : WEB_FAIL_EN;
    const char *first = pr->text ? pr->text : "";
    size_t fl = strlen(first);
    while (fl && (first[fl - 1] == ' ' || first[fl - 1] == '\n')) {
        fl--;
    }
    // "Сейчас посмотрю" gets its full stop; ".", "!", "?" and "…" (UTF-8 E2 80 A6) end a sentence already
    char last = fl ? first[fl - 1] : 0;
    bool ends = last == '.' || last == '!' || last == '?' || (fl >= 3 && !memcmp(first + fl - 3, "\xE2\x80\xA6", 3));
    const char *sep = !fl ? "" : ends ? " " : ". ";
    size_t n = fl + strlen(sep) + strlen(say) + 1;
    char *t = ai_malloc(n);
    if (!t) {
        return;
    }
    snprintf(t, n, "%.*s%s%s", (int)fl, first, sep, say);
    free(pr->text);
    pr->text = t;
}

// Protocol §2.8 auto mode: the first answer set needs_web, so query (the transcript) goes again as a text request
// with the web plugin and the same history; the clip is not uploaded again. One attempt within OR_TO_WEB. On
// success *pr takes the new reply (its transcript stays) and the cost of both requests; on any failure the first
// answer stays with the W1 sentence and res->web_failed is set. Returns false only when the turn was cancelled
// meanwhile; a re-run billed before that is in res->cost.
static bool web_rerun(chat_t *ch, const char *query, parsed_t *pr, ai_result_t *res)
{
    ai_event_t ev = { .type = AI_EV_WEB, .turn = ch->turn, .i = 1 };
    ai_emit(&ev);
    if (ai_turn_cancelled(ch->turn)) {
        return false;                          // barge-in or PWR before the search was sent: it is not paid for
    }
    ch->pcm = NULL;
    ch->user_text = query;
    ch->web = AI_WEB_ON;
    int64_t t0 = esp_timer_get_time();
    // PSRAM, not the ai stack: run_turn's frame is below this one while TLS may handshake again
    struct {
        parsed_t p;
        or_err_t err;
        or_resp_t r;
    } *w = ai_calloc(1, sizeof(*w));
    char *body = NULL;
    bool ok = false;
    if (!w) {
        res->web_failed = !ai_turn_cancelled(ch->turn);
        ESP_LOGW(TAG, "turn %" PRIu32 ": web search skipped (no memory)", ch->turn);
        if (res->web_failed) {
            web_fail_reply(pr);
        }
        return !ai_turn_cancelled(ch->turn);
    }
    if (chat_build(ch) != ESP_OK) {
        or_err_local(&w->err, "M0");
    } else {
        or_req_t q = {
            .method = HTTP_METHOD_POST, .url = ch->url, .bearer = ch->key, .accept = "application/json",
            .referer = ch->referer, .to = OR_TO_WEB, .body_len = body_len(ch),
            .write_body = chat_write_body, .body_ctx = ch, .abort = ai_abort_turn, .abort_ctx = &ch->turn,
        };
        ch->t_attempt_us = t0;
        esp_err_t e = or_exchange(&q, &w->r, &body, OR_CAP_CHAT, NULL);
        const char *row = NULL;
        ok = e == ESP_OK && w->r.status == 200 && body && parse_chat(ch, body, &w->p, &row) == ESP_OK &&
             w->p.text[0];
        if (!ok && row && strcmp(row, "P1")) {
            or_err_local(&w->err, row);
        } else if (!ok) {
            or_classify(OR_KIND_CHAT, &w->r, body, &w->err);
        }
    }
    free(body);
    res->t.web_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    bool alive = !ai_turn_cancelled(ch->turn);
    parsed_t *p2 = &w->p;
    if (alive && ok) {
        free(pr->text);
        pr->text = p2->text;
        p2->text = NULL;
        pr->cost += p2->cost;                  // both requests are billed
        pr->prompt_tokens += p2->prompt_tokens;
        pr->completion_tokens += p2->completion_tokens;
        pr->reasoning_tokens += p2->reasoning_tokens;
        pr->truncated = p2->truncated;
        pr->needs_web = false;
        if (p2->lang[0]) {
            strlcpy(pr->lang, p2->lang, sizeof(pr->lang));
        }
        strlcpy(pr->finish, p2->finish, sizeof(pr->finish));
        strlcpy(pr->model, p2->model, sizeof(pr->model));
        strlcpy(pr->provider, p2->provider, sizeof(pr->provider));
        strlcpy(pr->gen_id, p2->gen_id, sizeof(pr->gen_id));
        strlcpy(pr->source, p2->source, sizeof(pr->source));
        strlcpy(res->gen_id, w->r.gen_id[0] ? w->r.gen_id : p2->gen_id, sizeof(res->gen_id));
        ESP_LOGI(TAG, "turn %" PRIu32 ": web search ok in %" PRIu32 " ms, $%.6f, source %s", ch->turn,
                 res->t.web_ms, p2->cost, pr->source[0] ? pr->source : "-");
    } else {
        res->cost += p2->cost;                 // billed even when it did not parse or the turn was cancelled
        if (alive) {
            res->web_failed = true;
            web_fail_reply(pr);
            ESP_LOGW(TAG, "turn %" PRIu32 ": web search failed: %s (%s), keeping the first answer", ch->turn,
                     w->err.id, w->err.message);
        }
    }
    parsed_free(p2);
    free(w);
    return alive;
}

static const char *pair_other(const chat_t *ch, const char *code)
{
    return strcmp(code, ch->lang_a) ? ch->lang_a : ch->lang_b;
}

// Translator (protocol §3). pr->tr_tgt gets the device target: the one fixed by the direction retry, else the pair
// language that src_lang is not; the model's target_lang only gets a log line. Returns why the translation needs the
// direction retry ("script", "copy", "loop"), NULL when it looks right.
static const char *tr_review(const chat_t *ch, const char *said, parsed_t *pr)
{
    const char *t = ch->tgt[0] ? ch->tgt : !strcmp(pr->src_lang, ch->lang_a) ? ch->lang_b : ch->lang_a;
    strlcpy(pr->tr_tgt, t, sizeof(pr->tr_tgt));
    if (pr->tgt_lang[0] && strcmp(pr->tgt_lang, t)) {
        ESP_LOGW(TAG, "turn %" PRIu32 ": model target_lang %s, device rule %s (src_lang %s)", ch->turn, pr->tgt_lang,
                 t, pr->src_lang);
    }
    if (pr->truncated) {
        // max_tokens on a translation far longer than the utterance: a loop (temperature 0), not a long text
        return utf8_cp_len(pr->text) > 2 * utf8_cp_len(said) + 40 ? "loop" : NULL;
    }
    return ai_tr_check(said, pr->text, t, pair_other(ch, t));
}

// Sets up the protocol §3 direction retry after the first answer failed with why. said is what was checked; typed:
// it is the real input (typed or Whisper text), not the model's echo of a clip.
static void tr_retry_setup(chat_t *ch, const parsed_t *first, const char *why, const char *said, bool typed)
{
    if (!strcmp(why, "loop")) {
        ch->temp0 = false;                   // the same request at the provider's default temperature
        return;
    }
    // the model's own direction failed: the script of typed text is a sure sign of the source when the pair is written
    // differently (a clip's echo is not: the model may have written the transcript already translated)
    const char *by_script = typed && !ch->src[0] ? ai_tr_src_by_script(said, ch->lang_a, ch->lang_b) : NULL;
    if (ch->src[0]) {
        strlcpy(ch->tgt, first->tr_tgt, sizeof(ch->tgt));        // forced by the user: the direction stays
    } else if (by_script) {
        if (strcmp(by_script, first->src_lang)) {
            ESP_LOGW(TAG, "turn %" PRIu32 ": src_lang %s, but the text is in the script of %s", ch->turn,
                     first->src_lang, by_script);
        }
        strlcpy(ch->src, by_script, sizeof(ch->src));
        strlcpy(ch->tgt, pair_other(ch, by_script), sizeof(ch->tgt));
    } else if (!strcmp(why, "copy")) {
        // a copy in a pair sharing a script (en-de), or of a clip: the text does not tell which language the utterance
        // is in, most often the model took it for the other one. It decides again, told that a copy is wrong.
        ch->copy_note = true;
    } else {
        strlcpy(ch->tgt, first->tr_tgt, sizeof(ch->tgt));        // the model's direction in the wrong script
        if (strcmp(first->src_lang, "other")) {
            strlcpy(ch->src, first->src_lang, sizeof(ch->src));
        }
    }
}

static void run_turn(job_t *j)
{
    ai_result_t res = { .turn = j->turn, .mode = j->mode };
    or_err_t err;
    parsed_t pr = { 0 };
    parsed_t tr_first = { 0 };               // translator: the answer that got the direction retry
    bool tr_retry = false;
    bool tr_audio = false;                   // ... sent the clip again (after a copy)
    char *stt_text = NULL;
    chat_t *ch = ai_calloc(1, sizeof(chat_t));
    bool voice_in = j->pcm && j->samples;
    bool voice_in_released = false;          // AI_EV_RELEASED already sent (before a web re-run)
    bool failed = true;
    if (!ch) {
        or_err_local(&err, "M0");
        goto done;
    }
    ch->turn = j->turn;
    ch->mode = j->mode;
    char base[136];
    cfg_str("ai_url", base, sizeof(base), "https://openrouter.ai/api/v1");
    ai_join_url(ch->url, sizeof(ch->url), base, "chat/completions");
    cfg_str("ai_key", ch->key, sizeof(ch->key), "");
    cfg_str("ai_model", ch->model, sizeof(ch->model), "google/gemini-3.5-flash-lite");
    cfg_str("ai_model_fb", ch->fallback, sizeof(ch->fallback), "google/gemini-3.1-flash-lite");
    cfg_str("ref_url", ch->referer, sizeof(ch->referer), "");
    ch->openrouter = or_is_openrouter(base);
    if (j->mode == AI_MODE_ASSISTANT && ch->openrouter) {
        // protocol §2.8: ai_web 0 off, 1 auto, 2 always; other providers and the translator never search
        int32_t w = j->web ? AI_WEB_ON : cfg_int("ai_web", AI_WEB_AUTO);
        ch->web = w == AI_WEB_ON ? AI_WEB_ON : w == AI_WEB_AUTO ? AI_WEB_AUTO : AI_WEB_OFF;
    }
    int32_t wn = cfg_int("ai_web_n", 3);
    ch->web_n = wn < 1 ? 1 : wn > 5 ? 5 : (int)wn;
    ch->max_tokens = j->mode == AI_MODE_ASSISTANT ? ai_reply_max_tokens() : MAX_TOK_TR;
    ch->temp0 = j->mode == AI_MODE_TRANSLATOR;
    pick_effort(ch);
    strlcpy(ch->lang_a, j->lang_a, sizeof(ch->lang_a));
    strlcpy(ch->lang_b, j->lang_b, sizeof(ch->lang_b));
    strlcpy(ch->src, j->src, sizeof(ch->src));
    hist_touch(j->mode, ch->model, (int)cfg_int("hist_idle_min", 10));

    if (ai_turn_cancelled(j->turn)) {
        or_err_local(&err, "AB");
        goto done;
    }
    if (!ai_net_up()) {
        or_err_local(&err, "N0");
        goto done;
    }
    if (!ch->key[0]) {
        or_err_local(&err, "K0");
        goto done;
    }
    if (voice_in) {
        size_t first, count;
        trim_clip(j->pcm, j->samples, j->rate, &first, &count);
        ch->pcm = j->pcm + first;
        ch->samples = count;
        ch->rate = j->rate;
        res.t.clip_ms = (uint32_t)(count * 1000 / (j->rate ? j->rate : 16000));
    } else {
        ch->user_text = j->text ? j->text : "";
    }

    // STT first when asked to, or when the cached model entry says it takes no audio (protocol §6).
    ai_model_t mi;
    bool stt_first = cfg_int("stt_mode", 0) == 1 || (ai_model_cache_get(ch->model, &mi) && !mi.audio);
    bool stt_done = false;
    or_retry_t rs;
    or_retry_init(&rs, false);
    int64_t deadline = j->t_submit_us + TURN_RETRY_US;
    bool fix_reasoning = false, fix_schema = false, fix_web = false, ctx_cleared = false, dirty = true;
    if (ch->web == AI_WEB_ON) {
        ai_event_t ev = { .type = AI_EV_WEB, .turn = j->turn, .i = 0 };   // always mode: this request searches
        ai_emit(&ev);
    }

    for (;;) {
        if (ch->pcm && stt_first && !stt_done) {
            char ui_lang[8];
            cfg_str("ui_lang", ui_lang, sizeof(ui_lang), "ru");
            double c = 0;
            uint32_t ms = 0;
            stt_done = true;
            if (stt_transcribe_ex(j->turn, ch->pcm, ch->samples, ch->rate,
                                  j->mode == AI_MODE_ASSISTANT ? ui_lang : (ch->src[0] ? ch->src : NULL), &stt_text,
                                  &c, &ms, &err) != ESP_OK) {
                goto done;
            }
            res.cost += c;
            res.t.stt_ms = ms;
            res.stt_used = true;
            ch->pcm = NULL;
            ch->user_text = stt_text;
            dirty = true;
            if (!stt_text[0]) {
                // nothing intelligible: P4 without a chat round trip
                pr.transcript = ai_strdup("");
                pr.text = ai_strdup("");
                if (!pr.transcript || !pr.text) {
                    or_err_local(&err, "M0");
                    goto done;
                }
                failed = false;
                goto done;
            }
        }
        if (dirty) {
            if (chat_build(ch) != ESP_OK) {
                or_err_local(&err, "M0");
                goto done;
            }
            dirty = false;
        }
        or_req_t q = {
            .method = HTTP_METHOD_POST, .url = ch->url, .bearer = ch->key, .accept = "application/json",
            .referer = ch->referer, .to = OR_TO_CHAT, .body_len = body_len(ch),
            .write_body = chat_write_body, .body_ctx = ch, .abort = ai_abort_turn, .abort_ctx = &ch->turn,
        };
        if (tr_retry) {
            q.to = (or_timeouts_t)OR_TO_TRDIR;
        }
        or_resp_t r;
        char *body = NULL;
        ch->t_attempt_us = esp_timer_get_time();
        esp_err_t e = or_exchange(&q, &r, &body, OR_CAP_CHAT, NULL);
        res.t.upload_ms = r.t_sent_us ? (uint32_t)((r.t_sent_us - r.t_start_us) / 1000) : 0;
        res.t.first_byte_ms = r.t_headers_us ? (uint32_t)((r.t_headers_us - r.t_sent_us) / 1000) : 0;
        res.t.bytes_up = (uint32_t)r.bytes_sent;
        res.t.http_status = r.status;
        res.t.reused = r.reused;
        strlcpy(res.gen_id, r.gen_id, sizeof(res.gen_id));
        const char *row = NULL;
        if (e == ESP_OK && r.status == 200 && body) {
            parsed_free(&pr);
            if (parse_chat(ch, body, &pr, &row) == ESP_OK) {
                // Translator: an empty translation of real speech is retried once (protocol §3).
                if (ch->mode == AI_MODE_TRANSLATOR && pr.transcript[0] && !pr.text[0]) {
                    row = "P2";
                } else {
                    free(body);
                    const char *said = ch->user_text ? ch->user_text : pr.transcript;
                    const char *bad = ch->mode == AI_MODE_TRANSLATOR && pr.transcript[0] ? tr_review(ch, said, &pr)
                                                                                         : NULL;
                    if (bad && !tr_retry && esp_timer_get_time() - j->t_submit_us > TR_RETRY_LATE_US) {
                        ESP_LOGW(TAG, "turn %" PRIu32 ": translation fails the %s check, too late to ask again",
                                 j->turn, bad);
                    } else if (bad && !tr_retry) {
                        // Protocol §3 direction retry, once, in one short attempt (an error keeps this answer).
                        // The transcript goes as text, except after a copy of a clip: only the audio tells then.
                        tr_retry = dirty = true;
                        tr_first = pr;
                        memset(&pr, 0, sizeof(pr));
                        tr_retry_setup(ch, &tr_first, bad, said, !ch->pcm);
                        ESP_LOGW(TAG, "turn %" PRIu32 ": translation fails the %s check, asking again (%s -> %s)",
                                 j->turn, bad, ch->src[0] ? ch->src : "?", ch->tgt[0] ? ch->tgt : "?");
                        if (ch->pcm && !ch->copy_note) {
                            ch->pcm = NULL;
                            ch->user_text = tr_first.transcript;
                        }
                        tr_audio = ch->pcm != NULL;
                        continue;
                    }
                    if (bad && tr_retry) {
                        ESP_LOGW(TAG, "turn %" PRIu32 ": translation still fails the %s check, shown as it is",
                                 j->turn, bad);
                    }
                    failed = false;
                    break;
                }
            }
        }
        if (row && strcmp(row, "P1")) {
            or_err_local(&err, row);
        } else {
            or_classify(OR_KIND_CHAT, &r, body, &err);
        }
        free(body);
        if (!strcmp(err.id, "AB") || !strcmp(err.id, "P5") || tr_retry) {
            goto done;                           // a failed direction retry keeps the first translation at once
        }
        // Automatic fixes of protocol §9.1 (not counted as retries).
        // "endpoint" alone is not enough: "No endpoints found that can handle the requested
        // parameters" is a schema/reasoning problem, and Whisper would not fix it.
        static const char *const W_AUDIO[] = { "audio", "modalit" };
        static const char *const W_REASON[] = { "reasoning", "effort" };
        static const char *const W_SCHEMA[] = { "response_format", "schema", "structured" };
        static const char *const W_WEB[] = { "plugin", "web search", "web_search" };
        static const char *const W_TEMP[] = { "temperature", "requested parameters" };
        if (ch->temp0 && (!strcmp(err.id, "400") || !strcmp(err.id, "404")) && mentions_any(err.message, W_TEMP, 2)) {
            ESP_LOGW(TAG, "temperature rejected, sending without it");
            ch->temp0 = false;
            dirty = true;
            continue;
        }
        if (ch->web == AI_WEB_ON && !fix_web && (!strcmp(err.id, "400") || !strcmp(err.id, "404")) &&
            mentions_any(err.message, W_WEB, 3)) {
            ESP_LOGW(TAG, "web search rejected, sending without it");
            fix_web = dirty = true;
            ch->web = AI_WEB_OFF;
            continue;
        }
        if (ch->pcm && !stt_done && (!strcmp(err.id, "404") || !strcmp(err.id, "400")) &&
            mentions_any(err.message, W_AUDIO, 2)) {
            ESP_LOGW(TAG, "model rejects audio input, falling back to STT");
            stt_first = true;
            continue;
        }
        if (!strcmp(err.id, "400") && !fix_reasoning && ch->effort[0] && mentions_any(err.message, W_REASON, 2)) {
            fix_reasoning = ch->no_reasoning = dirty = true;
            continue;
        }
        if (!strcmp(err.id, "400") && !fix_schema && mentions_any(err.message, W_SCHEMA, 3)) {
            fix_schema = ch->json_object = dirty = true;
            continue;
        }
        if (!strcmp(err.id, "400ctx") && !ctx_cleared) {
            ctx_cleared = dirty = true;
            hist_clear();
        }
        uint32_t delay;
        if (!or_retry_next(&rs, &err, deadline, &delay)) {
            goto done;
        }
        res.t.retries++;
        ESP_LOGW(TAG, "turn %" PRIu32 ": %s (%s), retry %" PRIu32 " in %" PRIu32 " ms", j->turn, err.id,
                 err.message, res.t.retries, delay);
        ai_event_t ev = { .type = AI_EV_RETRY, .turn = j->turn, .i = (int32_t)res.t.retries, .err = &err };
        ai_emit(&ev);
        if (!ai_sleep_ms(delay, ai_abort_turn, &ch->turn)) {
            or_err_local(&err, "AB");
            goto done;
        }
    }

    // Protocol §2.8 auto mode: the model asked for current data, so the transcript goes again with web search.
    if (ch->web == AI_WEB_AUTO && pr.needs_web && !pr.truncated) {
        const char *query = (!voice_in || res.stt_used) ? (stt_text ? stt_text : j->text) : pr.transcript;
        if (query && query[0]) {
            if (voice_in) {
                ai_event_t ev = { .type = AI_EV_RELEASED, .turn = j->turn };   // the re-run is text only
                ai_emit(&ev);
                voice_in_released = true;
            }
            if (!web_rerun(ch, query, &pr, &res)) {
                // cancelled meanwhile (barge-in, PWR): nothing is shown, but the requests made were billed
                res.cost += pr.cost;
                ai_client_add_cost(res.cost);
                or_err_local(&err, "AB");
                failed = true;
            }
        }
    }

done:
    if (voice_in && !voice_in_released) {
        ai_event_t ev = { .type = AI_EV_RELEASED, .turn = j->turn };
        ai_emit(&ev);
    }
    if (tr_retry) {
        res.tr_retry = true;
        if (failed && strcmp(err.id, "AB")) {
            // the direction retry itself failed: the first translation is better than an error card
            ESP_LOGW(TAG, "turn %" PRIu32 ": direction retry failed (%s), keeping the first translation", j->turn,
                     err.id);
            parsed_free(&pr);
            pr = tr_first;
            memset(&tr_first, 0, sizeof(tr_first));
            failed = false;
        } else if (!failed) {
            res.cost += tr_first.cost;           // both requests are billed
            pr.prompt_tokens += tr_first.prompt_tokens;
            pr.completion_tokens += tr_first.completion_tokens;
            pr.audio_tokens += tr_first.audio_tokens;
            if (voice_in && !res.stt_used && !tr_audio) {
                char *t = pr.transcript;         // the clip's transcript, not the echo of the text re-run
                pr.transcript = tr_first.transcript;
                tr_first.transcript = t;
            }
        }
    }
    res.t.ai_ms = (uint32_t)((esp_timer_get_time() - j->t_submit_us) / 1000);
    if (ch) {
        free_body(ch);
        memset(ch->key, 0, sizeof(ch->key));
    }
    if (failed) {
        emit_error(&res, &err);
    } else {
        res.cost += pr.cost;
        strlcpy(res.finish, pr.finish, sizeof(res.finish));
        strlcpy(res.model, pr.model, sizeof(res.model));
        strlcpy(res.provider, pr.provider, sizeof(res.provider));
        if (!res.gen_id[0]) {
            strlcpy(res.gen_id, pr.gen_id, sizeof(res.gen_id));   // the body "id" is the generation id
        }
        res.prompt_tokens = pr.prompt_tokens;
        res.completion_tokens = pr.completion_tokens;
        res.audio_tokens = pr.audio_tokens;
        res.reasoning_tokens = pr.reasoning_tokens;
        res.truncated = pr.truncated;
        res.web = ch->web == AI_WEB_ON && !res.web_failed && pr.text[0];
        if (res.web) {
            // the plugin asks for markdown citations: they are neither shown nor read aloud (the host is)
            ai_strip_links(pr.text, pr.source, sizeof(pr.source));
            strlcpy(res.source, pr.source, sizeof(res.source));
        }
        // typed and Whisper text are shown as the transcript; the model's echo is ignored
        res.transcript = (!voice_in || res.stt_used) ? (char *)(stt_text ? stt_text : (j->text ? j->text : ""))
                                                     : pr.transcript;
        res.text = pr.text;
        res.unheard = voice_in && !res.transcript[0];
        if (j->mode == AI_MODE_ASSISTANT) {
            strlcpy(res.lang, pr.lang[0] ? pr.lang : "ru", sizeof(res.lang));
            strlcpy(res.tts_lang, res.lang, sizeof(res.tts_lang));
        } else {
            strlcpy(res.src_lang, pr.src_lang[0] ? pr.src_lang : "other", sizeof(res.src_lang));
            res.other_lang = !strcmp(res.src_lang, "other");
            // the target the answer was checked against (fixed by a direction retry), else by src_lang
            const char *target = pr.tr_tgt[0] ? pr.tr_tgt : !strcmp(res.src_lang, j->lang_a) ? j->lang_b : j->lang_a;
            strlcpy(res.lang, target, sizeof(res.lang));
            bool ignore = res.other_lang && cfg_int("tr_other", 0) == 1;
            strlcpy(res.tts_lang, ignore ? "" : target, sizeof(res.tts_lang));
        }
        if (res.unheard || !res.text[0]) {
            res.tts_lang[0] = 0;
        }
        // a failed web search (W1) needs no notice: its reply already says so
        res.notice_ru = res.unheard ? NOTICE_P4 : (res.truncated ? NOTICE_P3 : NULL);
        // a holding phrase whose web search failed is not an answer worth remembering
        if (j->mode == AI_MODE_ASSISTANT && !res.unheard && !res.truncated && !res.web_failed && res.transcript[0] &&
            res.text[0]) {
            hist_add(res.transcript, res.text, res.lang);
        }
        ai_client_add_cost(res.cost);
        ESP_LOGI(TAG, "turn %" PRIu32 " ok: %s via %s (%s), %" PRIu32 " ms, $%.6f%s", j->turn, res.model, res.provider,
                 res.gen_id, res.t.ai_ms, res.cost, res.web ? ", web" : res.web_failed ? ", web failed" : "");
        finish_last(&res, NULL);
        ai_event_t ev = { .type = AI_EV_RESULT, .turn = j->turn, .result = &res };
        ai_emit(&ev);
    }
    parsed_free(&pr);
    parsed_free(&tr_first);
    free(stt_text);
    free(ch);
}

// ---- prewarm ----------------------------------------------------------------------------------

// True when a turn (or another job) is already waiting: the pre-warm would only delay it.
static bool job_waiting(void)
{
    return uxQueueMessagesWaiting(s_q) > 0;
}

bool ai_job_waiting(void *ctx)
{
    (void)ctx;
    return job_waiting();
}

static void run_prewarm(void)
{
    char base[136];
    cfg_str("ai_url", base, sizeof(base), "https://openrouter.ai/api/v1");
    or_conn_t *c = or_conn_get(base);
    if (!c || or_conn_live(c) || !ai_net_up() || job_waiting()) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    char model[100];
    cfg_str("ai_model", model, sizeof(model), "google/gemini-3.5-flash-lite");
    ai_prewarm_gets(model, job_waiting);
    ESP_LOGI(TAG, "prewarm %" PRId64 " ms", (esp_timer_get_time() - t0) / 1000);
}

esp_err_t ai_client_prewarm(void)
{
    ESP_RETURN_ON_FALSE(s_task, ESP_ERR_INVALID_STATE, TAG, "not started");
    char base[136];
    cfg_str("ai_url", base, sizeof(base), "https://openrouter.ai/api/v1");
    or_conn_t *c = or_conn_get(base);
    if (or_conn_live(c)) {
        return ESP_OK;
    }
    job_t *j = ai_calloc(1, sizeof(job_t));
    ESP_RETURN_ON_FALSE(j, ESP_ERR_NO_MEM, TAG, "job");
    j->type = JOB_PREWARM;
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        free(j);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

// ---- task and submit --------------------------------------------------------------------------

static void ai_task(void *arg)
{
    for (;;) {
        job_t *j;
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (j->type) {
        case JOB_TURN:
            run_turn(j);
            free(j->text);
            free(j);
            break;
        case JOB_SYNC:
            j->rc = j->fn(j->ctx);
            xSemaphoreGive(j->done);           // j lives on the caller's stack
            break;
        case JOB_PREWARM:
            run_prewarm();
            free(j);
            break;
        }
    }
}

esp_err_t ai_client_submit(const ai_request_t *r)
{
    ESP_RETURN_ON_FALSE(s_task, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(r && r->turn, ESP_ERR_INVALID_ARG, TAG, "turn");
    ESP_RETURN_ON_FALSE((r->pcm && r->samples) || r->text, ESP_ERR_INVALID_ARG, TAG, "no input");
    job_t *j = ai_calloc(1, sizeof(job_t));
    ESP_RETURN_ON_FALSE(j, ESP_ERR_NO_MEM, TAG, "job");
    j->type = JOB_TURN;
    j->turn = r->turn;
    j->mode = r->mode;
    j->pcm = r->pcm;
    j->samples = r->pcm ? r->samples : 0;
    j->rate = r->rate ? r->rate : 16000;
    j->t_submit_us = esp_timer_get_time();
    if (!j->pcm) {
        j->text = ai_strdup(r->text);
        if (!j->text) {
            free(j);
            return ESP_ERR_NO_MEM;
        }
    }
    // Translator languages: ISO codes, A != B (the UI enforces it; repair bad values here).
    char a[8], b[8];
    if (r->lang_a) {
        strlcpy(a, r->lang_a, sizeof(a));
    } else {
        cfg_str("tr_lang_a", a, sizeof(a), "ru");
    }
    if (r->lang_b) {
        strlcpy(b, r->lang_b, sizeof(b));
    } else {
        cfg_str("tr_lang_b", b, sizeof(b), "en");
    }
    lower_ascii(a);
    lower_ascii(b);
    if (!valid_lang(a)) {
        strlcpy(a, "ru", sizeof(a));
    }
    if (!valid_lang(b) || !strcmp(a, b)) {
        strlcpy(b, strcmp(a, "en") ? "en" : "ru", sizeof(b));
    }
    strlcpy(j->lang_a, a, sizeof(j->lang_a));
    strlcpy(j->lang_b, b, sizeof(j->lang_b));
    j->web = r->web && r->mode == AI_MODE_ASSISTANT;
    if (r->src_lang && r->mode == AI_MODE_TRANSLATOR && j->pcm) {
        char s[8];
        strlcpy(s, r->src_lang, sizeof(s));
        lower_ascii(s);
        if (!strcmp(s, a) || !strcmp(s, b)) {
            strlcpy(j->src, s, sizeof(j->src));
        }
    }
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        free(j->text);
        free(j);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ai_client_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    // Every cJSON tree and printed string goes to PSRAM.
    cJSON_Hooks hooks = { .malloc_fn = ai_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    hist_init();
    s_q = xQueueCreate(AI_Q_LEN, sizeof(job_t *));
    ESP_RETURN_ON_FALSE(s_q, ESP_ERR_NO_MEM, TAG, "queue");
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(ai_task, "ai", AI_STACK, NULL, AI_PRIO, &s_task, AI_CORE,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_RETURN_ON_ERROR(tts_client_init(), TAG, "tts");
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}
