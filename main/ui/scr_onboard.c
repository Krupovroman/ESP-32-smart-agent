// O1-O5 first boot (docs/ui.md 5.7) on the step template: language, network, key, speaker, done. Shown once after
// boot when no Wi-Fi network is saved or no API key is set. The speaker step plays nothing (no chimes): it asks.
#include "ui_internal.h"
#include "ui_orb.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_work.h"
#include "devconsole.h"
#include "net.h"
#include "settings.h"

#include <string.h>

#define CHECK_MS        500
#define CHECK_MAX_MS    60000

static bool s_active;
static int s_step;
static bool s_wifi_ok, s_key_ok, s_speaker_done;
static ui_step_t s_st;
static lv_timer_t *s_check;
static uint32_t s_check_t0;

bool ui_onboarding(void)
{
    // only while a step is still on the stack (BOOT or UI_GOTO home ends the flow)
    return s_active && ui_nav_has(UI_SCR_ONBOARD);
}

static bool key_set(void)
{
    char k[16];
    return s_key_ok || ui_cfg_str("ai_key", k, sizeof(k)) > 0;
}

static int next_step(void)
{
    if (!s_wifi_ok) {
        return 2;
    }
    if (!key_set()) {
        return 3;
    }
    return s_speaker_done ? 5 : 4;
}

void ui_onboard_resume(void)
{
    // the flow of the current step (W -> K -> WC, Q, K hex) succeeded. Wi-Fi is up when W / WC or Q finish; the key
    // is read back (key_set: the pending or stored value), because Q also finishes when only Wi-Fi data arrived
    if (s_step == 2) {
        s_wifi_ok = true;
    }
    ui_nav_home();
    ui_nav_push(UI_SCR_ONBOARD, (uint32_t)next_step());
}

// ---- Boot check: after the self-test (console ready), off taskLVGL because net_status talks to the driver

typedef struct {
    bool wifi, key;
} check_job_t;

static void check_job(void *arg)
{
    check_job_t *j = arg;
    net_status_t st;
    net_status(&st);
    j->wifi = st.saved > 0 || st.state == NET_UP;
    j->key = settings_is_set("ai_key");
}

static void check_done(void *arg)
{
    check_job_t *j = arg;
    if (j->wifi && j->key) {
        return;
    }
    s_wifi_ok = j->wifi;
    s_key_ok = j->key;
    s_active = true;
    if (ui_nav_current() == UI_SCR_HOME) {
        ui_nav_push(UI_SCR_ONBOARD, 1);
    }
}

static void check_cb(lv_timer_t *t)
{
    // safe mode never becomes ready: no onboarding there
    if (!devconsole_is_ready()) {
        if (lv_tick_elaps(s_check_t0) > CHECK_MAX_MS) {
            lv_timer_delete(t);
            s_check = NULL;
        }
        return;
    }
    lv_timer_delete(t);
    s_check = NULL;
    check_job_t *j = ui_work_alloc(sizeof(check_job_t));
    if (j) {
        ui_work(check_job, check_done, j);
    }
}

void ui_onboard_check(void)
{
    if (!s_check) {
        s_check_t0 = lv_tick_get();
        s_check = lv_timer_create(check_cb, CHECK_MS, NULL);
    }
}

// ---- Steps

static bool back_cb(void)
{
    // PWR / back = previous step; on O1 it leaves the flow to H (the only way out without finishing it, so a
    // device without a key is never stuck in the onboarding)
    if (s_step > 1) {
        ui_nav_replace(UI_SCR_ONBOARD, (uint32_t)(s_step - 1));
        return true;
    }
    s_active = false;
    return false;
}

static void loaded_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_set_back(back_cb);
}

static void key_done(const char *text, bool ok, void *ctx)
{
    (void)ctx;
    if (ok && text[0]) {
        ui_save_str("ai_key", text);
        ui_save_flush();        // the AI client reads the stored key: the first try must not use the old one
        ui_onboard_resume();
    }
}

static void choose_lang(int lang)
{
    ui_save_str("ui_lang", lang ? "en" : "ru");
    s_step = 2;     // the stack is dropped by the language switch: continue on top of the new H
    if (lang != ui_str_lang()) {
        ui_apply_lang(lang);
        ui_nav_push(UI_SCR_ONBOARD, (uint32_t)next_step());
    } else {
        ui_nav_replace(UI_SCR_ONBOARD, (uint32_t)next_step());
    }
}

static void set_speaker(bool on)
{
    ui_set_speaker(on);     // saved; "Есть динамик" also turns voice replies on, "Нет динамика" off (5.2)
    s_speaker_done = true;
    ui_nav_replace(UI_SCR_ONBOARD, 5);
}

static void btn_cb(lv_event_t *e)
{
    bool primary = (intptr_t)lv_event_get_user_data(e) == 1;
    switch (s_step) {
    case 1: choose_lang(primary ? 0 : 1); break;
    case 2:
    case 3:
        if (primary) {
            ui_nav_push(UI_SCR_QR, 0);      // Wi-Fi, key and model from one phone page
        } else if (s_step == 2) {
            ui_nav_push(UI_SCR_WIFI, 0);
        } else {
            ui_text_open(&(ui_text_req_t){ .field = UI_FIELD_KEY_HEX, .prompt = ui_s(S_K_KEY), .done = key_done });
        }
        break;
    case 4: set_speaker(primary); break;
    default:
        s_active = false;
        ui_set(g_s_hints, 3);
        ui_nav_home();
        break;
    }
}

lv_obj_t *scr_onboard_build(uint32_t arg)
{
    s_step = arg >= 1 && arg <= 5 ? (int)arg : 1;
    s_active = true;
    lv_obj_t *scr = ui_step_screen("onboard", &s_st, s_step, btn_cb);
    lv_obj_add_event_cb(scr, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    switch (s_step) {
    case 1:
        ui_step_set(&s_st, ui_s(S_O1_T), ui_s(S_O1_S), ui_s(S_O1_P), ui_s(S_O1_B));
        ui_orb_intro(s_st.orb);     // the mini orb opens its eyes and glances left and right
        break;
    case 2: ui_step_set(&s_st, ui_s(S_O2_T), ui_s(S_O2_S), ui_s(S_W_PHONE), ui_s(S_O2_B)); break;
    case 3: ui_step_set(&s_st, ui_s(S_O3_T), ui_s(S_O3_S), ui_s(S_W_PHONE), ui_s(S_O3_B)); break;
    case 4: ui_step_set(&s_st, ui_s(S_O4_T), ui_s(S_O4_S), ui_s(S_O4_P), ui_s(S_O4_B)); break;
    default:
        ui_step_set(&s_st, ui_s(S_O5_T), ui_s(S_O5_S), ui_s(S_O5_P), NULL);
        ui_orb_set_expr(s_st.orb, EXPR_HAPPY, 0);
        break;
    }
    return scr;
}
