// UI core (docs/ui.md 7.6-7.8, architecture 1.13): init, the ui_post queue and its 20 ms pump, the
// navigation stack, physical-button hooks, and the inspection helpers used by the dev console.
#include "ui.h"
#include "ui_chat.h"
#include "ui_geom.h"
#include "ui_internal.h"
#include "ui_list.h"
#include "ui_orb.h"
#include "ui_rim.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_theme.h"
#include "ui_work.h"
#include "settings.h"

#include <stdlib.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "src/core/lv_obj_class_private.h"   // class names for the tree dump

static const char *TAG = "ui";

#define UI_Q_DEPTH      32
#define PUMP_MS         20
#define PUMP_BATCH      16

ui_subj_t g_s_state, g_s_mode, g_s_text_only, g_s_speaker, g_s_fault, g_s_net, g_s_upload, g_s_hints, g_s_portal;
ui_subj_t g_s_tr_dir, g_s_face;
char g_lang_a[4] = "ru", g_lang_b[4] = "en";

static QueueHandle_t s_q;
static atomic_uint s_posted, s_done, s_dropped;
static ui_input_cb_t s_in_cb;
static void *s_in_ctx;
static atomic_int s_ctx = UI_CTX_HOME;
static bool s_ready;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *scroller;     // what the rim thumb follows on this screen (H decides its own)
    ui_screen_t id;
    bool (*back)(void);     // optional back handler (onboarding: PWR = previous step)
} nav_t;

static nav_t s_nav[UI_NAV_MAX];
static int s_depth;

typedef lv_obj_t *(*builder_t)(uint32_t arg);

static const struct {
    const char *name;
    builder_t build;
    ui_ctx_t ctx;
} s_screens[UI_SCR__N] = {
    [UI_SCR_HOME] = { "home", NULL, UI_CTX_HOME },
    [UI_SCR_HIST] = { "hist", scr_hist_build, UI_CTX_READ },
    [UI_SCR_READER] = { "reader", scr_reader_build, UI_CTX_READ },
    [UI_SCR_TOUCH] = { "touch", scr_touch_build, UI_CTX_TOUCH },
    [UI_SCR_TEST] = { "test", ui_test_build, UI_CTX_TEST },
    [UI_SCR_MODE] = { "mode", scr_mode_build, UI_CTX_LIST },
    [UI_SCR_SETTINGS] = { "settings", scr_settings_build, UI_CTX_LIST },
    [UI_SCR_WIFI] = { "wifi", scr_wifi_build, UI_CTX_LIST },
    [UI_SCR_WIFI_NET] = { "wifinet", scr_wifi_net_build, UI_CTX_LIST },
    [UI_SCR_WIFI_CONN] = { "wificonn", scr_wifi_conn_build, UI_CTX_STEP },
    [UI_SCR_TEXT] = { "text", scr_text_build, UI_CTX_TEXT },
    [UI_SCR_QR] = { "phone", scr_qr_build, UI_CTX_QR },
    [UI_SCR_PROVIDER] = { "provider", scr_provider_build, UI_CTX_LIST },
    [UI_SCR_MODELS] = { "models", scr_models_build, UI_CTX_LIST },
    [UI_SCR_KEYINFO] = { "keyinfo", scr_keyinfo_build, UI_CTX_LIST },
    [UI_SCR_VOICE] = { "voice", scr_voice_build, UI_CTX_LIST },
    [UI_SCR_TTS_MODELS] = { "ttsmodels", scr_tts_models_build, UI_CTX_LIST },
    [UI_SCR_VOICES] = { "voices", scr_voices_build, UI_CTX_LIST },
    [UI_SCR_SPEED] = { "speed", scr_speed_build, UI_CTX_LIST },
    [UI_SCR_LANGS] = { "langs", scr_langs_build, UI_CTX_LIST },
    [UI_SCR_LANG_PICK] = { "langpick", scr_lang_pick_build, UI_CTX_LIST },
    [UI_SCR_DIR] = { "dir", scr_dir_build, UI_CTX_LIST },
    [UI_SCR_ADJUST] = { "adjust", scr_adjust_build, UI_CTX_ADJUST },
    [UI_SCR_DISPLAY] = { "display", scr_display_build, UI_CTX_LIST },
    [UI_SCR_ABOUT] = { "about", scr_about_build, UI_CTX_LIST },
    [UI_SCR_LICENSES] = { "licenses", scr_licenses_build, UI_CTX_LIST },
    [UI_SCR_DIALOG] = { "dialog", scr_dialog_build, UI_CTX_DIALOG },
    [UI_SCR_ONBOARD] = { "onboard", scr_onboard_build, UI_CTX_STEP },
    [UI_SCR_AMBIENT] = { "ambient", scr_ambient_build, UI_CTX_AMBIENT },
};

// ---- Posting (any task)

void ui_post(const ui_msg_t *m)
{
    if (!m) {
        return;
    }
    if (!s_q || xQueueSend(s_q, m, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1);
        heap_caps_free(m->s);
        return;
    }
    atomic_fetch_add(&s_posted, 1);
}

void ui_post_text(ui_msg_type_t type, int32_t i, const char *text)
{
    ui_msg_t m = { .type = type, .i = i, .s = NULL };
    if (text) {
        size_t n = strlen(text) + 1;
        m.s = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (!m.s) {
            atomic_fetch_add(&s_dropped, 1);
            return;
        }
        memcpy(m.s, text, n);
    }
    ui_post(&m);
}

bool ui_post_wait(const ui_msg_t *m, uint32_t ms)
{
    if (!m || !s_q || xQueueSend(s_q, m, pdMS_TO_TICKS(ms)) != pdTRUE) {
        return false;
    }
    atomic_fetch_add(&s_posted, 1);
    return true;
}

// ---- Typed question (UI_IN_TEXT): the app copies it out with ui_typed_text()

static SemaphoreHandle_t s_typed_mx;
static char *s_typed;

bool ui_send_typed(const char *text)
{
    size_t n = text ? strlen(text) : 0;
    char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!copy) {
        return false;
    }
    memcpy(copy, text ? text : "", n + 1);
    xSemaphoreTake(s_typed_mx, portMAX_DELAY);
    char *old = s_typed;
    s_typed = copy;
    xSemaphoreGive(s_typed_mx);
    heap_caps_free(old);
    return ui_emit(UI_IN_TEXT, (int32_t)n);
}

size_t ui_typed_text(char *out, size_t len)
{
    if (!out || !len || !s_typed_mx) {
        return 0;
    }
    xSemaphoreTake(s_typed_mx, portMAX_DELAY);
    size_t n = ui_str_copy(out, len, s_typed ? s_typed : "");
    xSemaphoreGive(s_typed_mx);
    return n;
}

void ui_set_input_cb(ui_input_cb_t cb, void *ctx)
{
    s_in_ctx = ctx;
    s_in_cb = cb;
}

bool ui_emit(ui_input_t ev, int32_t i)
{
    if (!s_in_cb) {
        return false;
    }
    s_in_cb(ev, i, s_in_ctx);
    return true;
}

bool ui_lock(uint32_t ms)
{
    return lvgl_port_lock(ms);
}

void ui_unlock(void)
{
    lvgl_port_unlock();
}

// ---- Navigation (7.8)

const char *ui_screen_name(ui_screen_t id)
{
    return (unsigned)id < UI_SCR__N ? s_screens[id].name : NULL;
}

ui_screen_t ui_nav_current(void)
{
    return s_depth > 0 ? s_nav[s_depth - 1].id : UI_SCR_HOME;
}

bool ui_nav_has(ui_screen_t id)
{
    for (int i = 0; i < s_depth; i++) {
        if (s_nav[i].id == id) {
            return true;
        }
    }
    return false;
}

static void ctx_update(void)
{
    atomic_store(&s_ctx, (int)s_screens[ui_nav_current()].ctx);
}

static lv_obj_t *nav_build(ui_screen_t id, uint32_t arg, lv_obj_t **scroller)
{
    lv_obj_t *prev_scroller = ui_rim_scroller();
    ui_rim_attach_scroll(NULL);     // builders attach their own scroller
    lv_obj_t *scr = s_screens[id].build(arg);
    if (!scr) {
        ui_rim_attach_scroll(prev_scroller);
        return NULL;
    }
    *scroller = ui_rim_scroller();
    return scr;
}

void ui_nav_push(ui_screen_t id, uint32_t arg)
{
    if ((unsigned)id >= UI_SCR__N || id == UI_SCR_HOME) {
        ui_nav_home();
        return;
    }
    if (s_depth >= UI_NAV_MAX) {
        ESP_LOGW(TAG, "nav stack full");
        return;
    }
    lv_obj_t *scroller;
    lv_obj_t *scr = nav_build(id, arg, &scroller);
    if (!scr) {
        return;
    }
    s_nav[s_depth++] = (nav_t){ scr, scroller, id, NULL };
    lv_screen_load(scr);        // instant: no load animation (A33)
    ctx_update();
}

void ui_nav_replace(ui_screen_t id, uint32_t arg)
{
    if (s_depth <= 1 || (unsigned)id >= UI_SCR__N || id == UI_SCR_HOME) {
        ui_nav_push(id, arg);
        return;
    }
    lv_obj_t *scroller;
    lv_obj_t *scr = nav_build(id, arg, &scroller);
    if (!scr) {
        return;
    }
    lv_obj_t *old = s_nav[s_depth - 1].scr;
    s_nav[s_depth - 1] = (nav_t){ scr, scroller, id, NULL };
    lv_screen_load(scr);
    lv_obj_delete_async(old);   // may be called from an event of the screen being replaced
    ctx_update();
}

void ui_nav_set_back(bool (*fn)(void))
{
    if (s_depth > 1) {
        s_nav[s_depth - 1].back = fn;
    }
}

static void nav_pop(void)
{
    if (s_depth <= 1) {
        return;
    }
    nav_t cur = s_nav[--s_depth];
    ui_rim_attach_scroll(NULL);
    lv_screen_load(s_nav[s_depth - 1].scr);
    lv_obj_delete_async(cur.scr);   // may be called from an event of the screen being left
    if (ui_nav_current() == UI_SCR_HOME) {
        scr_home_resume();
    } else {
        ui_rim_attach_scroll(s_nav[s_depth - 1].scroller);
    }
    ctx_update();
}

void ui_nav_back(void)
{
    if (s_depth > 1 && s_nav[s_depth - 1].back && s_nav[s_depth - 1].back()) {
        return;
    }
    nav_pop();
}

void ui_nav_home(void)
{
    // never through the back handlers (an onboarding step would refuse to leave), and without loading the
    // screens in between: their SCREEN_LOADED refreshes (scans, status jobs) would run for nothing
    if (s_depth <= 1) {
        return;
    }
    ui_rim_attach_scroll(NULL);
    lv_screen_load(s_nav[0].scr);
    while (s_depth > 1) {
        lv_obj_delete_async(s_nav[--s_depth].scr);
    }
    scr_home_resume();
    ctx_update();
}

static void nav_goto(int32_t id, const char *arg)
{
    uint32_t a = arg ? (uint32_t)strtoul(arg, NULL, 10) : 0;
    if (id == UI_SCR_HOME) {
        ui_nav_home();
    } else if ((unsigned)id < UI_SCR__N && (id != (int32_t)ui_nav_current() || arg)) {
        ui_nav_home();
        ui_nav_push((ui_screen_t)id, a);
    }
}

void ui_open(ui_open_t what)
{
    switch (what) {
    case UI_OPEN_MODE_SHEET: ui_nav_push(UI_SCR_MODE, 0); break;
    case UI_OPEN_SETTINGS: ui_nav_push(UI_SCR_SETTINGS, 0); break;
    case UI_OPEN_WIFI: ui_nav_push(UI_SCR_WIFI, 0); break;
    case UI_OPEN_LANG_A: ui_nav_push(UI_SCR_LANG_PICK, 0); break;
    case UI_OPEN_LANG_B: ui_nav_push(UI_SCR_LANG_PICK, 1); break;
    case UI_OPEN_FAULT_FIX:
        // the screen that fixes the persistent fault (3.14): W | P | AI, credit details, diagnostics
        switch (ui_get(g_s_fault)) {
        case UI_FAULT_BAD_KEY:
        case UI_FAULT_NO_KEY:
        case UI_FAULT_REGION: ui_nav_push(UI_SCR_PROVIDER, 0); break;
        case UI_FAULT_NO_CREDIT:
        case UI_FAULT_LOW_CREDIT: ui_nav_push(UI_SCR_KEYINFO, 0); break;
        case UI_FAULT_MODEL:
        case UI_FAULT_MODEL_EXPIRING: ui_nav_push(UI_SCR_MODELS, 0); break;
        case UI_FAULT_MIC: ui_nav_push(UI_SCR_ABOUT, 0); break;
        default: ui_nav_push(UI_SCR_WIFI, 0); break;
        }
        break;
    default: break;
    }
}

bool ui_open_card(int32_t card)
{
    switch (card) {
    case UI_CARD_NO_WIFI: ui_nav_push(UI_SCR_WIFI, 0); return true;
    case UI_CARD_BAD_KEY:
    case UI_CARD_NO_KEY:
    case UI_CARD_REGION: ui_nav_push(UI_SCR_PROVIDER, 0); return true;
    case UI_CARD_NO_CREDIT: ui_nav_push(UI_SCR_KEYINFO, 0); return true;
    case UI_CARD_MODEL: ui_nav_push(UI_SCR_MODELS, 0); return true;
    case UI_CARD_MIC:
    case UI_CARD_NO_SPEAKER: ui_nav_push(UI_SCR_ABOUT, 0); return true;
    default: return false;      // retry cards: the app re-sends the cached clip
    }
}

void ui_toast(const char *text)
{
    if (ui_nav_current() == UI_SCR_HOME) {
        scr_home_toast(text);
    } else {
        ui_chrome_toast(text);
    }
}

// ---- PWR hold ring (A29): 600 ms delay, then 0 -> 1000 permille over 2.4 s; release retracts in 200 ms

static int32_t s_pwr_v;

static void pwr_exec(void *var, int32_t v)
{
    (void)var;
    ui_rim_power(v);
}

static void pwr_retracted(lv_anim_t *a)
{
    (void)a;
    ui_rim_power(-1);
}

static void pwr_ring(bool down)
{
    lv_anim_delete(&s_pwr_v, pwr_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_pwr_v);
    lv_anim_set_exec_cb(&a, pwr_exec);
    if (down) {
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_delay(&a, 600);
        lv_anim_set_duration(&a, 2400);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
    } else {
        int32_t cur = ui_rim_power_get();
        if (cur <= 0) {
            ui_rim_power(-1);
            return;
        }
        lv_anim_set_values(&a, cur, 0);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_completed_cb(&a, pwr_retracted);
    }
    lv_anim_start(&a);
}

// ---- Pump: the only place app messages touch widgets and subjects (7.7)

static void set_langs(const char *s)
{
    // "ru|en"
    const char *bar = s ? strchr(s, '|') : NULL;
    if (!bar || bar == s || !bar[1]) {
        return;
    }
    snprintf(g_lang_a, sizeof(g_lang_a), "%.*s", (int)LV_MIN(bar - s, 3), s);
    snprintf(g_lang_b, sizeof(g_lang_b), "%.3s", bar + 1);
    scr_home_langs_changed();
}

static void rebuild_home(lv_timer_t *t)
{
    (void)t;
    // the old H goes at once (never inside one of its own events: this is a one-shot timer): until it is gone
    // its subject observers would run a second time against the new screen's widgets
    lv_obj_t *old = s_nav[0].scr;
    s_nav[0].scr = scr_home_create();
    if (s_depth == 1) {
        lv_screen_load(s_nav[0].scr);
        scr_home_resume();
    }
    lv_obj_delete(old);
}

void ui_apply_lang(int lang)
{
    if (lang == ui_str_lang()) {
        return;
    }
    // strings are baked into the widgets: drop the stack now and rebuild H (the chat model survives) from a
    // timer, because this may run inside an event of a screen that is still alive
    ui_str_set_lang(lang);
    ui_nav_home();
    lv_timer_t *t = lv_timer_create(rebuild_home, 0, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void set_lang(int lang)
{
    ui_apply_lang(lang);
}

static void dispatch(const ui_msg_t *m)
{
    switch (m->type) {
    case UI_SET_STATE: ui_set(g_s_state, LV_CLAMP(0, m->i, UI_ST__N - 1)); break;
    case UI_SET_MODE: ui_set(g_s_mode, m->i ? 1 : 0); break;
    case UI_USER_MSG: scr_home_user(m->s, m->i); break;
    case UI_BOT_MSG:
        if (m->s) {
            scr_home_bot(m->s, m->i != 0);
        }
        break;
    case UI_BOT_APPEND: scr_home_bot_append(m->s); break;
    case UI_TOAST: ui_toast(m->s ? m->s : ui_s((ui_sid_t)m->i)); break;
    case UI_ERROR: scr_home_error(m->i, m->s); break;
    case UI_SET_NET: ui_set(g_s_net, LV_CLAMP(0, m->i, UI_NET_FAILED)); break;
    case UI_PORTAL: ui_set(g_s_portal, m->i); break;
    case UI_GOTO:
        ui_sleep_wake();        // a requested screen is meant to be seen (and the wake catcher must go)
        nav_goto(m->i, m->s);
        break;
    case UI_SET_TEXT_ONLY: ui_set(g_s_text_only, m->i ? 1 : 0); break;
    case UI_RETRY: scr_home_retry(m->i); break;
    case UI_SET_MOOD: scr_home_mood(m->i); break;
    case UI_SET_FAULT: ui_set(g_s_fault, LV_CLAMP(0, m->i, UI_FAULT__N - 1)); break;
    case UI_UPLOAD_PCT: ui_set(g_s_upload, LV_CLAMP(0, m->i, 100)); break;
    case UI_USER_WAVE: scr_home_wave((const uint8_t *)m->s); break;
    case UI_CLEAR: scr_home_clear(ui_nav_current() == UI_SCR_HOME); break;
    case UI_BACK: ui_nav_back(); break;
    case UI_PWR_RING: pwr_ring(m->i != 0); break;
    case UI_SET_LANG: set_lang(m->i ? 1 : 0); break;
    case UI_STATUS: scr_home_status_text(m->s ? m->s : ui_s((ui_sid_t)m->i), 2000); break;
    case UI_SET_SPEAKER: ui_set(g_s_speaker, m->i ? 1 : 0); break;
    case UI_SET_LANGS: set_langs(m->s); break;
    case UI_SET_HINTS: ui_set(g_s_hints, LV_CLAMP(0, m->i, 3)); break;
    case UI_DIVIDER: scr_home_divider(m->s); break;
    case UI_SET_FACE: ui_set(g_s_face, m->i ? 1 : 0); break;
    case UI_SET_TR_DIR: ui_set(g_s_tr_dir, LV_CLAMP(0, m->i, 2)); break;
    case UI_WAKE: ui_sleep_wake(); break;
    case UI_SCREEN_OFF: ui_sleep_off(); break;
    case UI_WORK_DONE: ui_work_dispatch(m->s); break;
    case UI_SET_TURN: cm_set_tag((uint32_t)m->i); break;
    default: break;
    }
}

static void pump_cb(lv_timer_t *t)
{
    (void)t;
    ui_msg_t m;
    for (int n = 0; n < PUMP_BATCH && xQueueReceive(s_q, &m, 0) == pdTRUE; n++) {
        dispatch(&m);
        heap_caps_free(m.s);
        atomic_fetch_add(&s_done, 1);
    }
}

bool ui_wait_pumped(uint32_t timeout_ms)
{
    unsigned target = atomic_load(&s_posted);
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 10) {
        if ((int)(atomic_load(&s_done) - target) >= 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

unsigned ui_dropped(void)
{
    return atomic_load(&s_dropped);
}

// ---- Physical buttons (2.3): decisions from the atomic context, actions through the queue

ui_ctx_t ui_context(void)
{
    return (ui_ctx_t)atomic_load(&s_ctx);
}

bool ui_boot_down(void)
{
    ui_ctx_t c = ui_context();
    if (ui_asleep()) {
        // BOOT wakes to H and records at once (5.8); the wake leaves the ambient screen
        ui_post(&(ui_msg_t){ .type = UI_WAKE });
        if (c == UI_CTX_AMBIENT || c == UI_CTX_HOME) {
            return true;
        }
    }
    if (c == UI_CTX_TEXT) {
        ui_post_text(UI_TOAST, 0, ui_s(S_FINISH_INPUT));
        return false;
    }
    if (c == UI_CTX_DIALOG || c == UI_CTX_STEP) {
        return false;
    }
    if (c != UI_CTX_HOME) {
        ui_post(&(ui_msg_t){ .type = UI_GOTO, .i = UI_SCR_HOME });   // jump to H/H' and record there
    }
    return true;
}

bool ui_pwr_short(void)
{
    if (ui_asleep()) {
        ui_post(&(ui_msg_t){ .type = UI_WAKE });     // PWR only wakes
        return true;
    }
    if (ui_context() == UI_CTX_HOME) {
        return false;
    }
    ui_post(&(ui_msg_t){ .type = UI_BACK });
    return true;
}

void ui_screen_off(void)
{
    ui_post(&(ui_msg_t){ .type = UI_SCREEN_OFF });
}

bool ui_asleep(void)
{
    return ui_sleep_level() > 0;
}

void ui_pwr_down(void)
{
    ui_post(&(ui_msg_t){ .type = UI_PWR_RING, .i = 1 });
}

void ui_pwr_up(void)
{
    ui_post(&(ui_msg_t){ .type = UI_PWR_RING, .i = 0 });
}

// ---- Init

static void lvgl_log_cb(lv_log_level_t level, const char *buf)
{
    (void)level;
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        n--;
    }
    ESP_LOGW("LVGL", "%.*s", (int)n, buf);
}

static void indev_setup(void)
{
    // 400 ms tap/hold threshold (2.3), 100 ms repeat, 10 px scroll limit, 10 % throw (7.1)
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) {
            continue;
        }
        lv_indev_set_long_press_time(in, 400);
        lv_indev_set_long_press_repeat_time(in, 100);
        lv_indev_set_scroll_limit(in, 10);
        lv_indev_set_scroll_throw(in, 10);
    }
}

esp_err_t ui_init(lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_ARG, TAG, "no display");
    if (s_ready) {
        return ESP_OK;
    }
    s_q = xQueueCreate(UI_Q_DEPTH, sizeof(ui_msg_t));
    ESP_RETURN_ON_FALSE(s_q, ESP_ERR_NO_MEM, TAG, "queue");
    s_typed_mx = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_typed_mx, ESP_ERR_NO_MEM, TAG, "mutex");
    ui_work_init();
    cm_init();

    // first frame already matches the saved settings; the app keeps them in sync through ui_post()
    char buf[12];
    settings_get_str("ui_lang", buf, sizeof(buf));
    ui_str_set_lang(strcmp(buf, "en") == 0);
    settings_get_str("tr_lang_a", buf, sizeof(buf));
    if (buf[0]) {
        snprintf(g_lang_a, sizeof(g_lang_a), "%.3s", buf);
    }
    settings_get_str("tr_lang_b", buf, sizeof(buf));
    if (buf[0]) {
        snprintf(g_lang_b, sizeof(g_lang_b), "%.3s", buf);
    }
    int32_t mode = settings_get_int("mode");
    int32_t text_only = settings_get_int("text_only");

    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_log_register_print_cb(lvgl_log_cb);
#if LV_USE_PERF_MONITOR
    lv_sysmon_performance_pause(NULL);      // log mode keeps running otherwise; "perf on" resumes it
#endif
    indev_setup();
    ui_theme_init();
    g_s_state = ui_subj_int(UI_ST_IDLE, 0, UI_ST__N - 1);
    g_s_mode = ui_subj_int(mode ? 1 : 0, 0, 1);
    g_s_text_only = ui_subj_int(text_only ? 1 : 0, 0, 1);
    // speakers are now connected on the bench board; the app posts UI_SET_SPEAKER 0 once a stored
    // "no speaker" flag exists (onboarding O4 / S -> Динамик)
    g_s_speaker = ui_subj_int(1, 0, 1);
    g_s_fault = ui_subj_int(UI_FAULT_NONE, 0, UI_FAULT__N - 1);
    g_s_net = ui_subj_int(UI_NET_CONNECTING, 0, UI_NET_FAILED);
    g_s_upload = ui_subj_int(0, 0, 100);
    g_s_hints = ui_subj_int(3, 0, 3);
    g_s_portal = ui_subj_int(0, 0, 255);
    g_s_tr_dir = ui_subj_int(0, 0, 2);
    g_s_face = ui_subj_int(1, 0, 1);            // "Лицо шара: Вкл" by default (ui.md open question 3)

    lv_obj_t *home = scr_home_create();
    s_nav[0] = (nav_t){ .scr = home, .scroller = NULL, .id = UI_SCR_HOME };
    s_depth = 1;
    lv_screen_load(home);
    ui_rim_init();
    scr_home_resume();
    ui_orb_intro(scr_home_orb());           // S0 boot splash (A31) on the permanent Home orb
    lv_timer_create(pump_cb, PUMP_MS, NULL);
    ui_sleep_init();
    ui_onboard_check();                     // O1 once the system is up, if Wi-Fi or the key is missing
    ctx_update();
    s_ready = true;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "ready (lang %s, mode %d)", ui_str_lang() ? "en" : "ru", (int)mode);
    return ESP_OK;
}

// ---- Inspection (console; call under the lock)

lv_obj_t *ui_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    lv_obj_t *o = lv_obj_find_by_name(lv_screen_active(), name);
    return o ? o : lv_obj_find_by_name(lv_layer_top(), name);
}

typedef void (*walk_fn_t)(lv_obj_t *obj, void *ctx);

static void walk(lv_obj_t *obj, walk_fn_t fn, void *ctx)
{
    if (lv_obj_get_name(obj)) {
        fn(obj, ctx);
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        walk(lv_obj_get_child(obj, (int32_t)i), fn, ctx);
    }
}

void ui_walk_named(walk_fn_t fn, void *ctx)
{
    walk(lv_screen_active(), fn, ctx);
    walk(lv_layer_top(), fn, ctx);
}

const char *ui_obj_type(const lv_obj_t *obj)
{
    const lv_obj_class_t *c = lv_obj_get_class(obj);
    return c && c->name ? c->name : "obj";
}

bool ui_obj_visible(lv_obj_t *obj)
{
    for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP) {
            return false;
        }
    }
    return true;
}

// a &= b; false when empty (lv_area_intersect is private in 9.5)
static bool area_and(lv_area_t *a, const lv_area_t *b)
{
    a->x1 = LV_MAX(a->x1, b->x1);
    a->y1 = LV_MAX(a->y1, b->y1);
    a->x2 = LV_MIN(a->x2, b->x2);
    a->y2 = LV_MIN(a->y2, b->y2);
    return a->x1 <= a->x2 && a->y1 <= a->y2;
}

// Visible part of obj: its coords clipped by every ancestor (scrolled-out messages drop out)
bool ui_obj_visible_area(lv_obj_t *obj, lv_area_t *out)
{
    lv_obj_get_coords(obj, out);
    for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        if (!area_and(out, &pa)) {
            return false;
        }
    }
    return true;
}

// Rmax of what obj draws (ui.md 0.4): circle / rounded rect from its style radius
float ui_obj_rmax(lv_obj_t *obj, const lv_area_t *a)
{
    int32_t w = lv_area_get_width(a), h = lv_area_get_height(a);
    int32_t r = lv_obj_get_style_radius(obj, LV_PART_MAIN);
    if (r > LV_MIN(w, h) / 2) {
        r = LV_MIN(w, h) / 2;
    }
    return ui_geom_rmax_rrect(a->x1, a->y1, a->x2 + 1, a->y2 + 1, r);
}

static size_t json_str(char *out, size_t len, const char *s)
{
    size_t o = 0;
    if (len < 3) {
        return 0;
    }
    out[o++] = '"';
    for (; s && *s && o + 7 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, len - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o++] = '"';
    out[o] = 0;
    return o;
}

typedef struct {
    char *out;
    size_t len, o;
    int count;
} jbuf_t;

static void jb_printf(jbuf_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_printf(jbuf_t *j, const char *fmt, ...)
{
    if (j->o >= j->len) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->out + j->o, j->len - j->o, fmt, ap);
    va_end(ap);
    if (n > 0) {
        j->o = LV_MIN(j->o + (size_t)n, j->len - 1);
    }
}

static void tree_visit(lv_obj_t *obj, void *ctx)
{
    jbuf_t *j = ctx;
    char name[UI_NAME_MAX];
    lv_obj_get_name_resolved(obj, name, sizeof(name));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    char flags[4] = { 0 };
    int f = 0;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        flags[f++] = 'C';
    }
    if (!ui_obj_visible(obj)) {
        flags[f++] = 'H';
    }
    jb_printf(j, "%s{\"name\":", j->count++ ? "," : "");
    if (j->o < j->len) {
        j->o += json_str(j->out + j->o, j->len - j->o, name);
    }
    jb_printf(j, ",\"type\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"radius\":%d,\"flags\":\"%s\"",
              ui_obj_type(obj), (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a),
              (int)lv_obj_get_style_radius(obj, LV_PART_MAIN), flags);
    if (lv_obj_check_type(obj, &lv_label_class)) {
        jb_printf(j, ",\"text\":");
        if (j->o < j->len) {
            j->o += json_str(j->out + j->o, j->len - j->o, lv_label_get_text(obj));
        }
    }
    jb_printf(j, "}");
}

int ui_tree_json(char *out, size_t len)
{
    if (!out || len < 3) {
        return 0;
    }
    jbuf_t j = { out, len, 0, 0 };
    jb_printf(&j, "[");
    ui_walk_named(tree_visit, &j);
    jb_printf(&j, "]");
    return (int)j.o;
}

typedef struct {
    jbuf_t jb;
    int bad;
} audit_t;

static void audit_add(audit_t *au, lv_obj_t *obj, const char *rule, float value)
{
    char name[UI_NAME_MAX];
    lv_obj_get_name_resolved(obj, name, sizeof(name));
    jb_printf(&au->jb, "%s{\"name\":\"%s\",\"rule\":\"%s\",\"value\":%.1f}", au->bad++ ? "," : "", name, rule,
              (double)value);
}

static bool draws_something(lv_obj_t *obj)
{
    if (lv_obj_check_type(obj, &lv_label_class)) {
        return lv_label_get_text(obj) && lv_label_get_text(obj)[0];
    }
    // the orb is a custom-drawn transparent box: check its box like a rectangle
    return lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) > LV_OPA_MIN ||
           lv_obj_get_style_border_width(obj, LV_PART_MAIN) > 0 ||
           lv_obj_get_style_outline_width(obj, LV_PART_MAIN) > 0 || lv_obj_has_flag(obj, LV_OBJ_FLAG_ADV_HITTEST);
}

static void audit_visit(lv_obj_t *obj, void *ctx)
{
    audit_t *au = ctx;
    if (!ui_obj_visible(obj) || obj == lv_screen_active()) {
        return;
    }
    if (lv_obj_get_style_clip_corner(obj, LV_PART_MAIN)) {
        audit_add(au, obj, "clip_corner", 1);
    }
    lv_area_t a;
    // the arc title draws along r 148 inside its box, and the rim draws at r <= 168 by construction;
    // transparent hit zones may extend past the circle (0.4); list rows fade out towards the rim on purpose
    // (3.7: fully opaque only within 79 px of the centre line)
    bool fading = ui_list_is_row(obj) && lv_obj_get_style_opa(obj, LV_PART_MAIN) < LV_OPA_COVER;
    bool exempt = lv_obj_check_type(obj, &lv_arclabel_class) || obj == ui_rim_obj() || !draws_something(obj) ||
                  fading;
    if (!exempt && ui_obj_visible_area(obj, &a)) {
        float r = ui_obj_rmax(obj, &a);
        if (!ui_geom_fits(r)) {
            audit_add(au, obj, "rmax", r);
        }
    }
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        lv_area_t c;
        lv_obj_get_click_area(obj, &c);
        int32_t w = lv_area_get_width(&c), h = lv_area_get_height(&c);
        if (w < UI_MIN_HIT || h < UI_MIN_HIT) {
            audit_add(au, obj, "hit", (float)LV_MIN(w, h));
        }
    }
}

int ui_audit_json(char *out, size_t len)
{
    if (!out || len < 3) {
        return 0;
    }
    audit_t au = { { out, len, 0, 0 }, 0 };
    jb_printf(&au.jb, "[");
    ui_walk_named(audit_visit, &au);
    jb_printf(&au.jb, "]");
    return au.bad;
}

// ---- Snapshot inside taskLVGL

typedef struct {
    const char *name;
    lv_draw_buf_t *dst;
    SemaphoreHandle_t done;
    esp_err_t err;
} snap_t;

static void snap_cb(void *arg)
{
    snap_t *s = arg;
    lv_obj_t *obj = s->name ? ui_find(s->name) : lv_screen_active();
    s->err = !obj ? ESP_ERR_NOT_FOUND
                  : (lv_snapshot_take_to_draw_buf(obj, LV_COLOR_FORMAT_RGB565, s->dst) == LV_RESULT_OK ? ESP_OK
                                                                                                    : ESP_FAIL);
    xSemaphoreGive(s->done);
}

esp_err_t ui_snapshot(const char *obj_name, lv_draw_buf_t *dst)
{
    ESP_RETURN_ON_FALSE(dst, ESP_ERR_INVALID_ARG, TAG, "no buffer");
    snap_t s = { obj_name, dst, xSemaphoreCreateBinary(), ESP_FAIL };
    ESP_RETURN_ON_FALSE(s.done, ESP_ERR_NO_MEM, TAG, "sem");
    if (!ui_lock(1000)) {
        vSemaphoreDelete(s.done);
        return ESP_ERR_TIMEOUT;
    }
    lv_async_call(snap_cb, &s);
    ui_unlock();
    esp_err_t err = xSemaphoreTake(s.done, pdMS_TO_TICKS(2000)) == pdTRUE ? s.err : ESP_ERR_TIMEOUT;
    if (err == ESP_ERR_TIMEOUT && ui_lock(1000)) {
        lv_async_call_cancel(snap_cb, &s);      // never let taskLVGL touch the stack frame later
        ui_unlock();
    }
    vSemaphoreDelete(s.done);
    return err;
}
