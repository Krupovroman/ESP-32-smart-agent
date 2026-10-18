// UI background worker and debounced settings writes (ui_work.h).
#include "ui_work.h"
#include "ui_internal.h"
#include "ui_str.h"
#include "settings.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ui_work";

#define WORK_Q_LEN      12
#define WORK_STACK      6144    // net_scan keeps 20 AP records (1.8 KB) on the stack
#define WORK_PRIO       3
#define WORK_IDLE_MS    20000
#define SAVE_DELAY_MS   1500
#define SAVE_MAX_DEFER  20000
#define PEND_MAX        10

typedef struct {
    ui_job_fn_t fn, done;
    void *arg;
} job_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_mx;
static TaskHandle_t s_task;

static void worker(void *p)
{
    (void)p;
    job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(WORK_IDLE_MS)) == pdTRUE) {
            if (j.fn) {
                j.fn(j.arg);
            }
            if (!j.done) {
                heap_caps_free(j.arg);
                continue;
            }
            job_t *d = heap_caps_malloc(sizeof(job_t), MALLOC_CAP_SPIRAM);
            if (d) {
                *d = j;
                // blocking is fine here: the done callback must not be lost while the UI queue is momentarily full
                if (ui_post_wait(&(ui_msg_t){ .type = UI_WORK_DONE, .s = (char *)d }, 1000)) {
                    continue;
                }
                heap_caps_free(d);
            }
            ESP_LOGW(TAG, "done callback dropped");
            heap_caps_free(j.arg);
            continue;
        }
        // idle: exit unless a job arrived meanwhile (ui_work creates the task again under the same mutex)
        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (uxQueueMessagesWaiting(s_q) == 0) {
            s_task = NULL;
            xSemaphoreGive(s_mx);
            vTaskDelete(NULL);
        }
        xSemaphoreGive(s_mx);
    }
}

void *ui_work_alloc(size_t n)
{
    return heap_caps_calloc(1, n ? n : 1, MALLOC_CAP_SPIRAM);
}

void ui_work_init(void)
{
    if (!s_q) {
        s_q = xQueueCreate(WORK_Q_LEN, sizeof(job_t));
        s_mx = xSemaphoreCreateMutex();
    }
}

bool ui_work(ui_job_fn_t fn, ui_job_fn_t done, void *arg)
{
    if (!s_q || !s_mx) {
        heap_caps_free(arg);
        return false;
    }
    job_t j = { fn, done, arg };
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool ok = xQueueSend(s_q, &j, 0) == pdTRUE;
    if (ok && !s_task) {
        // internal stack (default caps): settings and Wi-Fi writes need it; core 0 with Wi-Fi and TLS
        if (xTaskCreatePinnedToCore(worker, "ui_work", WORK_STACK, NULL, WORK_PRIO, &s_task, 0) != pdPASS) {
            s_task = NULL;
            xQueueReset(s_q);
            ok = false;
        }
    }
    xSemaphoreGive(s_mx);
    if (!ok) {
        ESP_LOGW(TAG, "job dropped");
        heap_caps_free(arg);
    }
    return ok;
}

void ui_work_dispatch(void *job)
{
    job_t *j = job;
    if (!j) {
        return;
    }
    if (j->done) {
        j->done(j->arg);
    }
    heap_caps_free(j->arg);
}

// ---- Debounced settings writes

typedef struct {
    char key[16];
    bool used, str;
    int32_t v;
    char *s;            // PSRAM copy (secrets are zeroed before free)
    uint32_t gen;       // bumped on every change; a finished write clears the slot only if it is still current
} pend_t;

typedef struct {
    int n;
    pend_t e[PEND_MAX];
    bool failed;
} save_job_t;

static pend_t s_pend[PEND_MAX];
static uint32_t s_gen;
static lv_timer_t *s_save_timer;
static uint32_t s_first_t;

static void str_free(char *s)
{
    if (s) {
        memset(s, 0, strlen(s));
        heap_caps_free(s);
    }
}

static char *str_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static pend_t *pend_find(const char *key)
{
    for (int i = 0; i < PEND_MAX; i++) {
        if (s_pend[i].used && !strcmp(s_pend[i].key, key)) {
            return &s_pend[i];
        }
    }
    return NULL;
}

static void save_job(void *arg)
{
    save_job_t *j = arg;
    for (int i = 0; i < j->n; i++) {
        pend_t *e = &j->e[i];
        esp_err_t err = e->str ? settings_set_str(e->key, e->s ? e->s : "") : settings_set_int(e->key, e->v);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "save %s: %s", e->key, esp_err_to_name(err));
            j->failed = true;
        }
    }
}

static void save_done(void *arg)
{
    save_job_t *j = arg;
    for (int i = 0; i < j->n; i++) {
        pend_t *p = pend_find(j->e[i].key);
        if (p && p->gen == j->e[i].gen) {
            str_free(p->s);
            memset(p, 0, sizeof(*p));
        }
        str_free(j->e[i].s);
    }
    if (j->failed) {
        ui_toast(ui_s(S_NOT_SAVED));
    }
}

static void arm(void);

static void flush_now(void)
{
    save_job_t *j = ui_work_alloc(sizeof(save_job_t));
    if (!j) {
        return;
    }
    char *dups[PEND_MAX];
    int nd = 0;
    for (int i = 0; i < PEND_MAX; i++) {
        if (!s_pend[i].used) {
            continue;
        }
        pend_t *e = &j->e[j->n];
        *e = s_pend[i];
        e->s = NULL;
        if (s_pend[i].str) {
            // no copy, no write: a NULL string would be stored as "" (an erased key); the slot stays pending
            if (!(e->s = str_dup(s_pend[i].s ? s_pend[i].s : ""))) {
                continue;
            }
            dups[nd++] = e->s;
        }
        j->n++;
    }
    if (j->n == 0) {
        heap_caps_free(j);
        return;
    }
    if (!ui_work(save_job, save_done, j)) {
        // the job was dropped (and freed by ui_work): its copies go too, secrets zeroed; the slots stay pending
        for (int i = 0; i < nd; i++) {
            str_free(dups[i]);
        }
        arm();      // try again later instead of waiting for the next change
    }
}

static bool app_busy(void)
{
    int32_t st = ui_get(g_s_state);
    return st == UI_ST_REC || st == UI_ST_REC_LATCHED || st == UI_ST_REC_CANCEL || st == UI_ST_UPLOAD ||
           st == UI_ST_THINK || st == UI_ST_SPEAK;
}

static void save_timer_cb(lv_timer_t *t)
{
    if (app_busy() && lv_tick_elaps(s_first_t) < SAVE_MAX_DEFER) {
        lv_timer_reset(t);
        lv_timer_set_repeat_count(t, 1);
        lv_timer_resume(t);
        return;
    }
    s_first_t = 0;
    flush_now();
}

static void arm(void)
{
    if (!s_save_timer) {
        s_save_timer = lv_timer_create(save_timer_cb, SAVE_DELAY_MS, NULL);
        lv_timer_set_auto_delete(s_save_timer, false);
    }
    if (!s_first_t) {
        s_first_t = lv_tick_get() | 1u;
    }
    lv_timer_set_repeat_count(s_save_timer, 1);
    lv_timer_reset(s_save_timer);
    lv_timer_resume(s_save_timer);
}

static pend_t *pend_slot(const char *key)
{
    pend_t *p = pend_find(key);
    if (p) {
        return p;
    }
    for (int i = 0; i < PEND_MAX; i++) {
        if (!s_pend[i].used) {
            p = &s_pend[i];
            memset(p, 0, sizeof(*p));
            p->used = true;
            strlcpy(p->key, key, sizeof(p->key));
            return p;
        }
    }
    // table full: write what is pending now and retry once (the slots stay until the write finishes)
    flush_now();
    return NULL;
}

void ui_save_int(const char *key, int32_t v)
{
    pend_t *p = pend_slot(key);
    if (!p) {
        ESP_LOGW(TAG, "no slot for %s", key);
        return;
    }
    p->str = false;
    p->v = v;
    p->gen = ++s_gen;
    arm();
}

void ui_save_str(const char *key, const char *v)
{
    pend_t *p = pend_slot(key);
    char *copy = str_dup(v ? v : "");
    if (!p || !copy) {
        str_free(copy);
        ESP_LOGW(TAG, "no slot for %s", key);
        return;
    }
    str_free(p->s);
    p->str = true;
    p->s = copy;
    p->gen = ++s_gen;
    arm();
}

void ui_save_flush(void)
{
    if (s_save_timer) {
        lv_timer_pause(s_save_timer);
    }
    s_first_t = 0;
    flush_now();
}

bool ui_save_pending(const char *key)
{
    return pend_find(key) != NULL;
}

int32_t ui_cfg_int(const char *key)
{
    pend_t *p = pend_find(key);
    return p && !p->str ? p->v : settings_get_int(key);
}

size_t ui_cfg_str(const char *key, char *buf, size_t len)
{
    pend_t *p = pend_find(key);
    if (p && p->str && len) {
        strlcpy(buf, p->s ? p->s : "", len);
        return strlen(buf);
    }
    return settings_get_str(key, buf, len);
}
