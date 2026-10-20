// Conversation memory of protocol §4.3: a RAM ring in PSRAM, never written to NVS.
#include "ai_priv.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TRANSCRIPT_MAX_CP 600
#define REPLY_MAX_CP      2500      // the longest reply length level (ai_len 2)

typedef struct {
    char *transcript, *reply;
    char lang[8];
    time_t ts;
} entry_t;

static entry_t s_ring[AI_HIST_MAX_N];
static int s_head;            // next slot to write
static int s_count;
static int s_mode = -1;
static char s_model[97];
static int64_t s_last_ms;
static SemaphoreHandle_t s_lock;

void hist_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

static void clear_locked(void)
{
    for (int i = 0; i < AI_HIST_MAX_N; i++) {
        free(s_ring[i].transcript);
        free(s_ring[i].reply);
        memset(&s_ring[i], 0, sizeof(s_ring[i]));
    }
    s_head = 0;
    s_count = 0;
}

void hist_clear(void)
{
    if (!s_lock) {
        return;    // before ai_client_init() (the console starts first): nothing stored yet
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_locked();
    xSemaphoreGive(s_lock);
}

void hist_touch(int mode, const char *model, int idle_min)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = ai_now_ms();
    bool reset = (s_mode >= 0 && mode != s_mode) || (s_model[0] && strcmp(model, s_model)) ||
                 (idle_min > 0 && s_count && now - s_last_ms > (int64_t)idle_min * 60000);
    if (reset && s_count) {
        clear_locked();
    }
    s_mode = mode;
    strlcpy(s_model, model, sizeof(s_model));
    xSemaphoreGive(s_lock);
}

void hist_add(const char *transcript, const char *reply, const char *lang)
{
    char *t = utf8_trunc_dup(transcript, TRANSCRIPT_MAX_CP);
    char *r = utf8_trunc_dup(reply, REPLY_MAX_CP);
    if (!t || !r) {
        free(t);
        free(r);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = &s_ring[s_head];
    free(e->transcript);
    free(e->reply);
    e->transcript = t;
    e->reply = r;
    strlcpy(e->lang, lang ? lang : "", sizeof(e->lang));
    e->ts = time(NULL);
    s_head = (s_head + 1) % AI_HIST_MAX_N;
    if (s_count < AI_HIST_MAX_N) {
        s_count++;
    }
    s_last_ms = ai_now_ms();
    xSemaphoreGive(s_lock);
}

int hist_count(void)
{
    return s_count;
}

void hist_append_messages(cJSON *messages, int max_n, int max_cp)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Walk newest to oldest, stop before the code-point budget would be exceeded.
    int take = 0;
    size_t total = 0;
    for (int k = 0; k < s_count && k < max_n; k++) {
        const entry_t *e = &s_ring[(s_head - 1 - k + AI_HIST_MAX_N) % AI_HIST_MAX_N];
        size_t cp = utf8_cp_len(e->transcript) + utf8_cp_len(e->reply);
        if (total + cp > (size_t)max_cp) {
            break;
        }
        total += cp;
        take++;
    }
    for (int k = take - 1; k >= 0; k--) {
        const entry_t *e = &s_ring[(s_head - 1 - k + AI_HIST_MAX_N) % AI_HIST_MAX_N];
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "role", "user");
        cJSON_AddStringToObject(u, "content", e->transcript);
        cJSON_AddItemToArray(messages, u);
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "role", "assistant");
        cJSON_AddStringToObject(a, "content", e->reply);
        cJSON_AddItemToArray(messages, a);
    }
    xSemaphoreGive(s_lock);
}
