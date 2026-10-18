// Recording mocks for the app host tests (see mocks.h).
#include "mocks.h"
#include "audio_in.h"
#include "audio_out.h"
#include "board.h"
#include "devconsole.h"
#include "net.h"
#include "orhttp.h"
#include "portal.h"
#include "settings.h"
#include "tts_client.h"
#include "ui.h"
#include "ui_str.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"

int64_t g_now_us;

// ---- libc / IDF bits

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}

int setenv(const char *k, const char *v, int overwrite)
{
    (void)overwrite;
    return _putenv_s(k, v);
}

const char *esp_err_to_name(esp_err_t e)
{
    (void)e;
    return "ERR";
}

void host_log(const char *lvl, const char *tag, const char *fmt, ...)
{
    if (lvl[0] != 'E' && lvl[0] != 'W') {
        return;
    }
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    printf("    [%s %s] %s\n", lvl, tag, b);
}

size_t heap_caps_get_free_size(unsigned caps)
{
    return caps & MALLOC_CAP_SPIRAM ? 4000000 : 120000;
}

size_t heap_caps_get_largest_free_block(unsigned caps)
{
    return caps & MALLOC_CAP_SPIRAM ? 2000000 : 60000;
}

size_t heap_caps_get_minimum_free_size(unsigned caps)
{
    return heap_caps_get_free_size(caps);
}

// ---- FreeRTOS: one FIFO queue type, no tasks

typedef struct {
    UBaseType_t n, sz, head, count;
    uint8_t *buf;
} hq_t;

QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t sz)
{
    hq_t *q = calloc(1, sizeof(hq_t));
    q->n = n;
    q->sz = sz;
    q->buf = calloc(n, sz);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t h, const void *item, TickType_t t)
{
    (void)t;
    hq_t *q = h;
    if (q->count == q->n) {
        return pdFALSE;
    }
    memcpy(q->buf + ((q->head + q->count) % q->n) * q->sz, item, q->sz);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t t)
{
    (void)t;
    hq_t *q = h;
    if (!q->count) {
        return pdFALSE;
    }
    memcpy(item, q->buf + q->head * q->sz, q->sz);
    q->head = (q->head + 1) % q->n;
    q->count--;
    return pdTRUE;
}

void vTaskDelay(TickType_t t)
{
    g_now_us += (int64_t)t * 1000;
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *h, BaseType_t core)
{
    (void)fn, (void)name, (void)stack, (void)arg, (void)prio, (void)core;
    *h = (TaskHandle_t)1;       // tests drive the handlers themselves
    return pdPASS;
}

// ---- settings

typedef struct {
    const char *key;
    bool str;
    int v;
    char s[64];
} kv_t;

static kv_t s_kv[] = {
    { "mode" }, { "text_only" }, { "volume" }, { "bright" }, { "dim_s" }, { "max_rec_s" }, { "vad_stop" },
    { "tr_vad" }, { "mic_slot" }, { "mic_shift" }, { "tr_lang_a", true }, { "tr_lang_b", true },
    { "ui_lang", true }, { "ai_key", true },
};
static setting_desc_t s_desc;
static void (*s_on_change)(const char *key, void *ctx);
static void *s_on_change_ctx;
int g_settings_writes;

static kv_t *kv(const char *k)
{
    for (size_t i = 0; i < sizeof(s_kv) / sizeof(s_kv[0]); i++) {
        if (!strcmp(s_kv[i].key, k)) {
            return &s_kv[i];
        }
    }
    return NULL;
}

void set_int(const char *k, int v)
{
    kv(k)->v = v;
}

void set_str(const char *k, const char *v)
{
    strlcpy(kv(k)->s, v, sizeof(kv(k)->s));
}

const setting_desc_t *settings_find(const char *key)
{
    return kv(key) ? &s_desc : NULL;
}

int32_t settings_get_int(const char *key)
{
    kv_t *e = kv(key);
    if (!e) {
        printf("    [mock] unknown int setting %s\n", key);
        return 0;
    }
    return e->v;
}

size_t settings_get_str(const char *key, char *buf, size_t len)
{
    kv_t *e = kv(key);
    strlcpy(buf, e ? e->s : "", len);
    return strlen(buf);
}

bool settings_is_set(const char *key)
{
    kv_t *e = kv(key);
    return e && e->s[0];
}

esp_err_t settings_set_int(const char *key, int32_t v)
{
    kv_t *e = kv(key);
    if (!e) {
        return ESP_ERR_NOT_FOUND;
    }
    e->v = v;
    g_settings_writes++;
    if (s_on_change) {
        s_on_change(key, s_on_change_ctx);
    }
    return ESP_OK;
}

void settings_on_change(void (*cb)(const char *key, void *ctx), void *ctx)
{
    s_on_change = cb;
    s_on_change_ctx = ctx;
}

// ---- ai / tts

submit_rec_t g_sub[64];
int g_nsub;
uint32_t g_cancel_upto, g_tts_cancel_upto;
int g_prewarm, g_hist_clear;
esp_err_t g_submit_rc;
speak_rec_t g_speak[32];
int g_nspeak;
static uint32_t s_turns;
static ai_event_cb_t s_listener;
static void *s_listener_ctx;

uint32_t ai_client_new_turn(void)
{
    return ++s_turns;
}

esp_err_t ai_client_submit(const ai_request_t *r)
{
    if (g_submit_rc != ESP_OK) {
        return g_submit_rc;
    }
    submit_rec_t *s = &g_sub[g_nsub++ % 64];
    memset(s, 0, sizeof(*s));
    s->turn = r->turn;
    s->mode = r->mode;
    s->pcm = r->pcm;
    s->samples = r->samples;
    if (r->text) {
        s->has_text = true;
        strlcpy(s->text, r->text, sizeof(s->text));
    }
    if (r->src_lang) {
        strlcpy(s->src, r->src_lang, sizeof(s->src));
    }
    return ESP_OK;
}

void ai_client_cancel(uint32_t turn)
{
    if (turn > g_cancel_upto) {
        g_cancel_upto = turn;
    }
    if (turn > g_tts_cancel_upto) {
        g_tts_cancel_upto = turn;
    }
}

esp_err_t ai_client_prewarm(void)
{
    g_prewarm++;
    return ESP_OK;
}

void ai_client_history_clear(void)
{
    g_hist_clear++;
}

bool ai_client_ready(void)
{
    return true;
}

esp_err_t ai_add_listener(ai_event_cb_t cb, void *ctx)
{
    s_listener = cb;
    s_listener_ctx = ctx;
    return ESP_OK;
}

esp_err_t tts_client_speak(uint32_t turn, const char *text, const char *lang)
{
    speak_rec_t *s = &g_speak[g_nspeak++ % 32];
    s->turn = turn;
    strlcpy(s->text, text, sizeof(s->text));
    strlcpy(s->lang, lang, sizeof(s->lang));
    return ESP_OK;
}

void tts_client_cancel(uint32_t turn)
{
    if (turn > g_tts_cancel_upto) {
        g_tts_cancel_upto = turn;
    }
}

void emit_simple(ai_ev_type_t t, uint32_t turn, int32_t i)
{
    ai_event_t ev = { .type = t, .turn = turn, .i = i };
    s_listener(&ev, s_listener_ctx);
}

void emit_result(uint32_t turn, ai_mode_t mode, const char *transcript, const char *text, const char *lang,
                 const char *src_lang, const char *tts_lang, bool unheard)
{
    ai_result_t r;
    memset(&r, 0, sizeof(r));
    r.turn = turn;
    r.mode = mode;
    r.transcript = (char *)transcript;
    r.text = (char *)text;
    strlcpy(r.lang, lang, sizeof(r.lang));
    strlcpy(r.src_lang, src_lang, sizeof(r.src_lang));
    strlcpy(r.tts_lang, tts_lang, sizeof(r.tts_lang));
    r.unheard = unheard;
    r.other_lang = mode == AI_MODE_TRANSLATOR && !strcmp(src_lang, "other");
    r.t.ai_ms = 1500;
    r.t.upload_ms = 300;
    ai_event_t ev = { .type = AI_EV_RESULT, .turn = turn, .result = &r };
    s_listener(&ev, s_listener_ctx);
}

void or_err_local(or_err_t *e, const char *id)
{
    memset(e, 0, sizeof(*e));
    strlcpy(e->id, id, sizeof(e->id));
    snprintf(e->msg_ru, sizeof(e->msg_ru), "msg %s", id);
    e->retry_after_ms = -1;
}

size_t or_wav_b64_len(size_t samples)
{
    return (44 + samples * 2 + 2) / 3 * 4;
}

void emit_error(ai_ev_type_t t, uint32_t turn, const char *id, const char *msg)
{
    or_err_t e;
    or_err_local(&e, id);
    if (msg) {
        strlcpy(e.msg_ru, msg, sizeof(e.msg_ru));
    }
    ai_event_t ev = { .type = t, .turn = turn, .err = &e };
    s_listener(&ev, s_listener_ctx);
}

// ---- audio

int16_t g_rec[REC_MAX];
size_t g_rec_len;
bool g_recording;
uint32_t g_preroll, g_max_ms;
int g_rec_starts, g_rec_stops;
int g_src;
int g_level;
bool g_out_busy;
int g_out_stops;
int g_volume_set = -1;

esp_err_t audio_in_start_ex(uint32_t max_ms, uint32_t preroll_ms)
{
    g_recording = true;
    g_rec_len = 0;
    g_preroll = preroll_ms;
    g_max_ms = max_ms;
    g_rec_starts++;
    return ESP_OK;
}

esp_err_t audio_in_stop(audio_clip_t *out)
{
    g_recording = false;
    g_rec_stops++;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->pcm = g_rec;
        out->samples = g_rec_len;
        out->rate = 16000;
        out->ms = (uint32_t)(g_rec_len / 16);
    }
    return ESP_OK;
}

bool audio_in_recording(void)
{
    return g_recording;
}

size_t audio_in_peek(const int16_t **pcm)
{
    *pcm = g_rec;
    return g_rec_len;
}

audio_src_t audio_in_get_source(void)
{
    return g_src ? AUDIO_SRC_INJECT : AUDIO_SRC_MIC;
}

int audio_in_level(void)
{
    return g_level;
}

void audio_in_set_format(bool right_slot, int shift)
{
    (void)right_slot, (void)shift;
}

void audio_out_stop(void)
{
    g_out_stops++;
    g_out_busy = false;
}

bool audio_out_busy(void)
{
    return g_out_busy;
}

void audio_out_set_volume(uint8_t pct)
{
    g_volume_set = pct;
}

// ---- console events

static char s_ev[512][200];
int g_nev;

void devcon_event(const char *ev, const char *fmt, ...)
{
    char *b = s_ev[g_nev++ % 512];
    int n = snprintf(b, 200, "%s ", ev);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b + n, 200 - (size_t)n, fmt, ap);
        va_end(ap);
    }
}

int ev_count(const char *name)
{
    size_t l = strlen(name);
    int c = 0;
    for (int i = 0; i < g_nev && i < 512; i++) {
        c += !strncmp(s_ev[i], name, l) && s_ev[i][l] == ' ';
    }
    return c;
}

const char *ev_last(const char *name)
{
    size_t l = strlen(name);
    for (int i = (g_nev < 512 ? g_nev : 512) - 1; i >= 0; i--) {
        if (!strncmp(s_ev[i], name, l) && s_ev[i][l] == ' ') {
            return s_ev[i];
        }
    }
    return "";
}

// ---- UI

ui_rec_t g_ui[1024];
int g_nui;
int g_ui_ctx;
bool g_boot_ok;
int g_pwr_short, g_pwr_down, g_pwr_up;
bool g_locked;
int g_boot_refused;
uint32_t g_inactive_ms;
bool g_touch_down;
int g_wait_release;
int g_ui_level;
static ui_input_cb_t s_in_cb;

static void ui_rec(int type, int32_t i, const char *s)
{
    if (g_nui >= 1024) {
        return;
    }
    ui_rec_t *r = &g_ui[g_nui++];
    r->type = type;
    r->i = i;
    r->has_s = s != NULL;
    strlcpy(r->s, s ? s : "", sizeof(r->s));
}

void ui_sleep_to(int level)
{
    // as ui_sleep.c: deeper levels only by its idle timer / screen off, any wake back to 0, each change reported
    if (level != g_ui_level) {
        g_ui_level = level;
        if (s_in_cb) {
            s_in_cb(UI_IN_SLEEP, level, NULL);
        }
    }
}

void ui_post(const ui_msg_t *m)
{
    // UI_USER_WAVE carries 32 binary bytes, not text
    ui_rec(m->type, m->i, m->type == UI_USER_WAVE ? (m->s ? "<wave>" : NULL) : m->s);
    free(m->s);
    if (m->type == UI_WAKE || m->type == UI_GOTO) {
        ui_sleep_to(0);
    } else if (m->type == UI_SCREEN_OFF && g_ui_level < 3) {
        ui_sleep_to(3);
    }
}

void ui_post_text(ui_msg_type_t type, int32_t i, const char *text)
{
    ui_rec(type, i, text);
}

void ui_set_input_cb(ui_input_cb_t cb, void *ctx)
{
    (void)ctx;
    s_in_cb = cb;
}

ui_ctx_t ui_context(void)
{
    return (ui_ctx_t)g_ui_ctx;
}

bool ui_boot_down(void)
{
    if (g_locked) {
        g_boot_refused++;       // the real UI shakes the padlock
        return false;
    }
    return g_boot_ok;
}

static void lock_to(bool on)
{
    // as scr_lock.c: the state flips in the UI task, which then tells the app
    if (on != g_locked) {
        g_locked = on;
        if (s_in_cb) {
            s_in_cb(UI_IN_LOCK, on ? 1 : 0, NULL);
        }
    }
}

void ui_pwr_short(void)
{
    g_pwr_short++;
    lock_to(!g_locked);
}

void ui_keylock(bool on)
{
    lock_to(on);
}

bool ui_keylocked(void)
{
    return g_locked;
}

const char *ui_s(ui_sid_t id)
{
    // the app only formats the low-battery toast itself
    return id == S_BAT_LOW ? (g_ui_lang ? "Battery %d %%" : "Батарея %d %%") : "?";
}

void ui_pwr_down(void)
{
    g_pwr_down++;
}

void ui_pwr_up(void)
{
    g_pwr_up++;
}

bool ui_lock(uint32_t ms)
{
    (void)ms;
    return true;
}

void ui_unlock(void)
{
}

int g_ui_lang;

int ui_str_lang(void)
{
    return g_ui_lang;
}

size_t ui_typed_text(char *out, size_t len)
{
    return len ? (out[0] = 0, 0) : 0;
}

int ui_count(int type)
{
    int c = 0;
    for (int i = 0; i < g_nui; i++) {
        c += g_ui[i].type == type;
    }
    return c;
}

const ui_rec_t *ui_last(int type)
{
    for (int i = g_nui - 1; i >= 0; i--) {
        if (g_ui[i].type == type) {
            return &g_ui[i];
        }
    }
    return NULL;
}

int ui_index(int type, int from)
{
    for (int i = from; i < g_nui; i++) {
        if (g_ui[i].type == type) {
            return i;
        }
    }
    return -1;
}

lv_indev_t *lv_indev_active(void)
{
    return (lv_indev_t *)&g_wait_release;     // any non-NULL pointer
}

void lv_indev_wait_release(lv_indev_t *in)
{
    (void)in;
    g_wait_release++;
}

uint32_t lv_display_get_inactive_time(const lv_display_t *d)
{
    (void)d;
    return g_inactive_ms;
}

lv_indev_t *lv_indev_get_next(lv_indev_t *in)
{
    return in ? NULL : (lv_indev_t *)&g_touch_down;     // one pointer device
}

lv_indev_type_t lv_indev_get_type(const lv_indev_t *in)
{
    (void)in;
    return LV_INDEV_TYPE_POINTER;
}

lv_indev_state_t lv_indev_get_state(const lv_indev_t *in)
{
    (void)in;
    return g_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ---- board, net

int g_backlight = -1;
int g_power_off, g_latch_on;
bool g_boot_down, g_pwr_held;
int g_bat_mv = -1;
bool g_bat_chg;
int g_net_state = NET_UP;
bool g_portal;

void board_backlight_set(uint8_t pct)
{
    g_backlight = pct;
}

void board_power_off(void)
{
    g_power_off++;
    g_backlight = 0;
}

void board_power_latch_on(void)
{
    g_latch_on++;
}

esp_err_t board_buttons_start(board_btn_cb_t cb, void *ctx)
{
    (void)cb, (void)ctx;
    return ESP_OK;
}

bool board_button_down(board_btn_t b)
{
    return b == BOARD_BTN_BOOT ? g_boot_down : g_pwr_held;
}

const char *board_btn_name(board_btn_t b)
{
    return b == BOARD_BTN_BOOT ? "boot" : "pwr";
}

const char *board_btn_ev_name(board_btn_ev_t ev)
{
    static const char *const n[] = { "down", "up", "click", "long" };
    return n[ev];
}

int board_battery_mv(void)
{
    return g_bat_mv;
}

int board_battery_pct(int mv)
{
    if (mv < 2800 || mv > 4400) {
        return -1;
    }
    int p = (mv - 3300) * 100 / 900;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

bool board_battery_sample(void)
{
    return false;
}

void board_battery_state(board_bat_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mv = g_bat_mv;
    out->pct = board_battery_pct(g_bat_mv);
    out->charging = out->pct >= 0 && g_bat_chg;
}

void board_battery_sim(int pct, bool charging)
{
    (void)pct, (void)charging;
}

void net_status(net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = (net_state_t)g_net_state;
    strlcpy(out->ssid, "home", sizeof(out->ssid));
}

const char *net_state_name(net_state_t s)
{
    static const char *const n[] = { "off", "connecting", "up", "failed" };
    return n[s];
}

bool portal_active(void)
{
    return g_portal;
}

void mocks_reset(void)
{
    g_now_us = 10LL * 1000 * 1000;
    g_nui = g_nsub = g_nspeak = g_nev = 0;
    g_cancel_upto = g_tts_cancel_upto = 0;
    g_prewarm = g_hist_clear = g_settings_writes = 0;
    g_submit_rc = ESP_OK;
    g_rec_len = 0;
    g_recording = false;
    g_preroll = g_max_ms = 0;
    g_rec_starts = g_rec_stops = 0;
    g_src = 0;
    g_level = 30;               // -45 dBFS idle mic, as on the bench with the PC fan
    g_out_busy = false;
    g_out_stops = 0;
    g_ui_ctx = UI_CTX_HOME;
    g_boot_ok = true;
    g_pwr_short = g_pwr_down = g_pwr_up = 0;
    g_locked = false;
    g_boot_refused = 0;
    g_bat_chg = false;
    g_inactive_ms = 100000;
    g_touch_down = false;
    g_ui_level = 0;
    g_ui_lang = 0;
    g_wait_release = 0;
    g_backlight = -1;
    g_power_off = g_latch_on = 0;
    g_boot_down = g_pwr_held = false;
    g_bat_mv = -1;
    g_net_state = NET_UP;
    g_portal = false;
    set_int("mode", 0);
    set_int("text_only", 1);
    set_int("volume", 60);
    set_int("bright", 80);
    set_int("dim_s", 60);
    set_int("max_rec_s", 30);
    set_int("vad_stop", 0);
    set_int("tr_vad", 1);
    set_int("mic_slot", 0);
    set_int("mic_shift", 12);
    set_str("tr_lang_a", "ru");
    set_str("tr_lang_b", "en");
    set_str("ui_lang", "ru");
    set_str("ai_key", "sk-or-v1-test");
}
