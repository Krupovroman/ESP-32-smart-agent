// TTS streaming tests: tts_client.c compiled into this TU.
#include "tts_client.c"
#include "mocks.h"

static int s_first, s_done, s_err;
static char s_err_id[8];
static tts_stats_t s_st;

static void cap(const ai_event_t *e, void *ctx)
{
    (void)ctx;
    if (e->type == AI_EV_TTS_FIRST_AUDIO) s_first++;
    if (e->type == AI_EV_TTS_DONE) { s_done++; s_st = *e->tts; }
    if (e->type == AI_EV_TTS_ERROR) { s_err++; s_st = *e->tts; strlcpy(s_err_id, e->err->id, sizeof(s_err_id)); }
}

static uint8_t *pcm_body(size_t n, uint8_t seed)
{
    uint8_t *b = malloc(n);
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 7 + seed);
    return b;
}

static void speak(uint32_t turn, const char *text, const char *lang)
{
    s_first = s_done = s_err = 0;
    s_err_id[0] = 0;
    memset(&s_st, 0, sizeof(s_st));
    tts_job_t j = { .turn = turn, .text = ai_strdup(text) };
    strlcpy(j.lang, lang, sizeof(j.lang));
    run_speak(&j);
    free(j.text);
}

static void setup(void)
{
    mock_reset();
    host_settings_reset();
    host_set("ai_url", "https://relay.example/SECRET/api/v1");
    host_set("ai_key", "sk-or-v1-testkey");
    or_conn_close(or_conn_get("https://relay.example/SECRET/api/v1"));
    s_out_turn = 0;
    g_now_us += 3601LL * 1000000;    // failed routes expire after 1 h
}

static const char *TEXT3 =
    "Луна — единственный естественный спутник Земли. Она находится на расстоянии около трёхсот восьмидесяти "
    "четырёх тысяч километров. Её поверхность покрыта кратерами, которые образовались от ударов метеоритов "
    "много миллионов лет назад. Люди впервые высадились на Луну в тысяча девятьсот шестьдесят девятом году.";

static void t_stream(void)
{
    printf("t_stream\n");
    setup();
    char **ch;
    int n = tts_split(TEXT3, &ch);
    printf("    %d chunks\n", n);
    for (int i = 0; i < n; i++) printf("    [%u] %s\n", (unsigned)utf8_cp_len(ch[i]), ch[i]);
    size_t sizes[8] = { 4801, 24000, 9999, 3 };
    uint8_t *bodies[8];
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        bodies[i] = pcm_body(sizes[i % 4], (uint8_t)i);
        mock_reply_t r = { .status = 200, .content_type = "audio/pcm;rate=24000;channels=1", .body = bodies[i],
                           .body_len = sizes[i % 4], .chunked = true, .read_piece = 333 };
        mock_push(&r);
        total += sizes[i % 4] & ~(size_t)1;
    }
    speak(101, TEXT3, "ru");
    CHECK(s_done == 1 && s_first == 1 && s_err == 0);
    CHECK(g_out_rate == 24000 && g_out_ch == 1 && g_out_begins == 1);
    CHECK(s_st.chunks == (uint32_t)n && s_st.chunks_done == (uint32_t)n);
    printf("    out=%zu expected=%zu (odd tails dropped)\n", g_out_bytes, total);
    CHECK(g_out_bytes == total);
    CHECK(!memcmp(g_out_buf, bodies[0], 4800));
    cJSON *b = cJSON_Parse(g_reqs[0].body);
    CHECK(b && !strcmp(cJSON_GetObjectItem(b, "model")->valuestring, "microsoft/mai-voice-2.1-flash"));
    CHECK(!strcmp(cJSON_GetObjectItem(b, "voice")->valuestring, "ru-RU-Masha:MAI-Voice-2.1-Flash"));
    CHECK(!strcmp(cJSON_GetObjectItem(b, "response_format")->valuestring, "pcm"));
    CHECK(!cJSON_GetObjectItem(b, "speed"));
    CHECK(!strcmp(cJSON_GetObjectItem(b, "input")->valuestring, ch[0]));
    cJSON_Delete(b);
    CHECK(g_reqs[0].accept[0] == 0 && strstr(g_reqs[0].url, "/audio/speech"));
    CHECK(g_connects == 1);
    tts_split_free(ch, n);
    for (int i = 0; i < n; i++) free(bodies[i]);
}

static void t_wav_and_routes(void)
{
    printf("t_wav_and_routes\n");
    setup();
    // MAI 404 -> Gemini answers with a WAV image at 22050 Hz
    mock_push_json(404, "{\"error\":{\"message\":\"Model not found\",\"code\":404}}");
    uint8_t wav[44 + 2000];
    or_wav_header(wav, 22050, 1000);
    memcpy(wav + 44, pcm_body(2000, 9), 2000);
    mock_reply_t r = { .status = 200, .content_type = "audio/wav", .body = wav, .body_len = sizeof(wav),
                       .read_piece = 7 };
    mock_push(&r);
    speak(102, "Короткая проверка голоса для теста.", "ru");
    CHECK(s_done == 1 && g_out_rate == 22050 && g_out_bytes == 2000);
    CHECK(g_nreq == 2 && strstr(g_reqs[1].body, "google/gemini-3.8-flash-lite-tts") && strstr(g_reqs[1].body, "Kore"));
    CHECK(s_st.route_switches == 1);
    CHECK(tts_route_failed("microsoft/mai-voice-2.1-flash", "ru"));
    // the failed route is skipped next time
    mock_reset();
    uint8_t *p = pcm_body(480, 1);
    mock_reply_t ok = { .status = 200, .content_type = "audio/pcm", .body = p, .body_len = 480 };
    mock_push(&ok);
    speak(103, "Ещё одна проверка голоса для теста.", "ru");
    CHECK(s_done == 1 && strstr(g_reqs[0].body, "gemini") && g_out_rate == 24000);
    free(p);
}

static void t_mpeg_and_errors(void)
{
    printf("t_mpeg_and_errors\n");
    setup();
    g_now_us += 3601LL * 1000000;
    uint8_t junk[100] = { 0xFF, 0xFB };
    mock_reply_t mp3 = { .status = 200, .content_type = "audio/mpeg", .body = junk, .body_len = 100 };
    mock_push(&mp3);
    uint8_t *p = pcm_body(960, 2);
    mock_reply_t ok = { .status = 200, .content_type = "audio/pcm;rate=24000", .body = p, .body_len = 960 };
    mock_push(&ok);
    speak(104, "Проверка формата ответа сервера.", "ru");
    CHECK(s_done == 1 && g_nreq == 2 && g_out_bytes == 960);

    // cut body: retried, playback resumes without repeating
    setup();
    g_now_us += 3601LL * 1000000;
    uint8_t *big = pcm_body(10000, 3);
    mock_reply_t cut = { .status = 200, .content_type = "audio/pcm;rate=24000;channels=1", .body = big,
                         .body_len = 10000, .chunked = true, .cut_after = 4001 };
    mock_push(&cut);
    mock_reply_t full = { .status = 200, .content_type = "audio/pcm;rate=24000;channels=1", .body = big,
                          .body_len = 10000, .chunked = true };
    mock_push(&full);
    speak(105, "Проверка обрыва ответа сервера.", "ru");
    CHECK(s_done == 1 && s_st.retries == 1);
    printf("    out=%zu (expected 10000)\n", g_out_bytes);
    CHECK(g_out_bytes == 10000 && !memcmp(g_out_buf, big, 10000));

    // speed rejected -> resent without speed
    setup();
    g_now_us += 3601LL * 1000000;
    host_set("tts_speed", "110");
    mock_push_json(400, "{\"error\":{\"message\":\"speed is not supported\",\"code\":400}}");
    mock_push(&ok);
    speak(106, "Проверка скорости речи для теста.", "ru");
    CHECK(s_done == 1 && strstr(g_reqs[0].body, "\"speed\":1.1") && !strstr(g_reqs[1].body, "speed"));

    // every route fails -> T3
    setup();
    g_now_us += 3601LL * 1000000;
    for (int i = 0; i < 6; i++) mock_push_json(403, "{\"error\":{\"message\":\"Forbidden\",\"code\":403}}");
    speak(107, "Все маршруты недоступны для проверки.", "ru");
    CHECK(s_err == 1 && !strcmp(s_err_id, "T3"));
    printf("    routes tried: %d\n", g_nreq);

    // 401 -> T1 at once
    setup();
    g_now_us += 3601LL * 1000000;
    mock_push_json(401, "{\"error\":{\"message\":\"User not found.\",\"code\":401}}");
    speak(108, "Ключ неверный, проверка ошибки.", "ru");
    CHECK(s_err == 1 && !strcmp(s_err_id, "T1") && g_nreq == 1);

    // cancel: nothing played, no event
    setup();
    g_now_us += 3601LL * 1000000;
    tts_client_cancel(109);
    speak(109, "Отменённая озвучка.", "ru");
    CHECK(s_err == 0 && s_done == 0 && g_nreq == 0);
    free(p);
    free(big);
}

static void t_routes_table(void)
{
    printf("t_routes_table\n");
    host_settings_reset();
    g_now_us += 3601LL * 1000000;
    const char *langs[] = { "ru", "en", "de", "ja", "ar", "uk", "other" };
    for (size_t i = 0; i < sizeof(langs) / sizeof(langs[0]); i++) {
        tts_route_t r[6];
        int n = tts_routes(langs[i], r, 6);
        printf("    %-5s", langs[i]);
        for (int k = 0; k < n; k++) printf(" %s|%s", r[k].model, r[k].voice);
        printf("\n");
    }
    tts_route_t r[6];
    CHECK(tts_routes("en", r, 6) >= 1 && !strcmp(r[0].voice, "en-US-Harper:MAI-Voice-2.1-Flash"));
    CHECK(tts_routes("uk", r, 6) >= 1 && !strcmp(r[0].model, "elevenlabs/eleven-flash-v2.5"));
    CHECK(tts_routes("ja", r, 6) >= 1 && !strcmp(r[0].model, "google/gemini-3.8-flash-lite-tts"));
    host_set("vo_uk", "x-ai/grok-voice-tts-1.0|leo");
    int n = tts_routes("uk", r, 6);
    CHECK(n >= 1 && strcmp(r[0].model, "x-ai/grok-voice-tts-1.0"));    // grok cannot speak uk: skipped
    host_set("tts_model", "google/gemini-3.8-flash-lite-tts");
    host_set("tts_voice", "Puck");
    CHECK(tts_routes("de", r, 6) >= 1 && !strcmp(r[0].voice, "Puck"));
    uint8_t chn;
    CHECK(tts_rate_for("microsoft/mai-voice-2.1-flash", "audio/pcm;rate=16000;channels=2", &chn) == 16000 && chn == 2);
    CHECK(tts_rate_for("heygen/heygen-voice-1-turbo", "audio/pcm", &chn) == 44100 && chn == 1);
    CHECK(tts_rate_for("microsoft/mai-voice-2.1-flash", "Audio/PCM; Sample_Rate=22050", &chn) == 22050);
    host_settings_reset();
}

void test_tts(void)
{
    ai_add_listener(cap, NULL);
    s_task = xTaskGetCurrentTaskHandle();
    t_routes_table();
    t_stream();
    t_wav_and_routes();
    t_mpeg_and_errors();
}
