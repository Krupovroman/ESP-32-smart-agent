// Turn engine tests: ai_client.c compiled into this TU to reach its statics.
#include "ai_client.c"
#include "mocks.h"
#include <math.h>

typedef struct {
    ai_ev_type_t type;
    uint32_t turn;
    int32_t i;
    char id[8];
    char transcript[512], text[512], lang[8], src_lang[8], tts_lang[8];
    bool truncated, unheard, stt_used;
    bool web, web_failed;
    char source[48], notice[64];
    uint32_t retries, web_ms;
    double cost;
} ev_t;
static ev_t s_ev[64];
static int s_nev;

static void cap(const ai_event_t *e, void *ctx)
{
    (void)ctx;
    ev_t *v = &s_ev[s_nev++ % 64];
    memset(v, 0, sizeof(*v));
    v->type = e->type;
    v->turn = e->turn;
    v->i = e->i;
    if (e->err) strlcpy(v->id, e->err->id, sizeof(v->id));
    if (e->result) {
        strlcpy(v->transcript, e->result->transcript, sizeof(v->transcript));
        strlcpy(v->text, e->result->text, sizeof(v->text));
        strlcpy(v->lang, e->result->lang, sizeof(v->lang));
        strlcpy(v->src_lang, e->result->src_lang, sizeof(v->src_lang));
        strlcpy(v->tts_lang, e->result->tts_lang, sizeof(v->tts_lang));
        v->truncated = e->result->truncated;
        v->unheard = e->result->unheard;
        v->stt_used = e->result->stt_used;
        v->web = e->result->web;
        v->web_failed = e->result->web_failed;
        strlcpy(v->source, e->result->source, sizeof(v->source));
        strlcpy(v->notice, e->result->notice_ru ? e->result->notice_ru : "", sizeof(v->notice));
        v->retries = e->result->t.retries;
        v->web_ms = e->result->t.web_ms;
        v->cost = e->result->cost;
    }
}

static const ev_t *find_ev(ai_ev_type_t t)
{
    for (int i = 0; i < s_nev; i++) if (s_ev[i].type == t) return &s_ev[i];
    return NULL;
}
static int count_ev(ai_ev_type_t t)
{
    int n = 0;
    for (int i = 0; i < s_nev; i++) n += s_ev[i].type == t;
    return n;
}

static const char *ok_body(const char *content_json_escaped, const char *finish)
{
    static char b[4096];
    snprintf(b, sizeof(b),
             "{\"id\":\"gen-1\",\"provider\":\"Google AI Studio\",\"model\":\"google/gemini-3.1-flash-lite\","
             "\"choices\":[{\"index\":0,\"finish_reason\":\"%s\",\"message\":{\"role\":\"assistant\",\"content\":\"%s\","
             "\"refusal\":null}}],\"usage\":{\"prompt_tokens\":842,\"completion_tokens\":64,\"cost\":0.000347,"
             "\"prompt_tokens_details\":{\"audio_tokens\":160}}}", finish, content_json_escaped);
    return b;
}
#define ASSIST_OK "{\\\"transcript\\\":\\\"Сколько будет два плюс два?\\\",\\\"reply\\\":\\\"Четыре.\\\",\\\"lang\\\":\\\"ru\\\"}"

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static size_t b64dec(const char *s, uint8_t *out)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *s && *s != '='; s++) {
        int v = b64v(*s);
        if (v < 0) return (size_t)-1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

static job_t mkjob(ai_mode_t mode, const char *text, const int16_t *pcm, size_t n)
{
    job_t j = { .type = JOB_TURN, .turn = ai_client_new_turn(), .mode = mode, .pcm = pcm, .samples = n,
                .rate = 16000, .t_submit_us = esp_timer_get_time() };
    j.text = text ? ai_strdup(text) : NULL;
    strlcpy(j.lang_a, "ru", sizeof(j.lang_a));
    strlcpy(j.lang_b, "en", sizeof(j.lang_b));
    return j;
}
static void run(job_t *j)
{
    s_nev = 0;
    run_turn(j);
    free(j->text);
}

static int16_t *s_pcm;
static size_t s_npcm;

static void setup(void)
{
    mock_reset();
    host_settings_reset();
    host_set("ai_url", "https://relay.example/SECRET/api/v1");
    host_set("ai_key", "sk-or-v1-testkey");
    host_set("ai_maxtok", "1024");
    host_set("ai_effort", "auto");
    hist_clear();
    // force a fresh connection for every test
    or_conn_t *c = or_conn_get("https://relay.example/SECRET/api/v1");
    or_conn_close(c);
}

static void t_voice_body(void)
{
    printf("t_voice_body\n");
    setup();
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, NULL, s_pcm, s_npcm);
    run(&j);
    CHECK(g_len_mismatch == 0);
    CHECK(g_nreq == 1);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(r && !strcmp(r->text, "Четыре.") && !strcmp(r->lang, "ru") && !strcmp(r->tts_lang, "ru"));
    CHECK(r && !strcmp(r->transcript, "Сколько будет два плюс два?"));
    CHECK(find_ev(AI_EV_UPLOAD_DONE) && find_ev(AI_EV_RELEASED));
    mock_req_t *q = &g_reqs[0];
    CHECK(!strcmp(q->method, "POST"));
    CHECK(!strcmp(q->url, "https://relay.example/SECRET/api/v1/chat/completions"));
    CHECK(!strcmp(q->auth, "Bearer sk-or-v1-testkey"));
    CHECK(!strcmp(q->accept, "application/json") && !strcmp(q->ctype, "application/json"));
    CHECK(!strcmp(q->title, "ESP32 Round Assistant"));
    cJSON *root = cJSON_Parse(q->body);
    CHECK(root != NULL);
    if (!root) {
        printf("%.300s\n", q->body);
        return;
    }
    cJSON *models = cJSON_GetObjectItem(root, "models");
    CHECK(cJSON_GetArraySize(models) == 2 && !cJSON_GetObjectItem(root, "model"));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(root, "stream")));
    CHECK(cJSON_GetObjectItem(root, "max_tokens")->valueint == 1024);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "provider"), "require_parameters")));
    cJSON *rs = cJSON_GetObjectItem(root, "reasoning");
    CHECK(rs && !strcmp(cJSON_GetObjectItem(rs, "effort")->valuestring, "minimal") &&
          cJSON_IsTrue(cJSON_GetObjectItem(rs, "exclude")));
    CHECK(!cJSON_GetObjectItem(root, "temperature"));
    cJSON *rf = cJSON_GetObjectItem(root, "response_format");
    CHECK(!strcmp(cJSON_GetObjectItem(rf, "type")->valuestring, "json_schema"));
    cJSON *js = cJSON_GetObjectItem(rf, "json_schema");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(js, "strict")) &&
          !strcmp(cJSON_GetObjectItem(js, "name")->valuestring, "assistant_turn"));
    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    CHECK(cJSON_GetArraySize(msgs) == 2);
    cJSON *sys = cJSON_GetArrayItem(msgs, 0);
    const char *sp = cJSON_GetObjectItem(sys, "content")->valuestring;
    CHECK(strstr(sp, "Ты — голосовой ассистент") != NULL);
    CHECK(strstr(sp, "{DATETIME}") == NULL && strstr(sp, "Сейчас ") != NULL);
    printf("    datetime line: %s\n", strstr(sp, "Сейчас "));
    cJSON *user = cJSON_GetArrayItem(msgs, 1);
    cJSON *parts = cJSON_GetObjectItem(user, "content");
    CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 0), "text")->valuestring,
                  "Голосовое сообщение пользователя."));
    cJSON *ia = cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 1), "input_audio");
    CHECK(!strcmp(cJSON_GetObjectItem(ia, "format")->valuestring, "wav"));
    const char *data = cJSON_GetObjectItem(ia, "data")->valuestring;
    uint8_t *wav = malloc(strlen(data));
    size_t wl = b64dec(data, wav);
    CHECK(wl != (size_t)-1 && wl >= 44 && !memcmp(wav, "RIFF", 4) && !memcmp(wav + 8, "WAVEfmt ", 8));
    uint32_t rate = wav[24] | wav[25] << 8 | wav[26] << 16 | (uint32_t)wav[27] << 24;
    uint32_t dlen = wav[40] | wav[41] << 8 | wav[42] << 16 | (uint32_t)wav[43] << 24;
    CHECK(rate == 16000 && dlen == wl - 44);
    // the trimmed clip: silence before 0.5 s and after 1.5 s, 200 ms margins
    size_t samples = dlen / 2;
    printf("    clip: %zu of %zu samples sent (%.2f s)\n", samples, s_npcm, samples / 16000.0);
    CHECK(samples >= 16000 && samples <= 16000 + 2 * 3200 + 640);
    // the PCM is a contiguous slice of the original clip
    const int16_t *sent = (const int16_t *)(wav + 44);
    bool found = false;
    for (size_t off = 0; off + samples <= s_npcm && !found; off += 320) {
        found = !memcmp(s_pcm + off, sent, samples * 2);
    }
    CHECK(found);
    free(wav);
    cJSON_Delete(root);
    // redacted request: no key, no audio
    char red[16384];
    ai_client_last_request_redacted(red, sizeof(red));
    CHECK(strstr(red, "sk-or-v1") == NULL && strstr(red, "base64 chars of WAV") != NULL && strstr(red, "UklGR") == NULL);
}

static void t_typed_history(void)
{
    printf("t_typed_history\n");
    setup();
    host_set("ai_model_fb", "");
    for (int k = 0; k < 3; k++) {
        mock_push_json(200, ok_body(ASSIST_OK, "stop"));
        job_t j = mkjob(AI_MODE_ASSISTANT, "Сколько будет 2+2?", NULL, 0);
        run(&j);
        const ev_t *r = find_ev(AI_EV_RESULT);
        CHECK(r && !strcmp(r->transcript, "Сколько будет 2+2?"));   // typed text, not the echo
        CHECK(!find_ev(AI_EV_RELEASED));
    }
    CHECK(g_len_mismatch == 0);
    cJSON *root = cJSON_Parse(g_reqs[2].body);
    CHECK(root && cJSON_GetObjectItem(root, "model") && !cJSON_GetObjectItem(root, "models"));
    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    CHECK(cJSON_GetArraySize(msgs) == 6);   // system + 2 exchanges + user
    CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 1), "role")->valuestring, "user"));
    CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 2), "content")->valuestring, "Четыре."));
    CHECK(cJSON_IsString(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 5), "content")));
    cJSON_Delete(root);
    CHECK(g_reqs[1].reused && g_connects == 1);    // keep-alive reuse
    // model change clears the history
    host_set("ai_model", "google/gemini-3.5-flash-lite");
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, "x", NULL, 0);
    run(&j);
    root = cJSON_Parse(g_reqs[3].body);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root, "messages")) == 2);
    cJSON_Delete(root);
}

static void t_retries(void)
{
    printf("t_retries\n");
    setup();
    mock_reply_t r503 = { .status = 503, .content_type = "application/json",
                          .body = (const uint8_t *)"{\"error\":{\"code\":503,\"message\":\"overloaded\"}}",
                          .body_len = 46, .hdr_k = { "Retry-After" }, .hdr_v = { "1" } };
    mock_push(&r503);
    mock_push_json(200, "{\"error\":{\"code\":502,\"message\":\"provider down\",\"metadata\":{\"error_type\":\"provider_unavailable\"}}}");
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    int64_t t0 = g_now_us;
    run(&j);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(r && r->retries == 2);
    CHECK(count_ev(AI_EV_RETRY) == 2);
    CHECK(!strcmp(s_ev[0].id, "503") || !strcmp(s_ev[1].id, "503"));
    printf("    elapsed %lld ms with Retry-After 1 s and 502 1.5 s\n", (long long)((g_now_us - t0) / 1000));
}

static void t_errors(void)
{
    printf("t_errors\n");
    setup();
    mock_push_json(401, "{\"error\":{\"message\":\"User not found.\",\"code\":401}}");
    job_t j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    const ev_t *e = find_ev(AI_EV_ERROR);
    CHECK(e && !strcmp(e->id, "401") && g_nreq == 1);

    setup();
    mock_push_json(403, "{\"error\":{\"message\":\"This model is not available in your region.\",\"code\":403}}");
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    e = find_ev(AI_EV_ERROR);
    CHECK(e && !strcmp(e->id, "403r"));

    setup();
    mock_push_json(200, ok_body("", "content_filter"));
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    e = find_ev(AI_EV_ERROR);
    CHECK(e && !strcmp(e->id, "P5"));

    setup();
    mock_push_json(200, ok_body("not json at all", "stop"));
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    CHECK(find_ev(AI_EV_RESULT) && count_ev(AI_EV_RETRY) == 1);

    setup();
    mock_push_json(200, ok_body("not json", "stop"));
    mock_push_json(200, ok_body("still not", "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    e = find_ev(AI_EV_ERROR);
    CHECK(e && !strcmp(e->id, "P2") && g_nreq == 2);

    setup();
    g_net_up = false;
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    e = find_ev(AI_EV_ERROR);
    CHECK(e && !strcmp(e->id, "N0") && g_nreq == 0);
    g_net_up = true;

    setup();
    host_set("ai_key", "");
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    e = find_ev(AI_EV_ERROR);
    CHECK(e && !strcmp(e->id, "K0"));

    setup();
    mock_reply_t dns = { .fail_connect = true, .tls_err = 0x8001 };
    mock_push(&dns);
    mock_push(&dns);
    mock_push(&dns);
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    e = find_ev(AI_EV_ERROR);
    CHECK(e && !strcmp(e->id, "N1") && g_nreq == 3);   // 2 retries
}

static void t_autofix(void)
{
    printf("t_autofix\n");
    setup();
    mock_push_json(400, "{\"error\":{\"message\":\"Unsupported parameter: reasoning.effort\",\"code\":400}}");
    mock_push_json(400, "{\"error\":{\"message\":\"Invalid response_format json_schema\",\"code\":400}}");
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    CHECK(find_ev(AI_EV_RESULT) != NULL);
    CHECK(g_nreq == 3);
    CHECK(strstr(g_reqs[0].body, "\"reasoning\"") && !strstr(g_reqs[1].body, "\"reasoning\""));
    CHECK(strstr(g_reqs[2].body, "\"json_object\"") && strstr(g_reqs[2].body, "Return only a JSON object"));
    CHECK(!strstr(g_reqs[2].body, "\"json_schema\""));

    setup();
    mock_push_json(400, "{\"error\":{\"code\":400,\"message\":\"too long\",\"metadata\":{\"error_type\":\"context_length_exceeded\"}}}");
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    CHECK(find_ev(AI_EV_RESULT) && count_ev(AI_EV_RETRY) == 1);
}

static void t_truncated(void)
{
    printf("t_truncated\n");
    setup();
    mock_push_json(200, ok_body("{\\\"transcript\\\":\\\"Расскажи про Луну\\\",\\\"reply\\\":\\\"Луна — спутник Земли. Она", "length"));
    job_t j = mkjob(AI_MODE_ASSISTANT, "Расскажи про Луну", NULL, 0);
    run(&j);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(r && r->truncated && !strcmp(r->text, "Луна — спутник Земли. Она"));
    CHECK(hist_count() == 0);
}

static void t_reuse_dead(void)
{
    printf("t_reuse_dead\n");
    setup();
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, "one", NULL, 0);
    run(&j);
    mock_reply_t dead = { .fail_write_at_open = true };
    mock_push(&dead);
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "two", NULL, 0);
    run(&j);
    CHECK(find_ev(AI_EV_RESULT) && count_ev(AI_EV_RETRY) == 0 && g_connects == 2 && g_nreq == 3);

    // reused socket closed by FIN right after the request
    mock_reply_t fin = { .fin_before_headers = true };
    mock_push(&fin);
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "three", s_pcm, s_npcm);
    run(&j);
    CHECK(find_ev(AI_EV_RESULT) && count_ev(AI_EV_RETRY) == 0 && count_ev(AI_EV_UPLOAD_DONE) == 1);
    CHECK(g_len_mismatch == 0);
}

static void t_timeouts(void)
{
    printf("t_timeouts\n");
    setup();
    mock_reply_t slow = { .eagain_before_headers = 40 };   // > 30 s first byte
    mock_push(&slow);
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    CHECK(count_ev(AI_EV_RETRY) == 1 && find_ev(AI_EV_RETRY) && !strcmp(find_ev(AI_EV_RETRY)->id, "N6"));
    CHECK(find_ev(AI_EV_RESULT) != NULL);

    setup();
    static const char *body;
    body = ok_body(ASSIST_OK, "stop");
    mock_reply_t cut = { .status = 200, .content_type = "application/json", .body = (const uint8_t *)body,
                         .body_len = strlen(body), .cut_after = 100 };
    mock_push(&cut);
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    CHECK(find_ev(AI_EV_RETRY) && !strcmp(find_ev(AI_EV_RETRY)->id, "N7") && find_ev(AI_EV_RESULT));

    setup();
    mock_reply_t stall = { .status = 200, .content_type = "application/json", .body = (const uint8_t *)body,
                           .body_len = strlen(body), .eagain_every = 1 };
    mock_push(&stall);
    mock_push(&stall);
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    const ev_t *e = find_ev(AI_EV_ERROR);
    CHECK(e && (!strcmp(e->id, "N6") || !strcmp(e->id, "N8")));
    printf("    stalled body -> %s\n", e ? e->id : "?");
}

static void t_translator(void)
{
    printf("t_translator\n");
    setup();
    mock_push_json(200, ok_body("{\\\"src_lang\\\":\\\"en\\\",\\\"transcript\\\":\\\"Where is the station?\\\","
                                "\\\"translation\\\":\\\"Где вокзал?\\\"}", "stop"));
    job_t j = mkjob(AI_MODE_TRANSLATOR, "Where is the station?", NULL, 0);
    run(&j);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(r && !strcmp(r->src_lang, "en") && !strcmp(r->lang, "ru") && !strcmp(r->tts_lang, "ru"));
    cJSON *root = cJSON_Parse(g_reqs[0].body);
    cJSON *js = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "response_format"), "json_schema");
    cJSON *en = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(js, "schema"),
                                                                            "properties"), "src_lang"), "enum");
    CHECK(cJSON_GetArraySize(en) == 3 && !strcmp(cJSON_GetArrayItem(en, 0)->valuestring, "ru") &&
          !strcmp(cJSON_GetArrayItem(en, 2)->valuestring, "other"));
    CHECK(cJSON_GetObjectItem(root, "max_tokens")->valueint == 768);
    const char *sp = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "messages"), 0), "content")->valuestring;
    CHECK(strstr(sp, "between Russian (ru) and English (en)") != NULL);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root, "messages")) == 2);
    cJSON_Delete(root);

    // other language, tr_other = 1 -> nothing to speak
    setup();
    host_set("tr_other", "1");
    mock_push_json(200, ok_body("{\\\"src_lang\\\":\\\"de\\\",\\\"transcript\\\":\\\"Guten Morgen\\\","
                                "\\\"translation\\\":\\\"Доброе утро\\\"}", "stop"));
    j = mkjob(AI_MODE_TRANSLATOR, "Guten Morgen", NULL, 0);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(r && !strcmp(r->src_lang, "other") && r->tts_lang[0] == 0 && !strcmp(r->lang, "ru"));

    // truncated translator answer: src_lang is salvaged too
    setup();
    mock_push_json(200, ok_body("{\\\"src_lang\\\":\\\"en\\\",\\\"transcript\\\":\\\"Long story\\\",\\\"translation\\\":\\\"Длинная ист", "length"));
    j = mkjob(AI_MODE_TRANSLATOR, "Long story", NULL, 0);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(r && r->truncated && !strcmp(r->src_lang, "en") && !strcmp(r->lang, "ru") && !strcmp(r->text, "Длинная ист"));

    // a bare 404 (wrong relay secret) is not "model not found"
    setup();
    mock_reply_t bare = { .status = 404, .content_type = "text/plain", .body = (const uint8_t *)"Not Found", .body_len = 9 };
    mock_push(&bare);
    j = mkjob(AI_MODE_TRANSLATOR, "x", NULL, 0);
    run(&j);
    const ev_t *e404 = find_ev(AI_EV_ERROR);
    CHECK(e404 && !strcmp(e404->id, "H404"));
    printf("    bare 404 -> %s\n", e404 ? e404->id : "?");

    // empty translation of real speech -> retried once, then P2
    setup();
    mock_push_json(200, ok_body("{\\\"src_lang\\\":\\\"ru\\\",\\\"transcript\\\":\\\"Привет\\\",\\\"translation\\\":\\\"\\\"}", "stop"));
    mock_push_json(200, ok_body("{\\\"src_lang\\\":\\\"ru\\\",\\\"transcript\\\":\\\"Привет\\\",\\\"translation\\\":\\\"Hello\\\"}", "stop"));
    j = mkjob(AI_MODE_TRANSLATOR, "Привет", NULL, 0);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(r && !strcmp(r->text, "Hello") && !strcmp(r->tts_lang, "en"));

    // source forced by a held chip / "wrong language": one-value enum and a hint in the voice message
    setup();
    mock_push_json(200, ok_body("{\\\"src_lang\\\":\\\"en\\\",\\\"transcript\\\":\\\"Hi\\\",\\\"translation\\\":\\\"Привет\\\"}",
                                "stop"));
    j = mkjob(AI_MODE_TRANSLATOR, NULL, s_pcm, s_npcm);
    strlcpy(j.src, "en", sizeof(j.src));
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(r && !strcmp(r->src_lang, "en") && !strcmp(r->tts_lang, "ru"));
    root = cJSON_Parse(g_reqs[0].body);
    js = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "response_format"), "json_schema");
    en = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(js, "schema"), "properties"),
                                                 "src_lang"), "enum");
    CHECK(cJSON_GetArraySize(en) == 1 && !strcmp(cJSON_GetArrayItem(en, 0)->valuestring, "en"));
    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    cJSON *parts = cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, cJSON_GetArraySize(msgs) - 1), "content");
    CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 0), "text")->valuestring, "Utterance in English:"));
    cJSON_Delete(root);
}

static void t_stt(void)
{
    printf("t_stt\n");
    setup();
    host_set("stt_mode", "1");
    mock_push_json(200, "{\"text\":\"Какая столица Франции?\",\"usage\":{\"seconds\":2.1,\"cost\":0.00002}}");
    mock_push_json(200, ok_body("{\\\"transcript\\\":\\\"echo\\\",\\\"reply\\\":\\\"Париж.\\\",\\\"lang\\\":\\\"ru\\\"}", "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, NULL, s_pcm, s_npcm);
    run(&j);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(r && r->stt_used && !strcmp(r->transcript, "Какая столица Франции?") && !strcmp(r->text, "Париж."));
    CHECK(r && fabs(r->cost - (0.00002 + 0.000347)) < 1e-9);
    CHECK(g_nreq == 2 && strstr(g_reqs[0].url, "/audio/transcriptions") != NULL);
    cJSON *s = cJSON_Parse(g_reqs[0].body);
    CHECK(s && !strcmp(cJSON_GetObjectItem(s, "language")->valuestring, "ru") &&
          !strcmp(cJSON_GetObjectItem(s, "model")->valuestring, "openai/whisper-large-v3-turbo") &&
          cJSON_GetObjectItem(s, "temperature")->valueint == 0);
    cJSON_Delete(s);
    CHECK(strstr(g_reqs[1].body, "input_audio") == NULL);
    CHECK(g_len_mismatch == 0);

    // 404 for audio on the chat model -> STT, translator omits language
    setup();
    mock_push_json(404, "{\"error\":{\"message\":\"No endpoints found that support input audio\",\"code\":404}}");
    mock_push_json(200, "{\"text\":\"Hello there\",\"usage\":{\"cost\":0.00001}}");
    mock_push_json(200, ok_body("{\\\"src_lang\\\":\\\"en\\\",\\\"transcript\\\":\\\"x\\\",\\\"translation\\\":\\\"Привет\\\"}", "stop"));
    j = mkjob(AI_MODE_TRANSLATOR, NULL, s_pcm, s_npcm);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(r && r->stt_used && !strcmp(r->transcript, "Hello there"));
    s = cJSON_Parse(g_reqs[1].body);
    CHECK(s && !cJSON_GetObjectItem(s, "language"));
    cJSON_Delete(s);

    // a 404 about routing parameters (not audio) does not detour through Whisper
    setup();
    mock_push_json(404, "{\"error\":{\"message\":\"No endpoints found that can handle the requested parameters.\","
                        "\"code\":404}}");
    j = mkjob(AI_MODE_ASSISTANT, NULL, s_pcm, s_npcm);
    run(&j);
    const ev_t *e = find_ev(AI_EV_ERROR);
    CHECK(e && !strcmp(e->id, "404") && g_nreq == 1);

    // empty Whisper text -> P4 without chat
    setup();
    host_set("stt_mode", "1");
    mock_push_json(200, "{\"text\":\"\"}");
    j = mkjob(AI_MODE_ASSISTANT, NULL, s_pcm, s_npcm);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(r && r->unheard && r->tts_lang[0] == 0 && g_nreq == 1);
}

static void t_cancel(void)
{
    printf("t_cancel\n");
    setup();
    job_t j = mkjob(AI_MODE_ASSISTANT, NULL, s_pcm, s_npcm);
    ai_client_cancel(j.turn);
    run(&j);
    CHECK(!find_ev(AI_EV_ERROR) && !find_ev(AI_EV_RESULT) && find_ev(AI_EV_RELEASED) && g_nreq == 0);
}

// Protocol §2.1 rule 2 also for the table default ai_effort=minimal.
static void t_effort(void)
{
    printf("t_effort\n");
    setup();
    host_set("ai_effort", "minimal");
    host_set("ai_model", "google/gemini-3.7-flash");
    ai_model_t m = { .id = "google/gemini-3.7-flash", .audio = true, .structured = true, .mandatory = true,
                     .min_effort = "low" };
    ai_model_cache_put(&m);
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, "Сколько будет два плюс два?", NULL, 0);
    run(&j);
    CHECK(g_nreq == 1 && strstr(g_reqs[0].body, "\"effort\":\"low\"") != NULL);
    // a model whose entry has no reasoning: the object is omitted
    setup();
    host_set("ai_effort", "minimal");
    host_set("ai_model", "x/no-reasoning");
    ai_model_t m2 = { .id = "x/no-reasoning", .audio = true, .structured = true };
    ai_model_cache_put(&m2);
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "Сколько будет два плюс два?", NULL, 0);
    run(&j);
    CHECK(g_nreq == 1 && strstr(g_reqs[0].body, "\"reasoning\"") == NULL);
}

static void t_unheard_voice(void)
{
    printf("t_unheard_voice\n");
    setup();
    mock_push_json(200, ok_body("{\\\"transcript\\\":\\\"\\\",\\\"reply\\\":\\\"\\\",\\\"lang\\\":\\\"ru\\\"}", "stop"));
    job_t j = mkjob(AI_MODE_ASSISTANT, NULL, s_pcm, s_npcm);
    run(&j);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(r && r->unheard && r->tts_lang[0] == 0);
    CHECK(hist_count() == 0);
}

// ---- web search (protocol §2.8) ----

// A chat body in its own buffer (mock_push_json keeps the pointer; leaked on purpose): JSON-escaped content, cost
// and an optional url_citation annotation.
static const char *web_body(const char *content_esc, double cost, const char *cite_url)
{
    char ann[256] = "";
    if (cite_url) {
        snprintf(ann, sizeof(ann), ",\"annotations\":[{\"type\":\"url_citation\",\"url_citation\":{\"url\":\"%s\","
                 "\"title\":\"t\",\"content\":\"c\",\"start_index\":0,\"end_index\":1}}]", cite_url);
    }
    char *b = malloc(4096);
    snprintf(b, 4096,
             "{\"id\":\"gen-w\",\"provider\":\"Google AI Studio\",\"model\":\"google/gemini-3.1-flash-lite\","
             "\"choices\":[{\"index\":0,\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
             "\"content\":\"%s\",\"refusal\":null%s}}],\"usage\":{\"prompt_tokens\":900,\"completion_tokens\":40,"
             "\"cost\":%.6f}}", content_esc, ann, cost);
    return b;
}
#define Q_WEATHER "Какая погода в Москве?"
#define WEB_FIRST "{\\\"transcript\\\":\\\"" Q_WEATHER "\\\",\\\"needs_web\\\":true,\\\"reply\\\":\\\"Сейчас посмотрю.\\\"," \
                  "\\\"lang\\\":\\\"ru\\\"}"
#define WEB_ANSWER "{\\\"transcript\\\":\\\"" Q_WEATHER "\\\",\\\"reply\\\":\\\"Сейчас в Москве плюс пять градусов " \
                   "([gismeteo.ru](https://www.gismeteo.ru/weather-moscow/)).\\\",\\\"lang\\\":\\\"ru\\\"}"
#define WEB_CLEAN "Сейчас в Москве плюс пять градусов."

static const char *sys_of(cJSON *root)
{
    cJSON *m = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "messages"), 0);
    cJSON *c = cJSON_GetObjectItem(m, "content");
    return cJSON_IsString(c) ? c->valuestring : "";
}

static const char *last_user_text(cJSON *root)
{
    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    cJSON *c = cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, cJSON_GetArraySize(msgs) - 1), "content");
    return cJSON_IsString(c) ? c->valuestring : NULL;
}

// needs_web in the schema properties and in "required"
static bool has_needs_web(cJSON *root)
{
    cJSON *s = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "response_format"), "json_schema"),
                                   "schema");
    bool req = false;
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(s, "required")) {
        req |= cJSON_IsString(it) && !strcmp(it->valuestring, "needs_web");
    }
    cJSON *p = cJSON_GetObjectItem(cJSON_GetObjectItem(s, "properties"), "needs_web");
    return req && p && !strcmp(cJSON_GetObjectItem(p, "type")->valuestring, "boolean");
}

// max_results of the web plugin, -1 without it
static int web_results(cJSON *root)
{
    cJSON *pl = cJSON_GetObjectItem(root, "plugins");
    cJSON *w = cJSON_GetArrayItem(pl, 0);
    if (cJSON_GetArraySize(pl) != 1 || !w || strcmp(cJSON_GetObjectItem(w, "id")->valuestring, "web")) {
        return -1;
    }
    return cJSON_GetObjectItem(w, "max_results")->valueint;
}

static void t_web_auto(void)
{
    printf("t_web_auto\n");
    setup();
    mock_push_json(200, web_body(WEB_FIRST, 0.00012, NULL));
    mock_push_json(200, web_body(WEB_ANSWER, 0.0136, "https://www.gismeteo.ru/weather-moscow-4368/now/"));
    job_t j = mkjob(AI_MODE_ASSISTANT, Q_WEATHER, NULL, 0);
    run(&j);
    CHECK(g_nreq == 2 && g_len_mismatch == 0);
    const ev_t *w = find_ev(AI_EV_WEB);
    CHECK(w && w->i == 1);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(r && r->web && !r->web_failed && !strcmp(r->source, "gismeteo.ru") && !r->notice[0]);
    CHECK(r && !strcmp(r->text, WEB_CLEAN) && !strcmp(r->transcript, Q_WEATHER));
    CHECK(r && fabs(r->cost - (0.00012 + 0.0136)) < 1e-9);
    printf("    reply: %s | source %s | $%.6f\n", r ? r->text : "?", r ? r->source : "?", r ? r->cost : 0);
    CHECK(!find_ev(AI_EV_ERROR) && hist_count() == 1);
    cJSON *a = cJSON_Parse(g_reqs[0].body), *b = cJSON_Parse(g_reqs[1].body);
    CHECK(a && b);
    if (!a || !b) {
        return;
    }
    // first request: needs_web (before reply in the schema), no plugin, the auto rule in the prompt
    CHECK(has_needs_web(a) && web_results(a) < 0);
    CHECK(strstr(g_reqs[0].body, "\"needs_web\":{") < strstr(g_reqs[0].body, "\"reply\":{"));
    CHECK(strstr(sys_of(a), "поставь needs_web = true") && !strstr(sys_of(a), "нет доступа"));
    // re-run: plugin with ai_web_n results, no needs_web, the "results attached" rule, the transcript as text
    CHECK(!has_needs_web(b) && web_results(b) == 3);
    CHECK(strstr(sys_of(b), "результаты поиска в интернете") && !strstr(sys_of(b), "needs_web"));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(b, "messages")) == 2 && last_user_text(b) &&
          !strcmp(last_user_text(b), Q_WEATHER));
    CHECK(cJSON_GetObjectItem(b, "response_format") && cJSON_GetObjectItem(b, "reasoning"));
    cJSON_Delete(a);
    cJSON_Delete(b);
    char red[16384];
    ai_client_last_request_redacted(red, sizeof(red));
    CHECK(strstr(red, "\"plugins\":[{\"id\":\"web\",\"max_results\":3}]") != NULL);
    ai_last_t l;
    CHECK(ai_client_last(&l) && l.web && !l.web_failed);

    // the next turn carries the web answer (links stripped) in its history; ai_web_n sets max_results
    host_set("ai_web_n", "1");
    mock_push_json(200, web_body(WEB_FIRST, 0.00012, NULL));
    mock_push_json(200, web_body(WEB_ANSWER, 0.0136, NULL));
    j = mkjob(AI_MODE_ASSISTANT, Q_WEATHER, NULL, 0);
    run(&j);
    a = cJSON_Parse(g_reqs[2].body);
    b = cJSON_Parse(g_reqs[3].body);
    cJSON *hm = cJSON_GetArrayItem(cJSON_GetObjectItem(a, "messages"), 2);
    CHECK(hm && !strcmp(cJSON_GetObjectItem(hm, "content")->valuestring, WEB_CLEAN));
    CHECK(web_results(b) == 1 && cJSON_GetArraySize(cJSON_GetObjectItem(b, "messages")) == 4);
    r = find_ev(AI_EV_RESULT);
    CHECK(r && r->web && !strcmp(r->source, "gismeteo.ru"));     // no annotation: the host of the stripped link
    cJSON_Delete(a);
    cJSON_Delete(b);
}

static void t_web_voice(void)
{
    printf("t_web_voice\n");
    setup();
    mock_push_json(200, web_body("{\\\"transcript\\\":\\\"Какой сейчас курс доллара?\\\",\\\"needs_web\\\":true,"
                                 "\\\"reply\\\":\\\"Сейчас узнаю.\\\",\\\"lang\\\":\\\"ru\\\"}", 0.0004, NULL));
    mock_push_json(200, web_body("{\\\"transcript\\\":\\\"Какой сейчас курс доллара?\\\",\\\"reply\\\":\\\"Около "
                                 "восьмидесяти рублей.\\\",\\\"lang\\\":\\\"ru\\\"}", 0.0135,
                                 "https://cbr.ru/currency_base/daily/"));
    job_t j = mkjob(AI_MODE_ASSISTANT, NULL, s_pcm, s_npcm);
    run(&j);
    CHECK(g_nreq == 2 && g_len_mismatch == 0);
    CHECK(count_ev(AI_EV_UPLOAD_DONE) == 1 && count_ev(AI_EV_RELEASED) == 1);
    CHECK(strstr(g_reqs[0].body, "input_audio") && !strstr(g_reqs[1].body, "input_audio"));
    CHECK(g_reqs[1].len < 16384);                // the clip is not uploaded again
    cJSON *b = cJSON_Parse(g_reqs[1].body);
    CHECK(b && last_user_text(b) && !strcmp(last_user_text(b), "Какой сейчас курс доллара?"));
    cJSON_Delete(b);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(r && r->web && !strcmp(r->transcript, "Какой сейчас курс доллара?") &&
          !strcmp(r->text, "Около восьмидесяти рублей.") && !strcmp(r->source, "cbr.ru") && !strcmp(r->tts_lang, "ru"));
}

static void t_web_fail(void)
{
    printf("t_web_fail\n");
    // a server error on the web request: one attempt, then the first answer, no error card
    setup();
    mock_push_json(200, web_body(WEB_FIRST, 0.00012, NULL));
    mock_push_json(500, "{\"error\":{\"code\":500,\"message\":\"Internal Server Error\"}}");
    job_t j = mkjob(AI_MODE_ASSISTANT, Q_WEATHER, NULL, 0);
    run(&j);
    const ev_t *r = find_ev(AI_EV_RESULT);
    CHECK(g_nreq == 2 && !find_ev(AI_EV_ERROR) && count_ev(AI_EV_RETRY) == 0);
    CHECK(r && r->web_failed && !r->web && !strcmp(r->text, "Сейчас посмотрю.") && !r->source[0]);
    CHECK(r && !strcmp(r->notice, "(не удалось найти в интернете)") && fabs(r->cost - 0.00012) < 1e-9);
    CHECK(hist_count() == 0);                    // the holding phrase is not remembered
    ai_last_t l;
    CHECK(ai_client_last(&l) && l.ok && l.web_failed);

    // no first byte within 30 s: the web request gives up, the first answer stays
    setup();
    mock_push_json(200, web_body(WEB_FIRST, 0.00012, NULL));
    mock_reply_t slow = { .eagain_before_headers = 40 };
    mock_push(&slow);
    j = mkjob(AI_MODE_ASSISTANT, Q_WEATHER, NULL, 0);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(g_nreq == 2 && r && r->web_failed && !find_ev(AI_EV_ERROR));
    CHECK(r && r->web_ms >= 29000 && r->web_ms <= 31500);
    printf("    web request timed out after %u ms\n", r ? (unsigned)r->web_ms : 0u);

    // a web reply that does not parse: P2, first answer, its cost still counted
    setup();
    mock_push_json(200, web_body(WEB_FIRST, 0.00012, NULL));
    mock_push_json(200, web_body("not json", 0.0130, NULL));
    j = mkjob(AI_MODE_ASSISTANT, Q_WEATHER, NULL, 0);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(g_nreq == 2 && r && r->web_failed && fabs(r->cost - (0.00012 + 0.0130)) < 1e-9);

    // the first request fails: the normal error path, no web request
    setup();
    mock_push_json(401, "{\"error\":{\"message\":\"User not found.\",\"code\":401}}");
    j = mkjob(AI_MODE_ASSISTANT, Q_WEATHER, NULL, 0);
    run(&j);
    CHECK(g_nreq == 1 && find_ev(AI_EV_ERROR) && !find_ev(AI_EV_WEB));
}

static void t_web_modes(void)
{
    printf("t_web_modes\n");
    // always: the first request (voice included) carries the plugin, no needs_web, one request
    setup();
    host_set("ai_web", "2");
    mock_push_json(200, web_body("{\\\"transcript\\\":\\\"Что нового?\\\",\\\"reply\\\":\\\"Сегодня тихо.\\\","
                                 "\\\"lang\\\":\\\"ru\\\"}", 0.0138, "https://ria.ru/20261018/x.html"));
    job_t j = mkjob(AI_MODE_ASSISTANT, NULL, s_pcm, s_npcm);
    run(&j);
    const ev_t *w = find_ev(AI_EV_WEB), *r = find_ev(AI_EV_RESULT);
    CHECK(g_nreq == 1 && g_len_mismatch == 0 && w && w->i == 0);
    CHECK(r && r->web && !strcmp(r->source, "ria.ru") && !strcmp(r->text, "Сегодня тихо."));
    cJSON *a = cJSON_Parse(g_reqs[0].body);
    CHECK(a && web_results(a) == 3 && !has_needs_web(a) && strstr(g_reqs[0].body, "input_audio"));
    CHECK(a && strstr(sys_of(a), "результаты поиска в интернете"));
    cJSON_Delete(a);

    // always, but the relay rejects the plugin: sent again without it (an auto-fix, not a retry)
    setup();
    host_set("ai_web", "2");
    mock_push_json(400, "{\"error\":{\"message\":\"Plugin web is not available for this endpoint\",\"code\":400}}");
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(g_nreq == 2 && r && !r->web && count_ev(AI_EV_RETRY) == 0);
    a = cJSON_Parse(g_reqs[1].body);
    CHECK(a && web_results(a) < 0 && strstr(sys_of(a), "нет доступа к интернету"));
    cJSON_Delete(a);

    // off: no field, no plugin, the old rule; a needs_web in the reply changes nothing
    setup();
    host_set("ai_web", "0");
    mock_push_json(200, web_body(WEB_FIRST, 0.0001, NULL));
    j = mkjob(AI_MODE_ASSISTANT, Q_WEATHER, NULL, 0);
    run(&j);
    r = find_ev(AI_EV_RESULT);
    CHECK(g_nreq == 1 && !find_ev(AI_EV_WEB) && r && !r->web && !r->web_failed);
    a = cJSON_Parse(g_reqs[0].body);
    CHECK(a && !has_needs_web(a) && web_results(a) < 0 && strstr(sys_of(a), "нет доступа к интернету"));
    cJSON_Delete(a);

    // console "ai ask -w": forced on the first request whatever ai_web says
    setup();
    host_set("ai_web", "0");
    mock_push_json(200, web_body(WEB_ANSWER, 0.0136, NULL));
    j = mkjob(AI_MODE_ASSISTANT, Q_WEATHER, NULL, 0);
    j.web = true;
    run(&j);
    r = find_ev(AI_EV_RESULT);
    a = cJSON_Parse(g_reqs[0].body);
    CHECK(g_nreq == 1 && a && web_results(a) == 3 && r && r->web && !strcmp(r->text, WEB_CLEAN));
    cJSON_Delete(a);

    // the translator never searches
    setup();
    host_set("ai_web", "2");
    mock_push_json(200, ok_body("{\\\"src_lang\\\":\\\"en\\\",\\\"transcript\\\":\\\"Hi\\\",\\\"translation\\\":"
                                "\\\"Привет\\\"}", "stop"));
    j = mkjob(AI_MODE_TRANSLATOR, "Hi", NULL, 0);
    j.web = true;
    run(&j);
    CHECK(g_nreq == 1 && !strstr(g_reqs[0].body, "plugins") && !strstr(g_reqs[0].body, "needs_web"));
    CHECK(!find_ev(AI_EV_WEB) && find_ev(AI_EV_RESULT) && !find_ev(AI_EV_RESULT)->web);

    // other providers ignore the setting
    setup();
    host_set("ai_url", "https://api.openai.com/v1");
    host_set("ai_web", "2");
    mock_push_json(200, ok_body(ASSIST_OK, "stop"));
    j = mkjob(AI_MODE_ASSISTANT, "hi", NULL, 0);
    run(&j);
    a = cJSON_Parse(g_reqs[0].body);
    CHECK(g_nreq == 1 && a && web_results(a) < 0 && !has_needs_web(a) && strstr(sys_of(a), "нет доступа"));
    cJSON_Delete(a);
}

void test_ai(void)
{
    ai_add_listener(cap, NULL);
    cJSON_Hooks hooks = { .malloc_fn = ai_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    hist_init();
    s_task = xTaskGetCurrentTaskHandle();     // ai_run_sync() runs inline, as in the ai task
    // 2 s clip: 0.5 s of noise floor (-50 dBFS), 1 s of a 440 Hz tone, 0.5 s of noise
    s_npcm = 32000;
    s_pcm = malloc(s_npcm * 2);
    srand(1);
    for (size_t i = 0; i < s_npcm; i++) {
        double v = (rand() % 200 - 100);
        if (i >= 8000 && i < 24000) v += 8000 * sin(2 * 3.14159 * 440 * i / 16000.0);
        s_pcm[i] = (int16_t)v;
    }
    t_voice_body();
    t_typed_history();
    t_retries();
    t_errors();
    t_autofix();
    t_truncated();
    t_reuse_dead();
    t_timeouts();
    t_translator();
    t_stt();
    t_cancel();
    t_effort();
    t_unheard_voice();
    t_web_auto();
    t_web_voice();
    t_web_fail();
    t_web_modes();
}
