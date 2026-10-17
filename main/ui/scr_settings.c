// S "Настройки" (docs/ui.md 3.7) and its "Экран и сон" sub-list. Values shown in the subtitles are refreshed
// every time the list is shown again; the Wi-Fi status comes from the worker (net_status talks to the driver).
#include "ui_chat.h"
#include "ui_internal.h"
#include "ui_list.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_work.h"
#include "net.h"

#include <stdio.h>
#include <string.h>

enum {
    R_WIFI, R_PHONE, R_MODEL, R_VOICE, R_TEXT_ONLY, R_SPEAKER, R_VOLUME, R_BRIGHT, R_DISPLAY, R_FACE, R_UILANG,
    R_TRANSLATOR, R_PROVIDER, R_HISTORY, R_ABOUT, R__N
};

static lv_obj_t *s_scr, *s_rows[R__N];
static uint32_t s_gen;

typedef struct {
    uint32_t gen;
    net_status_t st;
} status_job_t;

static void status_job(void *arg)
{
    status_job_t *j = arg;
    net_status(&j->st);
}

static void status_done(void *arg)
{
    status_job_t *j = arg;
    if (!s_scr || j->gen != s_gen) {
        return;
    }
    const char *sub = j->st.state == NET_UP ? j->st.ssid
                      : j->st.state == NET_CONNECTING ? ui_s(S_S_CONNECTING) : ui_s(S_S_NOT_CONNECTED);
    ui_row_set_sub(s_rows[R_WIFI], sub);
}

static void dim_text(int32_t s, char *out, size_t len)
{
    char v[24];
    if (s <= 0) {
        ui_str_copy(out, len, ui_s(S_S_DIM_NEVER));
        return;
    }
    if (s < 60) {
        snprintf(v, sizeof(v), ui_s(S_SEC_FMT), (int)s);
    } else {
        snprintf(v, sizeof(v), ui_s(S_MIN_FMT), (int)(s / 60));
    }
    snprintf(out, len, ui_s(S_S_DIM_FMT), v);
}

static void refresh(void)
{
    char buf[112], tmp[100];
    ui_cfg_str("ai_model", tmp, sizeof(tmp));
    ui_short_model(tmp, buf, sizeof(buf));
    ui_row_set_sub(s_rows[R_MODEL], buf);
    ui_cfg_str("tts_voice", tmp, sizeof(tmp));
    ui_short_voice(tmp, buf, sizeof(buf));
    ui_row_set_sub(s_rows[R_VOICE], buf);
    ui_row_set_toggle(s_rows[R_TEXT_ONLY], !ui_get(g_s_text_only) && ui_get(g_s_speaker));
    ui_row_set_sub(s_rows[R_SPEAKER], ui_s(ui_get(g_s_speaker) ? S_S_SPK_YES : S_S_SPK_NO));
    snprintf(buf, sizeof(buf), "%d %%", (int)ui_cfg_int("volume"));
    ui_row_set_sub(s_rows[R_VOLUME], buf);
    snprintf(buf, sizeof(buf), "%d %%", (int)ui_cfg_int("bright"));
    ui_row_set_sub(s_rows[R_BRIGHT], buf);
    dim_text(ui_cfg_int("dim_s"), buf, sizeof(buf));
    ui_row_set_sub(s_rows[R_DISPLAY], buf);
    ui_row_set_toggle(s_rows[R_FACE], ui_get(g_s_face) != 0);
    ui_lang_pair(buf, sizeof(buf));
    ui_row_set_sub(s_rows[R_TRANSLATOR], buf);
    ui_cfg_str("ai_url", tmp, sizeof(tmp));
    ui_url_host(tmp, buf, sizeof(buf));
    ui_row_set_sub(s_rows[R_PROVIDER], buf);
    status_job_t *j = ui_work_alloc(sizeof(status_job_t));
    if (j) {
        j->gen = s_gen;
        ui_work(status_job, status_done, j);
    }
}

static void clear_history(void *ctx)
{
    (void)ctx;
    int m = ui_get(g_s_mode);
    cm_clear(1 - m);
    scr_home_clear(false);
    ui_emit(UI_IN_NEW_CHAT, 0);
    ui_toast(ui_s(S_H_CLEARED));
}

static void row_cb(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e)) {
    case R_WIFI: ui_nav_push(UI_SCR_WIFI, 0); break;
    case R_PHONE: ui_nav_push(UI_SCR_QR, 0); break;
    case R_MODEL: ui_nav_push(UI_SCR_MODELS, 0); break;
    case R_VOICE: ui_nav_push(UI_SCR_VOICE, 0); break;
    case R_TEXT_ONLY: {
        if (!ui_get(g_s_speaker)) {
            ui_toast(ui_s(S_C_SPK_T));
            break;
        }
        int32_t to = !ui_get(g_s_text_only);
        ui_set(g_s_text_only, to);
        ui_emit(UI_IN_TEXT_ONLY, to);
        ui_save_int("text_only", to);
        refresh();
        break;
    }
    case R_SPEAKER: {
        int32_t to = !ui_get(g_s_speaker);
        ui_set(g_s_speaker, to);
        ui_emit(UI_IN_SPEAKER, to);
        refresh();
        break;
    }
    case R_VOLUME: ui_nav_push(UI_SCR_ADJUST, 0); break;
    case R_BRIGHT: ui_nav_push(UI_SCR_ADJUST, 1); break;
    case R_DISPLAY: ui_nav_push(UI_SCR_DISPLAY, 0); break;
    case R_FACE:
        ui_set(g_s_face, !ui_get(g_s_face));
        refresh();
        break;
    case R_UILANG: {
        int to = !ui_str_lang();
        ui_save_str("ui_lang", to ? "en" : "ru");
        ui_apply_lang(to);                      // drops the stack and rebuilds H
        ui_nav_push(UI_SCR_SETTINGS, 0);        // stay here, now in the other language
        break;
    }
    case R_TRANSLATOR: ui_nav_push(UI_SCR_LANGS, 0); break;
    case R_PROVIDER: ui_nav_push(UI_SCR_PROVIDER, 0); break;
    case R_HISTORY:
        ui_dialog_open(&(ui_dialog_t){ .title = ui_s(S_H_CLEAR_Q), .body = ui_s(S_H_CLEAR_B),
                                       .confirm = ui_s(S_H_DELETE), .danger = true, .on_confirm = clear_history });
        break;
    case R_ABOUT: ui_nav_push(UI_SCR_ABOUT, 0); break;
    default: break;
    }
}

static void loaded_cb(lv_event_t *e)
{
    (void)e;
    refresh();
}

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s_scr) {
        s_scr = NULL;
        s_gen++;
        memset(s_rows, 0, sizeof(s_rows));
    }
}

lv_obj_t *scr_settings_build(uint32_t arg)
{
    (void)arg;
    static const struct {
        const char *name, *icon;
        ui_sid_t title;
        ui_row_right_t right;
    } defs[R__N] = {
        [R_WIFI] = { "s_wifi", SYM_WIFI, S_T_WIFI, UI_ROW_CHEVRON },
        [R_PHONE] = { "s_phone", SYM_QRCODE, S_T_PHONE, UI_ROW_CHEVRON },
        [R_MODEL] = { "s_model", SYM_ROBOT, S_T_MODELS, UI_ROW_CHEVRON },
        [R_VOICE] = { "s_voice", SYM_PLAY, S_T_VOICE, UI_ROW_CHEVRON },
        [R_TEXT_ONLY] = { "s_textonly", SYM_COMMENT, S_VOICE_REPLY, UI_ROW_TOGGLE },
        [R_SPEAKER] = { "s_speaker", SYM_VOLUME, S_S_SPEAKER, UI_ROW_NONE },
        [R_VOLUME] = { "s_volume", SYM_VOLUME, S_VOLUME, UI_ROW_CHEVRON },
        [R_BRIGHT] = { "s_bright", SYM_SUN, S_BRIGHT, UI_ROW_CHEVRON },
        [R_DISPLAY] = { "s_display", SYM_MOBILE, S_T_DISPLAY, UI_ROW_CHEVRON },
        [R_FACE] = { "s_face", SYM_EYE, S_S_FACE, UI_ROW_TOGGLE },
        [R_UILANG] = { "s_uilang", SYM_GLOBE, S_S_UILANG, UI_ROW_NONE },
        [R_TRANSLATOR] = { "s_translator", SYM_LANGUAGE, S_MODE_TRANSLATOR, UI_ROW_CHEVRON },
        [R_PROVIDER] = { "s_provider", SYM_KEY, S_T_PROVIDER, UI_ROW_CHEVRON },
        [R_HISTORY] = { "s_history", SYM_TRASH, S_TITLE_HISTORY, UI_ROW_NONE },
        [R_ABOUT] = { "s_about", SYM_INFO, S_T_ABOUT, UI_ROW_CHEVRON },
    };
    lv_obj_t *list;
    s_gen++;
    s_scr = ui_list_screen("settings", ui_s(S_T_SETTINGS), &list);
    lv_obj_add_event_cb(s_scr, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, delete_cb, LV_EVENT_DELETE, NULL);
    for (int i = 0; i < R__N; i++) {
        bool has_sub = defs[i].right != UI_ROW_TOGGLE;
        s_rows[i] = ui_list_row(list, defs[i].name, defs[i].icon, ui_s(defs[i].title), has_sub ? "" : NULL,
                                defs[i].right, row_cb, (void *)(intptr_t)i);
    }
    ui_row_set_sub(s_rows[R_PHONE], ui_s(S_S_PHONE_SUB));
    ui_row_set_sub(s_rows[R_UILANG], ui_s(S_LANG_SELF));
    ui_row_set_sub(s_rows[R_HISTORY], ui_s(S_S_CLEAR));
    ui_row_set_sub(s_rows[R_WIFI], ui_s(S_LOADING));
    ui_row_set_sub(s_rows[R_ABOUT], NULL);
    ui_list_refresh(list);
    return s_scr;
}

// ---- "Экран и сон": dim after (the ambient clock follows 1 min later, the screen goes off 5 min after that)

static void dim_cb(lv_event_t *e)
{
    ui_save_int("dim_s", (int32_t)(intptr_t)lv_event_get_user_data(e));
    ui_nav_back();
}

lv_obj_t *scr_display_build(uint32_t arg)
{
    (void)arg;
    static const int32_t vals[] = { 15, 30, 60, 120, 300, 0 };
    lv_obj_t *list;
    lv_obj_t *scr = ui_list_screen("display", ui_s(S_T_DISPLAY), &list);
    int32_t cur = ui_cfg_int("dim_s");
    lv_obj_t *sel = NULL;
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        char name[16], title[24];
        snprintf(name, sizeof(name), "dsp_%d", (int)vals[i]);
        if (vals[i] == 0) {
            ui_str_copy(title, sizeof(title), ui_s(S_NEVER));
        } else if (vals[i] < 60) {
            snprintf(title, sizeof(title), ui_s(S_SEC_FMT), (int)vals[i]);
        } else {
            snprintf(title, sizeof(title), ui_s(S_MIN_FMT), (int)(vals[i] / 60));
        }
        lv_obj_t *r = ui_list_row(list, name, NULL, title, NULL, UI_ROW_CHECK, dim_cb, (void *)(intptr_t)vals[i]);
        if (vals[i] == cur) {
            ui_row_set_check(r, true);
            sel = r;
        }
    }
    ui_list_refresh(list);
    if (sel) {
        ui_list_focus(sel);
    }
    return scr;
}
