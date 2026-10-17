// Model pickers (protocol §7), the single-model check and the key check (protocol §8).
#include "ai_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ai_models";

#define PAGE_CHAT       10
#define PAGE_TTS        12
#define MAX_PAGES       8
#define MAX_MODELS      (PAGE_CHAT * MAX_PAGES)
#define MAX_TTS         (PAGE_TTS * MAX_PAGES)
#define LIST_MAX_AGE_MS 120000              // Cache-Control: max-age=120
#define MCACHE_N        4

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ai_model_t s_mcache[MCACHE_N];
static int s_mcache_n, s_mcache_next;

static ai_model_t *s_list;
static int s_list_n;
static int64_t s_list_at;

typedef struct {
    ai_tts_model_t m;
    char *voices;                           // '\n'-separated, PSRAM
} tts_entry_t;
static tts_entry_t *s_tts;
static int s_tts_n;
static int64_t s_tts_at;

// ---- single-model cache -----------------------------------------------------------------------

bool ai_model_cache_get(const char *id, ai_model_t *out)
{
    bool found = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_mcache_n; i++) {
        if (!strcmp(s_mcache[i].id, id)) {
            *out = s_mcache[i];
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return found;
}

void ai_model_cache_put(const ai_model_t *m)
{
    taskENTER_CRITICAL(&s_mux);
    int slot = -1;
    for (int i = 0; i < s_mcache_n; i++) {
        if (!strcmp(s_mcache[i].id, m->id)) {
            slot = i;
        }
    }
    if (slot < 0) {
        if (s_mcache_n < MCACHE_N) {
            slot = s_mcache_n++;
        } else {
            slot = s_mcache_next;
            s_mcache_next = (s_mcache_next + 1) % MCACHE_N;
        }
    }
    s_mcache[slot] = *m;
    taskEXIT_CRITICAL(&s_mux);
}

// ---- JSON entry -> ai_model_t -----------------------------------------------------------------

static bool array_has(const cJSON *arr, const char *s)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it) && !strcmp(it->valuestring, s)) {
            return true;
        }
    }
    return false;
}

void ai_model_from_json(const cJSON *m, ai_model_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *id = ai_json_str(m, "id");
    const char *name = ai_json_str(m, "name");
    strlcpy(out->id, id ? id : "", sizeof(out->id));
    strlcpy(out->name, name ? name : out->id, sizeof(out->name));
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(m, "pricing");
    double p_in = ai_json_num(pr, "prompt", 0), p_out = ai_json_num(pr, "completion", 0);
    double p_audio = ai_json_num(pr, "audio", 0);
    out->usd_in_m = (float)(p_in * 1e6);
    out->usd_out_m = (float)(p_out * 1e6);
    out->usd_audio_m = (float)(p_audio * 1e6);
    const cJSON *sp = cJSON_GetObjectItemCaseSensitive(m, "supported_parameters");
    out->structured = array_has(sp, "response_format") && array_has(sp, "structured_outputs");
    const cJSON *arch = cJSON_GetObjectItemCaseSensitive(m, "architecture");
    out->audio = array_has(cJSON_GetObjectItemCaseSensitive(arch, "input_modalities"), "audio");
    // Effort rule of protocol §2.1: "minimal" when listed, else the lowest listed, else omit.
    const cJSON *rs = cJSON_GetObjectItemCaseSensitive(m, "reasoning");
    bool has_minimal = false;
    if (cJSON_IsObject(rs)) {
        out->mandatory = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rs, "mandatory"));
        const cJSON *ef = cJSON_GetObjectItemCaseSensitive(rs, "supported_efforts");
        has_minimal = array_has(ef, "minimal");
        if (has_minimal) {
            strlcpy(out->min_effort, "minimal", sizeof(out->min_effort));
        } else {
            const cJSON *it;
            cJSON_ArrayForEach(it, ef) {
                if (cJSON_IsString(it) && strcmp(it->valuestring, "none")) {
                    strlcpy(out->min_effort, it->valuestring, sizeof(out->min_effort));
                }
            }
        }
    }
    const char *exp = ai_json_str(m, "expiration_date");
    strlcpy(out->expires, exp ? exp : "", sizeof(out->expires));
    // Reference turn: 10 s audio = 320 tokens, 600 text tokens in, 180 out (+200 forced reasoning).
    double audio = p_audio > 0 ? p_audio : p_in;
    double out_tok = 180 + (out->mandatory && !has_minimal ? 200 : 0);
    out->est_turn_usd = (float)(320 * audio + 600 * p_in + out_tok * p_out);
}

// ---- GET helper -------------------------------------------------------------------------------

esp_err_t ai_get_json(const char *base_url, const char *path, size_t cap, char **body, int *status, or_err_t *err)
{
    *body = NULL;
    *status = 0;
    char url[320], key[208];
    ai_join_url(url, sizeof(url), base_url, path);
    cfg_str("ai_key", key, sizeof(key), "");
    or_err_t e_local;
    or_err_t *e = err ? err : &e_local;
    if (!ai_net_up()) {
        or_err_local(e, "N0");
        return ESP_ERR_INVALID_STATE;
    }
    or_req_t q = { .method = HTTP_METHOD_GET, .url = url, .bearer = key, .accept = "application/json",
                   .to = OR_TO_GET };
    or_retry_t rs;
    or_retry_init(&rs, true);
    esp_err_t ret = ESP_FAIL;
    for (;;) {
        or_resp_t r;
        char *b = NULL;
        esp_err_t x = or_exchange(&q, &r, &b, cap, NULL);
        *status = r.status;
        if (x == ESP_OK && r.status == 200 && b && !or_body_has_error(b)) {
            *body = b;
            ret = ESP_OK;
            break;
        }
        or_classify(OR_KIND_GET, &r, b, e);
        free(b);
        uint32_t delay;
        if (!or_retry_next(&rs, e, 0, &delay)) {
            ret = e->http == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            break;
        }
        ESP_LOGI(TAG, "GET %s: %s, retry in %" PRIu32 " ms", path, e->id, delay);
        ai_sleep_ms(delay, NULL, NULL);
    }
    memset(key, 0, sizeof(key));
    return ret;
}

static void base_url(char *out, size_t len)
{
    cfg_str("ai_url", out, len, "https://openrouter.ai/api/v1");
}

// ---- chat picker (protocol §7.1) --------------------------------------------------------------

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static int cmp_est(const void *a, const void *b)
{
    float x = ((const ai_model_t *)a)->est_turn_usd, y = ((const ai_model_t *)b)->est_turn_usd;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct {
    or_err_t err;
} list_ctx_t;

static esp_err_t job_list(void *ctx)
{
    list_ctx_t *lc = ctx;
    char base[136], cur[100];
    base_url(base, sizeof(base));
    cfg_str("ai_model", cur, sizeof(cur), "google/gemini-3.1-flash-lite");
    bool allow_free = (cfg_int("dev_flags", 0) & 8) != 0;
    ai_model_t *list = ai_calloc(MAX_MODELS, sizeof(ai_model_t));
    if (!list) {
        return ESP_ERR_NO_MEM;
    }
    int n = 0, total = MAX_MODELS;
    esp_err_t ret = ESP_OK;
    for (int page = 0; page < MAX_PAGES && page * PAGE_CHAT < total; page++) {
        char path[200];
        // No supported_parameters filter: it returns flex-tier prices (protocol §7.1 gotcha).
        snprintf(path, sizeof(path),
                 "models?input_modalities=audio&output_modalities=text&sort=pricing-low-to-high&max_price=1"
                 "&limit=%d&offset=%d", PAGE_CHAT, page * PAGE_CHAT);
        char *body;
        int status;
        ret = ai_get_json(base, path, OR_CAP_MODELS, &body, &status, &lc->err);
        if (ret != ESP_OK) {
            break;
        }
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) {
            ret = ESP_FAIL;
            break;
        }
        const cJSON *tc = cJSON_GetObjectItemCaseSensitive(root, "total_count");
        if (cJSON_IsNumber(tc)) {
            total = (int)tc->valuedouble;
        }
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        int got = cJSON_GetArraySize(data);
        const cJSON *e;
        cJSON_ArrayForEach(e, data) {
            if (n >= MAX_MODELS) {
                break;
            }
            ai_model_t m;
            ai_model_from_json(e, &m);
            bool selected = !strcmp(m.id, cur);
            if (!m.id[0] || ends_with(m.id, ":batch") || (ends_with(m.id, ":free") && !allow_free) ||
                m.id[0] == '~' || !strncmp(m.id, "openrouter/", 11) || m.usd_in_m < 0 || m.usd_out_m < 0 ||
                (m.expires[0] && !selected) || !m.structured || !m.audio) {
                continue;
            }
            list[n++] = m;
        }
        cJSON_Delete(root);
        if (got < PAGE_CHAT) {
            break;
        }
    }
    if (ret != ESP_OK && n == 0) {
        ESP_LOGW(TAG, "chat models: %s %s (%s)", lc->err.id, lc->err.msg_ru, lc->err.message);
        free(list);
        return ret;
    }
    // Rule 7: drop "-preview" when the same id without it is listed.
    int k = 0;
    for (int i = 0; i < n; i++) {
        bool drop = false;
        if (ends_with(list[i].id, "-preview")) {
            size_t bl = strlen(list[i].id) - 8;
            for (int j = 0; j < n && !drop; j++) {
                drop = j != i && strlen(list[j].id) == bl && !strncmp(list[j].id, list[i].id, bl);
            }
        }
        if (!drop) {
            list[k++] = list[i];
        }
    }
    n = k;
    qsort(list, (size_t)n, sizeof(ai_model_t), cmp_est);
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i].id, cur)) {
            ai_model_cache_put(&list[i]);
        }
    }
    taskENTER_CRITICAL(&s_mux);
    ai_model_t *old = s_list;
    s_list = list;
    s_list_n = n;
    s_list_at = ai_now_ms();
    taskEXIT_CRITICAL(&s_mux);
    free(old);
    ESP_LOGI(TAG, "%d chat models after filtering", n);
    return ESP_OK;
}

esp_err_t ai_client_list_models(int page, ai_model_t *out, int max, int *n, int *total)
{
    *n = 0;
    *total = 0;
    if (page == 0 && (!s_list || ai_now_ms() - s_list_at > LIST_MAX_AGE_MS)) {
        list_ctx_t lc;
        memset(&lc, 0, sizeof(lc));
        esp_err_t e = ai_run_sync(job_list, &lc);
        if (e != ESP_OK && !s_list) {
            return e;
        }
    }
    taskENTER_CRITICAL(&s_mux);
    int start = page * max;
    for (int i = start; s_list && i < s_list_n && *n < max; i++) {
        out[(*n)++] = s_list[i];
    }
    *total = s_list_n;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

// ---- TTS picker (protocol §7.2) ---------------------------------------------------------------

static float tts_price_1k(const char *tokenizer, double prompt, double completion)
{
    if (tokenizer && !strcmp(tokenizer, "Gemini")) {
        return (float)(1000.0 / 3.5 * prompt + (1000.0 / 14.0) * 25.0 * completion);
    }
    if (prompt == 0 && completion > 0) {
        return (float)((1000.0 / 14.0) * completion);      // priced per second of audio
    }
    return (float)(1000.0 * prompt);                         // priced per character
}

static bool tts_excluded(const char *id)
{
    static const char *const NO[] = { "canopylabs/orpheus-3b-0.1-ft", "sesame/csm-1b", "deepgram/flux-tts",
                                      "elevenlabs/eleven-turbo-v2", "elevenlabs/eleven-flash-v2" };
    if (!strncmp(id, "mistralai/voxtral-mini-tts", 26)) {
        return true;                                          // MP3 only (protocol §7.2 rule 4)
    }
    for (size_t i = 0; i < sizeof(NO) / sizeof(NO[0]); i++) {
        if (!strcmp(id, NO[i])) {
            return true;
        }
    }
    return false;
}

static void tts_list_free(tts_entry_t *l, int n)
{
    for (int i = 0; l && i < n; i++) {
        free(l[i].voices);
    }
    free(l);
}

static esp_err_t job_tts_list(void *ctx)
{
    list_ctx_t *lc = ctx;
    char base[136];
    base_url(base, sizeof(base));
    bool allow_free = (cfg_int("dev_flags", 0) & 8) != 0;
    tts_entry_t *list = ai_calloc(MAX_TTS, sizeof(tts_entry_t));
    if (!list) {
        return ESP_ERR_NO_MEM;
    }
    int n = 0;
    esp_err_t ret = ESP_OK;
    for (int page = 0; page < MAX_PAGES; page++) {
        char path[96];
        snprintf(path, sizeof(path), "models?output_modalities=speech&limit=%d&offset=%d", PAGE_TTS, page * PAGE_TTS);
        char *body;
        int status;
        ret = ai_get_json(base, path, OR_CAP_MODELS, &body, &status, &lc->err);
        if (ret != ESP_OK) {
            break;
        }
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) {
            ret = ESP_FAIL;
            break;
        }
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        int got = cJSON_GetArraySize(data);
        const cJSON *e;
        cJSON_ArrayForEach(e, data) {
            if (n >= MAX_TTS) {
                break;
            }
            const char *id = ai_json_str(e, "id");
            const cJSON *voices = cJSON_GetObjectItemCaseSensitive(e, "supported_voices");
            int nv = cJSON_GetArraySize(voices);
            if (!id || nv <= 0 || (ends_with(id, ":free") && !allow_free) || ai_json_str(e, "expiration_date") ||
                tts_excluded(id)) {
                continue;
            }
            tts_entry_t *t = &list[n];
            strlcpy(t->m.id, id, sizeof(t->m.id));
            strlcpy(t->m.name, ai_json_str(e, "name") ? ai_json_str(e, "name") : id, sizeof(t->m.name));
            const cJSON *pr = cJSON_GetObjectItemCaseSensitive(e, "pricing");
            const char *tok = ai_json_str(cJSON_GetObjectItemCaseSensitive(e, "architecture"), "tokenizer");
            t->m.usd_1k_ru = tts_price_1k(tok, ai_json_num(pr, "prompt", 0), ai_json_num(pr, "completion", 0));
            size_t need = 1;
            const cJSON *v;
            cJSON_ArrayForEach(v, voices) {
                if (cJSON_IsString(v)) {
                    need += strlen(v->valuestring) + 1;
                }
            }
            t->voices = ai_malloc(need);
            if (!t->voices) {
                continue;
            }
            size_t o = 0;
            cJSON_ArrayForEach(v, voices) {
                if (cJSON_IsString(v)) {
                    size_t l = strlen(v->valuestring);
                    memcpy(t->voices + o, v->valuestring, l);
                    o += l;
                    t->voices[o++] = '\n';
                    if (!t->m.first_voice[0]) {
                        strlcpy(t->m.first_voice, v->valuestring, sizeof(t->m.first_voice));
                    }
                    t->m.n_voices++;
                }
            }
            t->voices[o ? o - 1 : 0] = 0;
            n++;
        }
        cJSON_Delete(root);
        if (got < PAGE_TTS) {
            break;
        }
    }
    if (ret != ESP_OK && n == 0) {
        ESP_LOGW(TAG, "tts models: %s %s (%s)", lc->err.id, lc->err.msg_ru, lc->err.message);
        tts_list_free(list, n);
        return ret;
    }
    // cheapest first
    for (int i = 1; i < n; i++) {
        tts_entry_t x = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].m.usd_1k_ru > x.m.usd_1k_ru) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = x;
    }
    taskENTER_CRITICAL(&s_mux);
    tts_entry_t *old = s_tts;
    int old_n = s_tts_n;
    s_tts = list;
    s_tts_n = n;
    s_tts_at = ai_now_ms();
    taskEXIT_CRITICAL(&s_mux);
    tts_list_free(old, old_n);
    ESP_LOGI(TAG, "%d TTS models after filtering", n);
    return ESP_OK;
}

esp_err_t ai_client_list_tts_models(int page, ai_tts_model_t *out, int max, int *n, int *total)
{
    *n = 0;
    *total = 0;
    if (page == 0 && (!s_tts || ai_now_ms() - s_tts_at > LIST_MAX_AGE_MS)) {
        list_ctx_t lc;
        memset(&lc, 0, sizeof(lc));
        esp_err_t e = ai_run_sync(job_tts_list, &lc);
        if (e != ESP_OK && !s_tts) {
            return e;
        }
    }
    taskENTER_CRITICAL(&s_mux);
    for (int i = page * max; s_tts && i < s_tts_n && *n < max; i++) {
        out[(*n)++] = s_tts[i].m;
    }
    *total = s_tts_n;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

size_t ai_client_tts_voices(const char *model, const char *locale, char *out, size_t len)
{
    if (!len) {
        return 0;
    }
    out[0] = 0;
    size_t o = 0;
    // The list is replaced only by job_tts_list in the ai task; copy under the lock in small steps.
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; s_tts && i < s_tts_n; i++) {
        if (strcmp(s_tts[i].m.id, model) || !s_tts[i].voices) {
            continue;
        }
        const char *p = s_tts[i].voices;
        while (*p) {
            size_t l = strcspn(p, "\n");
            // microsoft/* ids are "<locale>-<Name>:<suffix>"; other models get a flat list
            size_t ll = locale ? strlen(locale) : 0;
            bool keep = !ll || strncmp(model, "microsoft/", 10) != 0 ||
                        (l > ll && !strncmp(p, locale, ll) && p[ll] == '-');
            if (keep && o + l + 2 < len) {
                memcpy(out + o, p, l);
                o += l;
                out[o++] = '\n';
            }
            p += l;
            if (*p == '\n') {
                p++;
            }
        }
        break;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (o) {
        o--;
    }
    out[o] = 0;
    return o;
}

// ---- single model check -----------------------------------------------------------------------

typedef struct {
    const char *id;
    ai_model_t *out;
    or_err_t *err;
} check_ctx_t;

static esp_err_t job_check(void *ctx)
{
    check_ctx_t *cc = ctx;
    char base[136], path[120];
    base_url(base, sizeof(base));
    snprintf(path, sizeof(path), "model/%s", cc->id);     // singular "model" (protocol §1.1)
    char *body;
    int status;
    esp_err_t e = ai_get_json(base, path, OR_CAP_KEY, &body, &status, cc->err);
    if (e != ESP_OK) {
        return e;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    ai_model_from_json(data, cc->out);
    cJSON_Delete(root);
    ai_model_cache_put(cc->out);
    return ESP_OK;
}

esp_err_t ai_client_check_model(const char *id, ai_model_t *out, or_err_t *err)
{
    char cur[100];
    if (!id || !id[0]) {
        cfg_str("ai_model", cur, sizeof(cur), "google/gemini-3.1-flash-lite");
        id = cur;
    }
    or_err_t local;
    check_ctx_t cc = { .id = id, .out = out, .err = err ? err : &local };
    memset(cc.err, 0, sizeof(*cc.err));
    return ai_run_sync(job_check, &cc);
}

// ---- key check (protocol §8) ------------------------------------------------------------------

typedef struct {
    ai_key_info_t *out;
    or_err_t *err;
} key_ctx_t;

static bool num_or_null(const cJSON *o, const char *k, double *v)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(x)) {
        *v = x->valuedouble;
        return true;
    }
    return false;
}

static esp_err_t job_key(void *ctx)
{
    key_ctx_t *kc = ctx;
    ai_key_info_t *k = kc->out;
    memset(k, 0, sizeof(*k));
    char base[136];
    base_url(base, sizeof(base));
    char *body;
    int status;
    esp_err_t e = ai_get_json(base, "key", OR_CAP_KEY, &body, &status, kc->err);
    if (e != ESP_OK) {
        return e;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(d)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    const char *s = ai_json_str(d, "label");
    strlcpy(k->label, s ? s : "", sizeof(k->label));
    k->usage = ai_json_num(d, "usage", 0);
    k->usage_daily = ai_json_num(d, "usage_daily", 0);
    k->usage_weekly = ai_json_num(d, "usage_weekly", 0);
    k->usage_monthly = ai_json_num(d, "usage_monthly", 0);
    k->has_limit = num_or_null(d, "limit", &k->limit);
    k->has_limit_remaining = num_or_null(d, "limit_remaining", &k->limit_remaining);
    s = ai_json_str(d, "limit_reset");
    strlcpy(k->limit_reset, s ? s : "", sizeof(k->limit_reset));
    k->free_tier = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "is_free_tier"));
    s = ai_json_str(d, "expires_at");
    strlcpy(k->expires_at, s ? s : "", sizeof(k->expires_at));
    const cJSON *eb = cJSON_GetObjectItemCaseSensitive(d, "effective_budget");
    if (cJSON_IsObject(eb)) {
        k->has_budget = num_or_null(eb, "remaining_usd", &k->budget_remaining);
        num_or_null(eb, "limit_usd", &k->budget_limit);
        num_or_null(eb, "spend_usd", &k->budget_spend);
        s = ai_json_str(eb, "resets_at");
        strlcpy(k->budget_resets_at, s ? s : "", sizeof(k->budget_resets_at));
    }
    // Remaining = min of the non-null caps; limit_remaining is not the account balance.
    if (k->has_limit_remaining) {
        k->has_remaining = true;
        k->remaining = k->limit_remaining;
        k->remaining_of = k->has_limit ? k->limit : 0;
    }
    if (k->has_budget && (!k->has_remaining || k->budget_remaining < k->remaining)) {
        k->has_remaining = true;
        k->remaining = k->budget_remaining;
        k->remaining_of = k->budget_limit;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ai_client_key_info(ai_key_info_t *out, or_err_t *err)
{
    or_err_t local;
    key_ctx_t kc = { .out = out, .err = err ? err : &local };
    memset(kc.err, 0, sizeof(*kc.err));
    return ai_run_sync(job_key, &kc);
}

// "1 ноября" for the next reset of a daily / weekly / monthly window (UTC), or of resets_at.
static void reset_date(const ai_key_info_t *k, char *out, size_t len)
{
    static const char *MON[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
                                 "сентября", "октября", "ноября", "декабря" };
    out[0] = 0;
    int y, m, d;
    bool from_budget = k->has_budget && k->remaining == k->budget_remaining && k->budget_resets_at[0];
    if (from_budget && sscanf(k->budget_resets_at, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12) {
        snprintf(out, len, "%d %s", d, MON[m - 1]);
        return;
    }
    if (!k->limit_reset[0] || !ai_time_synced()) {
        return;
    }
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    if (!strcmp(k->limit_reset, "monthly")) {
        t.tm_mday = 1;
        t.tm_mon += 1;
    } else if (!strcmp(k->limit_reset, "weekly")) {
        t.tm_mday += ((8 - t.tm_wday) % 7) ? (8 - t.tm_wday) % 7 : 7;    // next Monday
    } else if (!strcmp(k->limit_reset, "daily")) {
        t.tm_mday += 1;
    } else {
        return;
    }
    t.tm_hour = 12;           // normalise without crossing a day boundary
    time_t tt = mktime(&t);
    struct tm n;
    localtime_r(&tt, &n);
    snprintf(out, len, "%d %s", n.tm_mday, MON[n.tm_mon % 12]);
}

size_t ai_key_info_lines(const ai_key_info_t *k, char *out, size_t len)
{
    size_t o = 0;
    out[0] = 0;
#define ADD(...) do { if (o < len) o += (size_t)snprintf(out + o, len - o, __VA_ARGS__); } while (0)
    if (k->has_remaining) {
        char rd[32];
        reset_date(k, rd, sizeof(rd));
        ADD("Осталось $%.2f", k->remaining);
        if (k->remaining_of > 0) {
            ADD(" из $%.2f", k->remaining_of);
        }
        if (rd[0]) {
            ADD(" · сброс %s", rd);
        }
        ADD("\n");
    } else {
        ADD("Остаток недоступен: задайте лимит для ключа на openrouter.ai → Keys\n");
    }
    ADD("Потрачено: сегодня $%.2f · месяц $%.2f · всего $%.2f\n", k->usage_daily, k->usage_monthly, k->usage);
    if (k->has_remaining && k->remaining <= 0) {
        ADD("Лимит исчерпан\n");
    } else if (k->has_remaining && k->remaining < 1.0) {
        ADD("Мало средств — ответы могут замедлиться\n");
    }
    if (k->free_tier) {
        ADD("Бесплатный аккаунт — пополните баланс\n");
    }
#undef ADD
    if (o >= len) {
        o = len - 1;
    }
    return o;
}
