// Picker, model check and key tests: ai_models.c compiled into this TU.
#include "ai_models.c"
#include "mocks.h"

static const char *PAGE1 =
    "{\"data\":["
    "{\"id\":\"google/gemini-3.1-flash-lite\",\"name\":\"Gemini 3.1 Flash Lite\",\"pricing\":{\"prompt\":\"0.00000025\","
    "\"completion\":\"0.0000015\",\"audio\":\"0.0000005\"},\"supported_parameters\":[\"response_format\","
    "\"structured_outputs\",\"reasoning\",\"max_tokens\"],\"architecture\":{\"input_modalities\":[\"text\",\"audio\"]},"
    "\"reasoning\":{\"mandatory\":false,\"supported_efforts\":[\"high\",\"medium\",\"low\",\"minimal\",\"none\"]},"
    "\"expiration_date\":null},"
    "{\"id\":\"google/gemini-3.1-flash-lite-preview\",\"name\":\"Preview\",\"pricing\":{\"prompt\":\"0.0000001\","
    "\"completion\":\"0.0000001\"},\"supported_parameters\":[\"response_format\",\"structured_outputs\"],"
    "\"architecture\":{\"input_modalities\":[\"audio\"]},\"expiration_date\":null},"
    "{\"id\":\"google/gemini-3.7-flash\",\"name\":\"Gemini 3.7 Flash\",\"pricing\":{\"prompt\":\"0.00000075\","
    "\"completion\":\"0.00000375\",\"audio\":\"0.00000075\"},\"supported_parameters\":[\"response_format\","
    "\"structured_outputs\"],\"architecture\":{\"input_modalities\":[\"audio\"]},"
    "\"reasoning\":{\"mandatory\":true,\"supported_efforts\":[\"high\",\"medium\",\"low\"]},\"expiration_date\":null},"
    "{\"id\":\"xiaomi/mimo-v2.5\",\"name\":\"MiMo\",\"pricing\":{\"prompt\":\"0.0000001\",\"completion\":\"0.0000002\"},"
    "\"supported_parameters\":[\"response_format\",\"structured_outputs\"],\"architecture\":{\"input_modalities\":"
    "[\"audio\"]},\"expiration_date\":\"2026-10-21\"},"
    "{\"id\":\"some/free:free\",\"name\":\"Free\",\"pricing\":{\"prompt\":\"0\",\"completion\":\"0\"},"
    "\"supported_parameters\":[],\"architecture\":{\"input_modalities\":[\"audio\"]}},"
    "{\"id\":\"openrouter/auto\",\"pricing\":{\"prompt\":\"-1\",\"completion\":\"-1\"}},"
    "{\"id\":\"perceptron/perceptron-mk1.5\",\"pricing\":{\"prompt\":\"0.0000001\",\"completion\":\"0.0000001\"},"
    "\"supported_parameters\":[\"max_tokens\"],\"architecture\":{\"input_modalities\":[\"audio\"]}},"
    "{\"id\":\"xiaomi/mimo-v2.6-flash\",\"name\":\"MiMo 2.6 Flash\",\"pricing\":{\"prompt\":\"0.00000014\","
    "\"completion\":\"0.00000028\"},\"supported_parameters\":[\"response_format\",\"structured_outputs\"],"
    "\"architecture\":{\"input_modalities\":[\"text\",\"audio\"]},\"reasoning\":null,\"expiration_date\":null}"
    "],\"total_count\":8,\"links\":{\"next\":null}}";

static const char *KEY =
    "{\"data\":{\"label\":\"sk-or-v1-au7...890\",\"limit\":100,\"limit_remaining\":74.5,\"limit_reset\":\"monthly\","
    "\"usage\":25.5,\"usage_daily\":0.04,\"usage_weekly\":1.5,\"usage_monthly\":1.2,\"is_free_tier\":false,"
    "\"expires_at\":\"2027-12-31T23:59:59Z\",\"effective_budget\":{\"scope\":\"member\",\"limit_usd\":50,"
    "\"spend_usd\":32.5,\"remaining_usd\":17.5,\"reset_interval\":\"weekly\",\"resets_at\":\"2026-11-01T00:00:00.000Z\"},"
    "\"free_model_daily_requests\":{\"limit\":50,\"remaining\":38,\"used\":12}}}";

static const char *TTS_PAGE =
    "{\"data\":["
    "{\"id\":\"microsoft/mai-voice-2.1-flash\",\"name\":\"MAI Voice 2.1 Flash\",\"pricing\":{\"prompt\":\"0.000015\","
    "\"completion\":\"0\"},\"architecture\":{\"tokenizer\":\"Other\"},\"supported_voices\":[\"ru-RU-Masha:MAI-Voice-2.1-Flash\","
    "\"ru-RU-Lev:MAI-Voice-2.1-Flash\",\"en-US-Harper:MAI-Voice-2.1-Flash\"],\"expiration_date\":null},"
    "{\"id\":\"google/gemini-3.8-flash-lite-tts\",\"name\":\"Gemini TTS\",\"pricing\":{\"prompt\":\"0.0000005\","
    "\"completion\":\"0.000006\"},\"architecture\":{\"tokenizer\":\"Gemini\"},\"supported_voices\":[\"Kore\",\"Puck\"]},"
    "{\"id\":\"fish-audio/drama-3\",\"supported_voices\":[]},"
    "{\"id\":\"mistralai/voxtral-mini-tts-2603\",\"supported_voices\":[\"a\"]},"
    "{\"id\":\"sesame/csm-1b\",\"supported_voices\":[\"a\"]}"
    "]}";

void test_models(void)
{
    printf("t_models\n");
    mock_reset();
    host_settings_reset();
    host_set("ai_url", "https://relay.example/SECRET/api/v1");
    host_set("ai_key", "sk-or-v1-testkey");
    host_set("ai_model", "google/gemini-3.1-flash-lite");    // the listed current model (the default is 3.5)
    mock_push_json(200, PAGE1);
    list_ctx_t lc;
    memset(&lc, 0, sizeof(lc));
    CHECK(job_list(&lc) == ESP_OK);
    CHECK(strstr(g_reqs[0].url, "models?input_modalities=audio&output_modalities=text&sort=pricing-low-to-high"
                                "&max_price=1&limit=10&offset=0") != NULL);
    for (int i = 0; i < s_list_n; i++) {
        printf("    %-40s est=%.6f effort=%s mand=%d\n", s_list[i].id, s_list[i].est_turn_usd, s_list[i].min_effort,
               s_list[i].mandatory);
    }
    CHECK(s_list_n == 3);
    CHECK(!strcmp(s_list[0].id, "xiaomi/mimo-v2.6-flash") && s_list[0].min_effort[0] == 0);
    CHECK(!strcmp(s_list[1].id, "google/gemini-3.1-flash-lite") && !strcmp(s_list[1].min_effort, "minimal"));
    CHECK(!strcmp(s_list[2].id, "google/gemini-3.7-flash") && !strcmp(s_list[2].min_effort, "low"));
    CHECK(s_list[1].est_turn_usd > 0.00057f && s_list[1].est_turn_usd < 0.00059f);
    ai_model_t m;
    CHECK(ai_model_cache_get("google/gemini-3.1-flash-lite", &m) && m.audio && m.structured);

    mock_reset();
    mock_push_json(200, KEY);
    ai_key_info_t k;
    key_ctx_t kc = { .out = &k, .err = &(or_err_t){ 0 } };
    CHECK(job_key(&kc) == ESP_OK);
    CHECK(k.has_remaining && k.remaining == 17.5 && k.remaining_of == 50);
    char lines[512];
    ai_key_info_lines(&k, lines, sizeof(lines));
    printf("%s", lines);
    CHECK(strstr(lines, "Осталось $17.50 из $50.00 · сброс 1 ноября") != NULL);
    CHECK(strstr(lines, "Потрачено: сегодня $0.04 · месяц $1.20 · всего $25.50") != NULL);

    mock_reset();
    mock_push_json(200, "{\"data\":{\"label\":\"x\",\"limit\":null,\"limit_remaining\":null,\"usage\":0.3,"
                        "\"is_free_tier\":true}}");
    CHECK(job_key(&kc) == ESP_OK && !k.has_remaining);
    ai_key_info_lines(&k, lines, sizeof(lines));
    printf("%s", lines);
    CHECK(strstr(lines, "Остаток недоступен") && strstr(lines, "Бесплатный аккаунт"));

    mock_reset();
    mock_push_json(404, "{\"error\":{\"message\":\"Model not found: google/does-not-exist\",\"code\":404}}");
    or_err_t err;
    check_ctx_t cc = { .id = "google/does-not-exist", .out = &m, .err = &err };
    CHECK(job_check(&cc) == ESP_ERR_NOT_FOUND && !strcmp(err.id, "404") && g_nreq == 1);
    CHECK(strstr(g_reqs[0].url, "/api/v1/model/google/does-not-exist") != NULL);

    mock_reset();
    mock_push_json(200, TTS_PAGE);
    CHECK(job_tts_list(&lc) == ESP_OK && s_tts_n == 2);
    for (int i = 0; i < s_tts_n; i++) {
        printf("    %-40s $%.4f/1k voices=%u\n", s_tts[i].m.id, s_tts[i].m.usd_1k_ru, s_tts[i].m.n_voices);
    }
    char v[256];
    ai_client_tts_voices("microsoft/mai-voice-2.1-flash", "ru-RU", v, sizeof(v));
    printf("    ru voices: %s\n", v);
    CHECK(!strcmp(v, "ru-RU-Masha:MAI-Voice-2.1-Flash\nru-RU-Lev:MAI-Voice-2.1-Flash"));
    ai_client_tts_voices("google/gemini-3.8-flash-lite-tts", "ru-RU", v, sizeof(v));
    CHECK(!strcmp(v, "Kore\nPuck"));

    // background GET retries: 3 silent retries, then the error
    mock_reset();
    for (int i = 0; i < 4; i++) mock_push_json(502, "{\"error\":{\"code\":502,\"message\":\"x\"}}");
    int64_t t0 = g_now_us;
    char *body;
    int st;
    CHECK(ai_get_json("https://relay.example/SECRET/api/v1", "key", 8192, false, &body, &st, &err) == ESP_FAIL);
    CHECK(g_nreq == 4);
    printf("    4 attempts in %lld ms\n", (long long)((g_now_us - t0) / 1000));

    // the TLS pre-warm GETs never retry (a turn is queued behind them)
    mock_reset();
    for (int i = 0; i < 4; i++) mock_push_json(502, "{\"error\":{\"code\":502,\"message\":\"x\"}}");
    CHECK(ai_get_json("https://relay.example/SECRET/api/v1", "key", 8192, true, &body, &st, &err) == ESP_FAIL);
    CHECK(g_nreq == 1);
}
