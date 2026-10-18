// Pure-function tests: sanitiser, splitter, salvage, base64/WAV, URL helpers, error table.
#include "ai_priv.h"
#include "mocks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void show_split(const char *text)
{
    char *clean = malloc(strlen(text) + 1);
    tts_sanitize(text, clean, strlen(text) + 1);
    char **ch;
    int n = tts_split(clean, &ch);
    printf("  split (%d):\n", n);
    for (int i = 0; i < n; i++) {
        size_t l = utf8_cp_len(ch[i]);
        printf("    [%u] %s\n", (unsigned)l, ch[i]);
        CHECK(l >= 20 || n == 1);
        CHECK(l <= (i == 0 ? 230u : 430u));
    }
    tts_split_free(ch, n);
    free(clean);
}

static void t_text(void)
{
    printf("t_text\n");
    char out[512];
    tts_sanitize("**Привет**, мир! 😀 Смотри https://example.com/x?y=1 и `код` > цитата | таблица\n\n\nНовая   строка",
                 out, sizeof(out));
    printf("  sanitized: <%s>\n", out);
    CHECK(!strcmp(out, "Привет, мир! Смотри и код цитата таблица\nНовая строка"));

    show_split("Да. Это А. С. Пушкин написал в 1830 г. стихотворение. Цена 3.5 руб. за штуку, т.е. недорого! "
               "А дальше — ещё одно предложение про погоду и всё остальное?! Конец…");
    show_split("Короткий ответ.");
    {
        char **c2;
        int n2 = tts_split("Это стихотворение написал А. С. Пушкин в 1830 г. в Болдине, т.е. осенью. Второе предложение здесь.", &c2);
        CHECK(n2 == 2 && !strcmp(c2[0], "Это стихотворение написал А. С. Пушкин в 1830 г. в Болдине, т.е. осенью."));
        tts_split_free(c2, n2);
        n2 = tts_split("Число 3.14 важно для расчётов, и это первое предложение. Да.", &c2);
        CHECK(n2 == 1);
        tts_split_free(c2, n2);
    }
    show_split("Первое предложение довольно длинное, оно содержит много слов, чтобы превысить порог в сорок символов. "
               "Второе предложение тоже длинное и подробное, оно рассказывает о чём-то важном и интересном для "
               "пользователя. Третье предложение завершает мысль, добавляя немного деталей. Четвёртое — короткое. "
               "Пятое предложение снова длинное, оно добавлено специально, чтобы проверить упаковку фрагментов по "
               "четыреста символов и порог в сто пятьдесят символов для второго и последующих фрагментов текста.");
    // one sentence over 400 characters with commas
    char longs[2000] = "";
    for (int i = 0; i < 30; i++) strcat(longs, "очень длинная часть, ");
    strcat(longs, "конец.");
    show_split(longs);
    // no punctuation at all
    char nop[1200] = "";
    for (int i = 0; i < 60; i++) strcat(nop, "слово ");
    show_split(nop);
    show_split("Hello. Mr. Smith went to Washington. He said: \"Hi!\" Then he left. 你好。我很好！谢谢？");
    {
        // one Chinese sentence of 450 characters: cut after a full-width comma, never inside a run of hanzi
        char zh[2000] = "";
        for (int i = 0; i < 50; i++) strcat(zh, "今天天气很好我们");
        strcat(zh, "，");
        for (int i = 0; i < 6; i++) strcat(zh, "去公园散步吧好");
        strcat(zh, "。");
        char **c3;
        int n3 = tts_split(zh, &c3);
        size_t l0 = n3 > 0 ? utf8_cp_len(c3[0]) : 0;
        CHECK(n3 >= 2 && l0 == 200);        // first chunk <= 200: no comma in 150..200, hard cut at 200
        tts_split_free(c3, n3);
        char zh2[2000] = "";
        for (int i = 0; i < 20; i++) strcat(zh2, "今天天气很好我们");
        strcat(zh2, "，");
        for (int i = 0; i < 10; i++) strcat(zh2, "去公园散步吧好");
        strcat(zh2, "。");
        n3 = tts_split(zh2, &c3);
        CHECK(n3 == 2 && utf8_cp_len(c3[0]) == 161 && !strcmp(c3[0] + strlen(c3[0]) - 3, "，"));
        tts_split_free(c3, n3);
    }
    char **ch;
    CHECK(tts_split("", &ch) == 0);
    CHECK(tts_split("... !!! ???", &ch) == 0);
    tts_split_free(ch, 0);
}

static void t_salvage(void)
{
    printf("t_salvage\n");
    char *s = ai_json_salvage("{\"transcript\":\"a \\\"b\\\"\",\"reply\":\"Привет \\u0441\\u043c\\ud83d\\ude00 и", "reply");
    CHECK(s && !strcmp(s, "Привет см\xF0\x9F\x98\x80 и"));
    free(s);
    s = ai_json_salvage("{\"a\":{\"reply\":\"inner\"},\"reply\":\"outer\"}", "reply");
    CHECK(s && !strcmp(s, "outer"));
    free(s);
    CHECK(ai_json_salvage("{\"reply\":", "reply") == NULL);
    char *t = utf8_trunc_dup("абвгд", 3);
    CHECK(!strcmp(t, "абв…"));
    free(t);
}

static void t_b64_wav(void)
{
    printf("t_b64_wav\n");
    char out[16];
    CHECK(or_b64_encode((const uint8_t *)"Man", 3, out) == 4 && !memcmp(out, "TWFu", 4));
    CHECK(or_b64_encode((const uint8_t *)"Ma", 2, out) == 4 && !memcmp(out, "TWE=", 4));
    CHECK(or_b64_encode((const uint8_t *)"M", 1, out) == 4 && !memcmp(out, "TQ==", 4));
    CHECK(or_wav_b64_len(160000) == 426728);       // 10 s at 16 kHz (protocol §1.4)
    CHECK(or_wav_b64_len(160000 * 2 / 2) == 4 * ((44 + 320000 + 2) / 3));
    uint8_t h[44];
    or_wav_header(h, 16000, 160000);
    char b64[64];
    size_t n = or_b64_encode(h, 44, b64);
    b64[n] = 0;
    printf("  header b64: %s\n", b64);
    CHECK(!strncmp(b64, "UklGRiTiBABXQVZFZm10IBAAAAABAAEAgD4AAAB9AAACABAAZGF0YQDiBA", 58));
}

static void t_url(void)
{
    printf("t_url\n");
    char o[96];
    CHECK(or_url_origin("https://OpenRouter.ai/api/v1/chat", o, sizeof(o)) && !strcmp(o, "https://openrouter.ai"));
    CHECK(or_url_origin("https://h.example:8443/x", o, sizeof(o)) && !strcmp(o, "https://h.example:8443"));
    CHECK(!or_url_origin("ftp://x", o, sizeof(o)));
    CHECK(or_is_openrouter("https://openrouter.ai/api/v1"));
    CHECK(or_is_openrouter("https://server-ip.sslip.io/abcdef/api/v1/chat/completions"));
    CHECK(!or_is_openrouter("https://api.openai.com/v1"));
    CHECK(!or_is_openrouter("https://notopenrouter.ai/v1"));
    CHECK(or_is_openrouter("https://eu.openrouter.ai/v1"));
    char u[128];
    ai_join_url(u, sizeof(u), "https://a/b/api/v1/", "/chat/completions");
    CHECK(!strcmp(u, "https://a/b/api/v1/chat/completions"));
    // tts_key / stt_key fall back to ai_key only on the ai_url host
    char k[64];
    host_set("ai_url", "https://relay.example/SECRET/api/v1");
    host_set("ai_key", "sk-or-v1-test");
    CHECK(cfg_key("tts_key", "https://RELAY.example/other/v1", k, sizeof(k)) && !strcmp(k, "sk-or-v1-test"));
    CHECK(!cfg_key("tts_key", "https://piper.home.example/v1", k, sizeof(k)) && k[0] == 0);
    CHECK(!cfg_key("stt_key", "https://relay.example:8443/SECRET/api/v1", k, sizeof(k)) && k[0] == 0);
    host_set("tts_key", "own");
    CHECK(cfg_key("tts_key", "https://piper.home.example/v1", k, sizeof(k)) && !strcmp(k, "own"));
    host_settings_reset();
    CHECK(cfg_key("stt_key", "https://openrouter.ai/api/v1", k, sizeof(k)) && k[0] == 0);   // K0 case
}

static void t_classify(void)
{
    printf("t_classify\n");
    or_resp_t r;
    or_err_t e;
    memset(&r, 0, sizeof(r));
    r.phase = OR_PH_CONNECT;
    r.err = ESP_ERR_HTTP_CONNECT;
    r.tls_err = 0x8001;
    or_classify(OR_KIND_CHAT, &r, NULL, &e);
    CHECK(!strcmp(e.id, "N1") && e.max_retries == 2);
    r.tls_err = 0x801A;
    r.tls_flags = 8;
    or_classify(OR_KIND_CHAT, &r, NULL, &e);
    CHECK(!strcmp(e.id, "N3") && !e.retry);
    r.tls_flags = 0;
    or_classify(OR_KIND_CHAT, &r, NULL, &e);
    CHECK(!strcmp(e.id, "N4"));
    r.tls_err = 0x8004;
    or_classify(OR_KIND_CHAT, &r, NULL, &e);
    CHECK(!strcmp(e.id, "N2") && !strcmp(e.msg_retry_ru, "Нет связи с сервером. Повторяю…"));
    memset(&r, 0, sizeof(r));
    r.phase = OR_PH_FIRST_BYTE;
    r.err = ESP_ERR_HTTP_EAGAIN;
    or_classify(OR_KIND_GET, &r, NULL, &e);
    CHECK(!strcmp(e.id, "N6") && e.max_retries == 2);
    r.deadline = true;
    or_classify(OR_KIND_GET, &r, NULL, &e);
    CHECK(!strcmp(e.id, "N8"));

    memset(&r, 0, sizeof(r));
    r.status = 402;
    or_classify(OR_KIND_CHAT, &r, "{\"error\":{\"code\":402,\"message\":\"x\",\"metadata\":{\"limit_source\":"
                                  "\"openrouter_key_limit\"}}}", &e);
    CHECK(!strcmp(e.id, "402k"));
    r.status = 403;
    or_classify(OR_KIND_CHAT, &r, "{\"error\":{\"code\":403,\"message\":\"budget\",\"metadata\":{\"limit_source\":"
                                  "\"openrouter_guardrail_budget\",\"resets_at\":\"2026-10-20T00:00:00Z\"}}}", &e);
    CHECK(!strcmp(e.id, "403b") && !strcmp(e.msg_ru, "Исчерпан бюджет ключа до 20.10.2026."));
    r.status = 429;
    strcpy(r.retry_after, "30");
    or_classify(OR_KIND_CHAT, &r, "{\"error\":{\"code\":429,\"message\":\"Rate limit exceeded\"}}", &e);
    CHECK(!strcmp(e.id, "429") && !e.retry);
    strcpy(r.retry_after, "3");
    or_classify(OR_KIND_CHAT, &r, NULL, &e);
    CHECK(e.retry && e.retry_after_ms == 3000);
    // HTTP-date Retry-After 5 s ahead
    time_t now = time(NULL) + 5;
    struct tm g;
    gmtime_s(&g, &now);
    strftime(r.retry_after, sizeof(r.retry_after), "%a, %d %b %Y %H:%M:%S GMT", &g);
    char ra[40];
    strftime(ra, sizeof(ra), "%a, %d %b %Y %H:%M:%S GMT", &g);
    strlcpy(r.retry_after, ra, sizeof(r.retry_after));
    or_classify(OR_KIND_CHAT, &r, NULL, &e);
    printf("  http-date '%s' retry_after_ms=%d\n", r.retry_after, (int)e.retry_after_ms);
    CHECK(e.retry && e.retry_after_ms >= 3000 && e.retry_after_ms <= 6000);
    r.retry_after[0] = 0;
    r.status = 200;
    or_classify(OR_KIND_CHAT, &r, "{\"error\":{\"code\":529,\"message\":\"overloaded\"}}", &e);
    CHECK(!strcmp(e.id, "529") && e.max_retries == 2);
    r.status = 418;
    or_classify(OR_KIND_CHAT, &r, "teapot", &e);
    CHECK(!strcmp(e.id, "H418") && !strcmp(e.msg_ru, "Сервер вернул ошибку 418."));

    // retry budget: 429 without Retry-After -> 2 s then 5 s, then stop
    or_retry_t rs;
    or_retry_init(&rs, false);
    or_err_local(&e, "N2");
    uint32_t d;
    CHECK(or_retry_next(&rs, &e, 0, &d) && d >= 800 && d <= 1200);
    CHECK(or_retry_next(&rs, &e, 0, &d) && d >= 2400 && d <= 3600);
    CHECK(!or_retry_next(&rs, &e, 0, &d));
    or_retry_init(&rs, true);
    CHECK(or_retry_next(&rs, &e, 0, &d) && d == 2000);
    CHECK(or_retry_next(&rs, &e, 0, &d) && d == 4000);
    CHECK(or_retry_next(&rs, &e, 0, &d) && d == 8000);
    CHECK(!or_retry_next(&rs, &e, 0, &d));
}

static void t_prompts(void)
{
    printf("t_prompts\n");
    char dt[96];
    ai_datetime_ru(dt, sizeof(dt));
    printf("  datetime: %s\n", dt);
    CHECK(strstr(dt, "года, ") && strstr(dt, "(UTC+3)"));
    char *p = ai_prompt_translator("de", "en");
    CHECK(strstr(p, "between German (de) and English (en)") && strstr(p, "\"de\" or \"en\"") &&
          strstr(p, "translate into English; if \"en\", translate into German"));
    free(p);
    host_set("ai_sys", "Мой промпт.\nСейчас {DATETIME}.\nКонец.");
    p = ai_prompt_assistant(AI_WEB_AUTO);
    CHECK(strstr(p, "Мой промпт.") && !strstr(p, "{DATETIME}") && strstr(p, "Конец.") && !strstr(p, "needs_web"));
    free(p);
    host_set("ai_sys", "Мой промпт.\n{WEB}\nКонец.");
    p = ai_prompt_assistant(AI_WEB_ON);
    CHECK(strstr(p, "Мой промпт.\n- К запросу приложены результаты поиска") && strstr(p, "\nКонец."));
    free(p);
    host_settings_reset();
    // protocol §2.8: the {WEB} line of the built-in prompt per request shape
    char *off = ai_prompt_assistant(AI_WEB_OFF), *aut = ai_prompt_assistant(AI_WEB_AUTO);
    char *on = ai_prompt_assistant(AI_WEB_ON);
    CHECK(off && aut && on);
    CHECK(strstr(off, "нет доступа к интернету") && !strstr(off, "needs_web") && !strstr(off, "{WEB}"));
    CHECK(strstr(aut, "поставь needs_web = true") && !strstr(aut, "нет доступа") && !strstr(aut, "{WEB}"));
    CHECK(strstr(on, "результаты поиска в интернете") && !strstr(on, "needs_web") && !strstr(on, "нет доступа"));
    CHECK(strstr(aut, "Латиницу") && strstr(aut, "Если просьба неясна") && strstr(aut, "Сейчас "));
    free(off);
    free(aut);
    free(on);
}

static void t_links(void)
{
    printf("t_links\n");
    char h[48];
    ai_url_host("https://www.Gismeteo.ru/weather-moscow-4368/", h, sizeof(h));
    CHECK(!strcmp(h, "gismeteo.ru"));
    ai_url_host("http://rbc.ru:8080/x?y", h, sizeof(h));
    CHECK(!strcmp(h, "rbc.ru"));
    ai_url_host("ftp://x.ru/", h, sizeof(h));
    CHECK(h[0] == 0);
    ai_url_host("https://", h, sizeof(h));
    CHECK(h[0] == 0);
    ai_url_host("https://bad host/", h, sizeof(h));
    CHECK(h[0] == 0);
    ai_url_host("https://very-long-subdomain.of-a-very-long-domain.example.com/", h, 16);
    CHECK(h[0] == 0);
    static const struct {
        const char *in, *out, *host;
    } cases[] = {
        { "Плюс пять градусов ([gismeteo.ru](https://www.gismeteo.ru/weather-moscow/)).", "Плюс пять градусов.",
          "gismeteo.ru" },
        { "Смотрите [прогноз погоды](https://meteo.ru/x) на сегодня.", "Смотрите прогноз погоды на сегодня.",
          "meteo.ru" },
        { "Курс девяносто рублей, источник https://cbr.ru/currency.", "Курс девяносто рублей, источник.", "cbr.ru" },
        { "[a.ru](https://a.ru/1) [b.ru](https://b.ru/2)", "", "a.ru" },
        { "Ничего ( без ссылок ) тут нет .", "Ничего ( без ссылок ) тут нет .", "" },
        { "Итог: [rbc.ru](https://rbc.ru/a), [ria.ru](https://ria.ru/b).\nВторая строка.", "Итог.\nВторая строка.",
          "rbc.ru" },
        { "Погода хорошая, по данным [meteo.ru](https://meteo.ru), тепло.", "Погода хорошая, по данным, тепло.",
          "meteo.ru" },
        // numbered citations and footnote marks are not read aloud ("градусов один")
        { "Плюс пять градусов [1](https://a.ru/x).", "Плюс пять градусов.", "a.ru" },
        { "Курс девяносто рублей [1][2].", "Курс девяносто рублей.", "" },
        { "Новый выпуск [2, 3] вышел вчера.", "Новый выпуск вышел вчера.", "" },
        { "Выберите вариант [а] или [б].", "Выберите вариант [а] или [б].", "" },
        // a link target with its own parentheses ends at the matching one
        { "Смотрите [статью](https://ru.wikipedia.org/wiki/X_(y)) сегодня.", "Смотрите статью сегодня.",
          "ru.wikipedia.org" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buf[256];
        strlcpy(buf, cases[i].in, sizeof(buf));
        h[0] = 0;
        ai_strip_links(buf, h, sizeof(h));
        printf("  [%s] -> [%s] host=%s\n", cases[i].in, buf, h);
        CHECK(!strcmp(buf, cases[i].out) && !strcmp(h, cases[i].host));
    }
    char buf[64] = "Текст [x.ru](https://x.ru).";
    strlcpy(h, "first.ru", sizeof(h));
    ai_strip_links(buf, h, sizeof(h));         // a host from the annotations stays
    CHECK(!strcmp(buf, "Текст.") && !strcmp(h, "first.ru"));
    ai_strip_links(NULL, NULL, 0);
}

void test_misc(void)
{
    t_text();
    t_salvage();
    t_b64_wav();
    t_url();
    t_classify();
    t_prompts();
    t_links();
}
