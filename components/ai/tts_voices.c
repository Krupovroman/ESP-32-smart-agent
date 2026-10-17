// Voice routing (protocol §5.5, §5.6), sample-rate table (§5.2) and TTS price estimates.
#include "ai_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "tts";

#define MAI_FLASH   "microsoft/mai-voice-2.1-flash"
#define MAI_SUFFIX  ":MAI-Voice-2.1-Flash"
#define GEMINI_TTS  "google/gemini-3.8-flash-lite-tts"
#define GROK_TTS    "x-ai/grok-voice-tts-1.0"
#define ELEVEN_TTS  "elevenlabs/eleven-flash-v2.5"
#define FAIL_TTL_MS (3600LL * 1000)          // a failed route is skipped for 1 h
#define FAIL_N      8
#define NOSPEED_N   4

// Full ids (constant strings: no shared buffer between the tts task and the console).
static const struct {
    const char *lang, *voice;
} MAI_VOICES[] = {
    { "ru", "ru-RU-Masha" MAI_SUFFIX }, { "en", "en-US-Harper" MAI_SUFFIX }, { "de", "de-DE-Mia" MAI_SUFFIX },
    { "fr", "fr-FR-Soleil" MAI_SUFFIX }, { "es", "es-ES-Marta" MAI_SUFFIX }, { "it", "it-IT-Rosa" MAI_SUFFIX },
    { "pt", "pt-BR-Luana" MAI_SUFFIX }, { "pl", "pl-PL-Harper" MAI_SUFFIX }, { "tr", "tr-TR-Elif" MAI_SUFFIX },
    { "zh", "zh-CN-Mei" MAI_SUFFIX }, { "ko", "ko-KR-Haena" MAI_SUFFIX },
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// Route failures and models without speed control: 1.3 KB in PSRAM, created on the first entry.
typedef struct {
    struct {
        char model[97];
        char lang[8];         // "*" = every language (e.g. a region block)
        int64_t until_ms;
    } fail[FAIL_N];
    int fail_next;
    char nospeed[NOSPEED_N][97];
    int nospeed_next;
} route_mem_t;
static route_mem_t *s_mem;

static route_mem_t *mem_get(bool create)
{
    route_mem_t *m = __atomic_load_n(&s_mem, __ATOMIC_ACQUIRE);
    if (!m && create) {
        route_mem_t *n = ai_calloc(1, sizeof(route_mem_t));
        route_mem_t *expect = NULL;
        if (n && __atomic_compare_exchange_n(&s_mem, &expect, n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            m = n;
        } else {
            free(n);
            m = expect;
        }
    }
    return m;
}

static const char *mai_voice(const char *lang)
{
    for (size_t i = 0; i < sizeof(MAI_VOICES) / sizeof(MAI_VOICES[0]); i++) {
        if (!strcmp(MAI_VOICES[i].lang, lang)) {
            return MAI_VOICES[i].voice;
        }
    }
    return NULL;
}

static bool is_gemini_tts(const char *model)
{
    return !strncmp(model, "google/gemini-", 14) && strstr(model, "-tts");
}

const char *tts_model_default_voice(const char *model, const char *lang)
{
    if (!strcmp(model, MAI_FLASH)) {
        const char *v = mai_voice(lang);
        return v ? v : MAI_VOICES[0].voice;
    }
    if (is_gemini_tts(model)) {
        return "Kore";
    }
    if (!strncmp(model, "x-ai/grok-voice", 15)) {
        return !strcmp(lang, "ar") ? "ara" : "eve";
    }
    if (!strncmp(model, "elevenlabs/", 11)) {
        return "sarah";
    }
    return NULL;
}

// Languages a model is known not to speak (protocol §5.5).
static bool can_speak(const char *model, const char *lang)
{
    if (!strcmp(model, MAI_FLASH)) {
        return mai_voice(lang) != NULL;
    }
    if (!strncmp(model, "x-ai/grok-voice", 15)) {
        return strcmp(lang, "uk") != 0;
    }
    return true;
}

// "ru-RU-Masha:…" -> "ru"; voices without a locale prefix speak any language ("").
static void voice_lang(const char *voice, char out[4])
{
    out[0] = 0;
    if (strlen(voice) > 6 && voice[0] >= 'a' && voice[0] <= 'z' && voice[1] >= 'a' && voice[1] <= 'z' &&
        voice[2] == '-' && voice[3] >= 'A' && voice[3] <= 'Z' && voice[4] >= 'A' && voice[4] <= 'Z' &&
        voice[5] == '-') {
        out[0] = voice[0];
        out[1] = voice[1];
        out[2] = 0;
    }
}

bool tts_route_failed(const char *model, const char *lang)
{
    route_mem_t *m = mem_get(false);
    if (!m) {
        return false;
    }
    bool f = false;
    int64_t now = ai_now_ms();
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < FAIL_N; i++) {
        if (m->fail[i].until_ms > now && !strcmp(m->fail[i].model, model) &&
            (!strcmp(m->fail[i].lang, "*") || !strcmp(m->fail[i].lang, lang))) {
            f = true;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return f;
}

void tts_route_fail(const char *model, const char *lang, bool all_langs)
{
    ESP_LOGW(TAG, "route %s (%s) failed, skipped for 1 h", model, all_langs ? "*" : lang);
    route_mem_t *m = mem_get(true);
    if (!m) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    int i = m->fail_next;
    m->fail_next = (m->fail_next + 1) % FAIL_N;
    strlcpy(m->fail[i].model, model, sizeof(m->fail[i].model));
    strlcpy(m->fail[i].lang, all_langs ? "*" : lang, sizeof(m->fail[i].lang));
    m->fail[i].until_ms = ai_now_ms() + FAIL_TTL_MS;
    taskEXIT_CRITICAL(&s_mux);
}

bool tts_speed_unsupported(const char *model)
{
    route_mem_t *m = mem_get(false);
    if (!m) {
        return false;
    }
    bool f = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NOSPEED_N; i++) {
        if (!strcmp(m->nospeed[i], model)) {
            f = true;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return f;
}

void tts_speed_mark_unsupported(const char *model)
{
    route_mem_t *m = mem_get(true);
    if (!m) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    strlcpy(m->nospeed[m->nospeed_next], model, sizeof(m->nospeed[0]));
    m->nospeed_next = (m->nospeed_next + 1) % NOSPEED_N;
    taskEXIT_CRITICAL(&s_mux);
}

static int add_route(tts_route_t *out, int n, int max, const char *lang, const char *model, const char *voice)
{
    if (n >= max || !model || !model[0] || !voice || !voice[0] || !can_speak(model, lang) ||
        tts_route_failed(model, lang)) {
        return n;
    }
    for (int i = 0; i < n; i++) {
        if (!strcmp(out[i].model, model)) {
            return n;           // one entry per model: a voice problem is fixed inside the route
        }
    }
    strlcpy(out[n].model, model, sizeof(out[n].model));
    strlcpy(out[n].voice, voice, sizeof(out[n].voice));
    return n + 1;
}

int tts_routes(const char *lang, tts_route_t *out, int max)
{
    int n = 0;
    // 1. Per-language override "model|voice" (setting vo_<lang>, when the table has it).
    char key[16], ov[200];
    snprintf(key, sizeof(key), "vo_%s", lang);
    cfg_str(key, ov, sizeof(ov), "");
    char *bar = strchr(ov, '|');
    if (bar) {
        *bar = 0;
        n = add_route(out, n, max, lang, ov, bar + 1);
    }
    // 2. The configured model and voice, when the voice fits the language.
    char model[100], voice[100], vl[4];
    cfg_str("tts_model", model, sizeof(model), MAI_FLASH);
    cfg_str("tts_voice", voice, sizeof(voice), "");
    voice_lang(voice, vl);
    if (voice[0] && (!vl[0] || !strcmp(vl, lang))) {
        n = add_route(out, n, max, lang, model, voice);
    } else {
        n = add_route(out, n, max, lang, model, tts_model_default_voice(model, lang));
    }
    // 3. Protocol §5.5 table, then 4. the §5.6 order: Gemini, Grok, ElevenLabs (uk: ElevenLabs first).
    if (!strcmp(lang, "uk")) {
        n = add_route(out, n, max, lang, ELEVEN_TTS, "sarah");
        n = add_route(out, n, max, lang, GEMINI_TTS, "Kore");
    } else {
        if (mai_voice(lang)) {
            n = add_route(out, n, max, lang, MAI_FLASH, tts_model_default_voice(MAI_FLASH, lang));
        }
        n = add_route(out, n, max, lang, GEMINI_TTS, "Kore");
        n = add_route(out, n, max, lang, GROK_TTS, !strcmp(lang, "ar") ? "ara" : "eve");
        n = add_route(out, n, max, lang, ELEVEN_TTS, "sarah");
    }
    return n;
}

esp_err_t tts_route(const char *lang, char *model, size_t mlen, char *voice, size_t vlen, int attempt)
{
    tts_route_t r[6];
    int n = tts_routes(lang, r, 6);
    if (attempt < 0 || attempt >= n) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(model, r[attempt].model, mlen);
    strlcpy(voice, r[attempt].voice, vlen);
    return ESP_OK;
}

double tts_price_per_char(const char *model)
{
    // USD per character (protocol §7.2 live prices); Gemini is token-priced, about 0.011 $/1000 chars.
    if (!strcmp(model, MAI_FLASH) || !strncmp(model, "x-ai/grok-voice", 15)) {
        return 15e-6;
    }
    if (!strncmp(model, "microsoft/mai-voice-2.1", 23)) {
        return 22e-6;
    }
    if (!strcmp(model, GEMINI_TTS)) {
        return 11e-6;
    }
    if (is_gemini_tts(model)) {
        return 16e-6;
    }
    if (!strcmp(model, ELEVEN_TTS) || !strncmp(model, "elevenlabs/eleven-v4-turbo", 26)) {
        return 20e-6;
    }
    if (!strncmp(model, "elevenlabs/", 11)) {
        return 40e-6;
    }
    return 15e-6;
}

void tts_parse_ct(const char *ct, char *media, size_t mlen, uint32_t *rate, uint8_t *channels)
{
    *rate = 0;
    *channels = 0;
    char buf[64];
    strlcpy(buf, ct ? ct : "", sizeof(buf));
    for (char *p = buf; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
    char *save = NULL;
    char *tok = strtok_r(buf, ";", &save);
    strlcpy(media, tok ? tok : "", mlen);
    // trim the media type
    size_t ml = strlen(media);
    while (ml && media[ml - 1] == ' ') {
        media[--ml] = 0;
    }
    while (tok && (tok = strtok_r(NULL, ";", &save))) {
        while (*tok == ' ') {
            tok++;
        }
        char *eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        long v = strtol(eq + 1, NULL, 10);
        if (!strcmp(tok, "rate") || !strcmp(tok, "sample_rate") || !strcmp(tok, "samplerate")) {
            *rate = v > 0 ? (uint32_t)v : 0;
        } else if (!strcmp(tok, "channels")) {
            *channels = v > 0 && v <= 2 ? (uint8_t)v : 0;
        }
    }
}

uint32_t tts_rate_for(const char *model, const char *content_type, uint8_t *channels)
{
    char media[32];
    uint32_t rate;
    uint8_t ch;
    tts_parse_ct(content_type, media, sizeof(media), &rate, &ch);
    *channels = ch ? ch : 1;
    uint32_t forced = (uint32_t)cfg_int("tts_rate", 0);
    if (forced) {
        return forced;
    }
    if (rate) {
        return rate;
    }
    // No rate= parameter: per-model table of protocol §5.2 (no blind default).
    if (!strncmp(model, "heygen/", 7)) {
        return 44100;
    }
    if (!strncmp(model, "microsoft/mai-", 14) || is_gemini_tts(model) || !strncmp(model, "x-ai/grok-voice", 15) ||
        !strncmp(model, "elevenlabs/", 11) || !strncmp(model, "hexgrad/kokoro", 14)) {
        return 24000;
    }
    ESP_LOGW(TAG, "no rate for %s, assuming 24000", model);
    return 24000;
}
