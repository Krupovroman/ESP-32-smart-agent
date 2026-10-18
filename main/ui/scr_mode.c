// M (mode sheet), L (translator languages), LP (language picker) and the direction list of docs/ui.md 3.7.
#include "ui_internal.h"
#include "ui_list.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_work.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

static void upper(const char *in, char *out, size_t len)
{
    size_t i = 0;
    for (; in[i] && i + 1 < len && i < 3; i++) {
        out[i] = (char)toupper((unsigned char)in[i]);
    }
    out[i] = 0;
}

static void pair_text(char *out, size_t len)
{
    ui_lang_pair(out, len);
}

static void dir_text(int dir, char *out, size_t len)
{
    char a[4], b[4];
    upper(g_lang_a, a, sizeof(a));
    upper(g_lang_b, b, sizeof(b));
    if (dir == 1) {
        snprintf(out, len, "%s " TXT_ARROW " %s", a, b);
    } else if (dir == 2) {
        snprintf(out, len, "%s " TXT_ARROW " %s", b, a);
    } else {
        ui_str_copy(out, len, ui_s(S_AUTO));
    }
}

// Voice replies / speak translations (the left button on H does the same, 5.2)
static void toggle_voice(lv_obj_t *row)
{
    if (!ui_get(g_s_speaker)) {
        ui_toast(ui_s(S_C_SPK_T));
        return;
    }
    int32_t to = !ui_get(g_s_text_only);
    ui_set(g_s_text_only, to);
    ui_emit(UI_IN_TEXT_ONLY, to);
    ui_save_int("text_only", to);
    ui_toast(ui_s(to ? S_VOICE_OFF : S_VOICE_ON));
    if (row) {
        ui_row_set_toggle(row, !to);
    }
}

// ---- M

static lv_obj_t *s_m_scr, *s_m_fault, *s_m_langs, *s_m_dir, *s_m_volume, *s_m_voice;
static char *s_draft;           // typed question kept after a cancel (4.6)

static void set_mode(int m)
{
    if (m != ui_get(g_s_mode)) {
        ui_set(g_s_mode, m);    // colour morph A21 and the chat of that mode
        ui_emit(UI_IN_SET_MODE, m);
        ui_save_int("mode", m);
    }
    ui_nav_home();
}

static void ask_done(const char *text, bool ok, void *ctx)
{
    (void)ctx;
    heap_caps_free(s_draft);
    s_draft = NULL;
    if (!ok) {
        size_t n = strlen(text) + 1;
        s_draft = n > 1 ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL;
        if (s_draft) {
            memcpy(s_draft, text, n);
        }
        return;
    }
    ui_nav_home();
    if (!ui_send_typed(text)) {
        ui_toast(ui_s(S_NOT_YET));
    }
}

static void m_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0: set_mode(0); break;
    case 1: set_mode(1); break;
    case 2: ui_nav_push(UI_SCR_LANGS, 0); break;
    case 3: ui_nav_push(UI_SCR_DIR, 0); break;
    case 4:
        ui_text_open(&(ui_text_req_t){ .field = UI_FIELD_QUESTION, .prompt = ui_s(S_K_QUESTION), .init = s_draft,
                                       .done = ask_done });
        break;
    case 5: ui_nav_push(UI_SCR_HIST, 0); break;
    case 6:
        // "Новый разговор": context and the chat of this mode go (A15r)
        ui_emit(UI_IN_NEW_CHAT, 0);
        ui_nav_home();
        scr_home_clear(true);
        break;
    case 7: ui_nav_push(UI_SCR_ADJUST, 0); break;
    case 8: toggle_voice(row); break;
    case 9: ui_nav_push(UI_SCR_SETTINGS, 0); break;
    case 10: ui_open(UI_OPEN_FAULT_FIX); break;     // back returns here
    default: break;
    }
}

static void m_refresh(void)
{
    char buf[64];
    if (s_m_fault) {
        // back from the fixing screen: a fault that is gone leaves M too
        int32_t f = ui_fault_shown();
        bool gone = f == UI_FAULT_NONE;
        if (gone != lv_obj_has_flag(s_m_fault, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_set_flag(s_m_fault, LV_OBJ_FLAG_HIDDEN, gone);
            ui_list_refresh(lv_obj_get_parent(s_m_fault));
        }
        if (!gone) {
            ui_row_set_title(s_m_fault, ui_fault_text(f));
            ui_row_set_sub_accent(s_m_fault, ui_fault_warning(f) ? UI_ACC_WARNING : UI_ACC_ERROR);
        }
    }
    if (s_m_langs) {
        pair_text(buf, sizeof(buf));
        ui_row_set_sub(s_m_langs, buf);
        dir_text(ui_get(g_s_tr_dir), buf, sizeof(buf));
        ui_row_set_sub(s_m_dir, buf);
    }
    snprintf(buf, sizeof(buf), "%d %%", (int)ui_cfg_int("volume"));
    ui_row_set_sub(s_m_volume, buf);
    ui_row_set_toggle(s_m_voice, !ui_get(g_s_text_only) && ui_get(g_s_speaker));
}

static void m_loaded_cb(lv_event_t *e)
{
    (void)e;
    m_refresh();
}

static void m_delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s_m_scr) {
        s_m_scr = s_m_fault = s_m_langs = s_m_dir = s_m_volume = s_m_voice = NULL;
    }
}

lv_obj_t *scr_mode_build(uint32_t arg)
{
    (void)arg;
    lv_obj_t *list;
    s_m_scr = ui_list_screen("mode", ui_s(S_T_MODE), &list);
    lv_obj_add_event_cb(s_m_scr, m_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_m_scr, m_delete_cb, LV_EVENT_DELETE, NULL);
    int mode = ui_get(g_s_mode);
    char buf[64];
    // the pill leads here also with a fault: its fixing screen first, the rest of M stays reachable (3.14). No
    // icon, so "Модель истекает" fits the 180 px title; m_refresh sets the text
    s_m_fault = NULL;
    if (ui_fault_shown() != UI_FAULT_NONE) {
        s_m_fault = ui_list_row(list, "m_fault", NULL, "", ui_s(S_M_FIX), UI_ROW_CHEVRON, m_cb, (void *)10);
    }
    lv_obj_t *r = ui_list_row(list, "m_assistant", SYM_COMMENT, ui_s(S_MODE_ASSISTANT), ui_s(S_M_ASSISTANT_SUB),
                              UI_ROW_CHECK, m_cb, (void *)0);
    ui_row_set_check(r, mode == 0);
    pair_text(buf, sizeof(buf));
    r = ui_list_row(list, "m_translator", SYM_LANGUAGE, ui_s(S_MODE_TRANSLATOR), buf, UI_ROW_CHECK, m_cb,
                    (void *)1);
    ui_row_set_check(r, mode == 1);
    s_m_langs = s_m_dir = NULL;
    if (mode == 1) {
        s_m_langs = ui_list_row(list, "m_langs", SYM_GLOBE, ui_s(S_T_LANGS), "", UI_ROW_CHEVRON, m_cb, (void *)2);
        s_m_dir = ui_list_row(list, "m_dir", SYM_EXCHANGE, ui_s(S_T_DIR), "", UI_ROW_CHEVRON, m_cb, (void *)3);
    }
    ui_list_row(list, "m_type", SYM_KEYBOARD, ui_s(S_M_TYPE), NULL, UI_ROW_CHEVRON, m_cb, (void *)4);
    ui_list_row(list, "m_history", SYM_HISTORY, ui_s(S_TITLE_HISTORY), NULL, UI_ROW_CHEVRON, m_cb, (void *)5);
    ui_list_row(list, "m_new", SYM_SYNC, ui_s(S_NEW_CHAT), ui_s(S_M_NEW_SUB), UI_ROW_NONE, m_cb, (void *)6);
    s_m_volume = ui_list_row(list, "m_volume", SYM_VOLUME, ui_s(S_VOLUME), "", UI_ROW_CHEVRON, m_cb, (void *)7);
    s_m_voice = ui_list_row(list, "m_voice", SYM_VOLUME, ui_s(mode ? S_VOICE_TR : S_VOICE_REPLY), NULL,
                            UI_ROW_TOGGLE, m_cb, (void *)8);
    ui_list_row(list, "m_settings", SYM_COG, ui_s(S_T_SETTINGS), NULL, UI_ROW_CHEVRON, m_cb, (void *)9);
    m_refresh();
    ui_list_refresh(list);
    return s_m_scr;
}

// ---- L: my language, the other person, direction

static lv_obj_t *s_l_scr, *s_l_a, *s_l_b, *s_l_dir;

static void l_refresh(void)
{
    char buf[48];
    ui_row_set_sub(s_l_a, ui_lang_name(g_lang_a));
    ui_row_set_sub(s_l_b, ui_lang_name(g_lang_b));
    dir_text(ui_get(g_s_tr_dir), buf, sizeof(buf));
    ui_row_set_sub(s_l_dir, buf);
}

static void l_cb(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    ui_nav_push(i == 2 ? UI_SCR_DIR : UI_SCR_LANG_PICK, (uint32_t)i);
}

static void l_loaded_cb(lv_event_t *e)
{
    (void)e;
    l_refresh();
}

static void l_delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s_l_scr) {
        s_l_scr = s_l_a = s_l_b = s_l_dir = NULL;
    }
}

lv_obj_t *scr_langs_build(uint32_t arg)
{
    (void)arg;
    lv_obj_t *list;
    s_l_scr = ui_list_screen("langs", ui_s(S_T_LANGS), &list);
    lv_obj_add_event_cb(s_l_scr, l_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_l_scr, l_delete_cb, LV_EVENT_DELETE, NULL);
    s_l_a = ui_list_row(list, "l_a", SYM_GLOBE, ui_s(S_T_LANG_A), "", UI_ROW_CHEVRON, l_cb, (void *)0);
    s_l_b = ui_list_row(list, "l_b", SYM_GLOBE, ui_s(S_T_LANG_B), "", UI_ROW_CHEVRON, l_cb, (void *)1);
    s_l_dir = ui_list_row(list, "l_dir", SYM_EXCHANGE, ui_s(S_T_DIR), "", UI_ROW_CHEVRON, l_cb, (void *)2);
    l_refresh();
    ui_list_refresh(list);
    return s_l_scr;
}

// ---- LP: language picker for slot A (arg 0) or B (arg 1)

static int s_lp_slot;

static void lp_cb(lv_event_t *e)
{
    const char *code = ui_lang_code((int)(intptr_t)lv_event_get_user_data(e));
    const char *cur = s_lp_slot ? g_lang_b : g_lang_a;
    const char *other = s_lp_slot ? g_lang_a : g_lang_b;
    if (strcmp(code, cur) != 0) {
        char a[4], b[4];
        // picking the language of the other slot swaps the two (3.7)
        if (!strcmp(code, other)) {
            snprintf(a, sizeof(a), "%s", g_lang_b);
            snprintf(b, sizeof(b), "%s", g_lang_a);
        } else {
            snprintf(a, sizeof(a), "%s", s_lp_slot ? g_lang_a : code);
            snprintf(b, sizeof(b), "%s", s_lp_slot ? code : g_lang_b);
        }
        ui_set_langs(a, b);
    }
    ui_nav_back();
}

lv_obj_t *scr_lang_pick_build(uint32_t arg)
{
    s_lp_slot = arg ? 1 : 0;
    lv_obj_t *list;
    lv_obj_t *scr = ui_list_screen("langpick", ui_s(s_lp_slot ? S_T_LANG_B : S_T_LANG_A), &list);
    const char *cur = s_lp_slot ? g_lang_b : g_lang_a;
    const char *other = s_lp_slot ? g_lang_a : g_lang_b;
    lv_obj_t *sel = NULL;
    for (int i = 0; i < ui_lang_count(); i++) {
        char name[16], sub[48];
        const char *code = ui_lang_code(i);
        snprintf(name, sizeof(name), "lp_%s", code);
        upper(code, sub, sizeof(sub));
        if (!strcmp(code, other)) {
            // the other slot's language: picking it swaps the pair
            size_t n = strlen(sub);
            snprintf(sub + n, sizeof(sub) - n, " " TXT_MIDDOT " %s", ui_s(s_lp_slot ? S_T_LANG_A : S_T_LANG_B));
        }
        lv_obj_t *r = ui_list_row(list, name, NULL, ui_lang_name(code), sub, UI_ROW_CHECK, lp_cb, (void *)(intptr_t)i);
        if (!strcmp(code, cur)) {
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

// ---- Direction: auto / A -> B / B -> A (RAM only, the pill shows it)

static void dir_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    ui_set(g_s_tr_dir, d);
    ui_emit(UI_IN_TR_DIR, d);
    ui_nav_back();
}

lv_obj_t *scr_dir_build(uint32_t arg)
{
    (void)arg;
    lv_obj_t *list;
    lv_obj_t *scr = ui_list_screen("dir", ui_s(S_T_DIR), &list);
    static const char *const names[3] = { "dir_auto", "dir_ab", "dir_ba" };
    for (int d = 0; d < 3; d++) {
        char buf[32];
        dir_text(d, buf, sizeof(buf));
        lv_obj_t *r = ui_list_row(list, names[d], NULL, buf, NULL, UI_ROW_CHECK, dir_cb, (void *)(intptr_t)d);
        ui_row_set_check(r, ui_get(g_s_tr_dir) == d);
    }
    ui_list_refresh(list);
    return scr;
}
