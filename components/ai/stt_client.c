// Whisper fallback: POST {stt_url}/audio/transcriptions with the clip as base64 WAV (protocol §6).
#include "stt_client.h"
#include "ai_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "stt";

typedef struct {
    char *json;               // printed body, split at the placeholder
    const char *suffix;
    size_t pre_len, suf_len;
    const int16_t *pcm;
    size_t samples;
    uint32_t rate;
} stt_body_t;

static esp_err_t stt_write(or_conn_t *c, void *ctx)
{
    stt_body_t *b = ctx;
    esp_err_t e = or_write_all(c, b->json, b->pre_len);
    if (e == ESP_OK) {
        e = or_write_b64_wav(c, b->pcm, b->samples, b->rate);
    }
    if (e == ESP_OK) {
        e = or_write_all(c, b->suffix, b->suf_len);
    }
    return e;
}

esp_err_t stt_transcribe_ex(uint32_t turn, const int16_t *pcm, size_t samples, uint32_t rate, const char *lang,
                            char **text, double *cost, uint32_t *ms, or_err_t *err)
{
    *text = NULL;
    *cost = 0;
    *ms = 0;
    char base[136], url[200], key[208], model[100];
    cfg_url("stt_url", base, sizeof(base));
    ai_join_url(url, sizeof(url), base, "audio/transcriptions");
    bool need_key = cfg_key("stt_key", base, key, sizeof(key));
    cfg_str("stt_model", model, sizeof(model), "openai/whisper-large-v3-turbo");
    if (!key[0] && need_key) {
        or_err_local(err, "K0");
        return ESP_ERR_INVALID_STATE;
    }
    char ph[24];
    snprintf(ph, sizeof(ph), "B64_%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model);
    cJSON *ia = cJSON_AddObjectToObject(root, "input_audio");
    cJSON_AddStringToObject(ia, "data", ph);
    cJSON_AddStringToObject(ia, "format", "wav");
    if (lang && lang[0]) {
        cJSON_AddStringToObject(root, "language", lang);   // omitted in translator mode: auto-detect
    }
    cJSON_AddNumberToObject(root, "temperature", 0);
    stt_body_t b = { .json = cJSON_PrintUnformatted(root), .pcm = pcm, .samples = samples, .rate = rate };
    cJSON_Delete(root);
    char *p = b.json ? strstr(b.json, ph) : NULL;
    if (!p) {
        cJSON_free(b.json);
        or_err_local(err, "M0");
        return ESP_ERR_NO_MEM;
    }
    *p = 0;
    b.pre_len = (size_t)(p - b.json);
    b.suffix = p + strlen(ph);
    b.suf_len = strlen(b.suffix);

    or_req_t q = {
        .method = HTTP_METHOD_POST, .url = url, .bearer = key, .accept = "application/json", .to = OR_TO_STT,
        .body_len = (int64_t)(b.pre_len + or_wav_b64_len(samples) + b.suf_len),
        .write_body = stt_write, .body_ctx = &b, .abort = ai_abort_turn, .abort_ctx = &turn,
    };
    or_retry_t rs;
    or_retry_init(&rs, false);
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ESP_FAIL;
    for (;;) {
        or_resp_t r;
        char *body = NULL;
        esp_err_t e = or_exchange(&q, &r, &body, OR_CAP_STT, NULL);
        if (e == ESP_OK && r.status == 200 && body && !or_body_has_error(body)) {
            cJSON *o = cJSON_Parse(body);
            const char *t = ai_json_str(o, "text");
            if (t) {
                *text = ai_strdup(t);
                *cost = ai_json_num(cJSON_GetObjectItemCaseSensitive(o, "usage"), "cost", 0);
                ret = *text ? ESP_OK : ESP_ERR_NO_MEM;
                if (!*text) {
                    or_err_local(err, "M0");
                }
            } else {
                or_err_local(err, "P2");
            }
            cJSON_Delete(o);
            free(body);
            if (ret == ESP_OK || strcmp(err->id, "P2")) {
                break;
            }
        } else {
            or_classify(OR_KIND_STT, &r, body, err);
            free(body);
        }
        uint32_t delay;
        if (!strcmp(err->id, "AB") || !or_retry_next(&rs, err, 0, &delay)) {
            break;
        }
        ESP_LOGW(TAG, "%s (%s), retry in %" PRIu32 " ms", err->id, err->message, delay);
        if (!ai_sleep_ms(delay, ai_abort_turn, &turn)) {
            or_err_local(err, "AB");
            break;
        }
    }
    cJSON_free(b.json);
    memset(key, 0, sizeof(key));
    *ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%" PRIu32 " ms, %u chars, $%.6f", *ms, (unsigned)strlen(*text), *cost);
    } else if (!strcmp(err->id, "AB")) {
        ret = ESP_ERR_NOT_FINISHED;
    } else {
        ESP_LOGW(TAG, "failed: %s %s (%s)", err->id, err->msg_ru, err->message);
    }
    return ret;
}

esp_err_t stt_client_transcribe(uint32_t turn, const audio_clip_t *clip, const char *lang, char *text,
                                size_t text_len)
{
    if (!clip || !clip->pcm || !text || !text_len) {
        return ESP_ERR_INVALID_ARG;
    }
    char *t = NULL;
    double cost = 0;
    uint32_t ms = 0;
    or_err_t err;
    esp_err_t e = stt_transcribe_ex(turn, clip->pcm, clip->samples, clip->rate ? clip->rate : 16000, lang, &t, &cost,
                                    &ms, &err);
    if (e == ESP_OK) {
        strlcpy(text, t, text_len);
        ai_client_add_cost(cost);
    } else {
        text[0] = 0;
    }
    free(t);
    return e;
}
