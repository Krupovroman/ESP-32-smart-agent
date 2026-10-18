// Home H / H' (docs/ui.md 3.2-3.4, 3.14, 5.1-5.5): pill or status line, orb, two side buttons (voice /
// settings, or the language chips in translator mode), first-use hint, and the chat of the current mode.
#include "ui_chat.h"
#include "ui_geom.h"
#include "ui_internal.h"
#include "ui_orb.h"
#include "ui_rim.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// ---- Geometry (all checked against Rmax <= 168 in ui.md 3.2 / 3.3; repeated here where derived)
#define PILL_X          110     // stadium x 110-250 (140x50); fault pill x 100-260
#define PILL_W          140
#define PILL_FAULT_X    100
#define PILL_FAULT_W    160
#define PILL_H          50
#define PILL_Y_EMPTY    36      // centres (135,61)/(225,61): |(45,119)| + 25 = 152.2
#define PILL_Y_CONV     24      // |(45,131)| + 25 = 163.5; fault |(55,131)| + 25 = 167.1
#define STATUS_H        20      // ui_16 line
#define STATUS_Y_EMPTY  51      // x 80-280, y 51-71: |(100,129)| = 163.2
#define STATUS_X_EMPTY  80
#define STATUS_W_EMPTY  200
#define STATUS_Y_CONV   39      // x 90-270, y 39-59: |(90,141)| = 167.3
#define STATUS_X_CONV   90
#define STATUS_W_CONV   180
#define SIDE_D          52      // circles at (100,292) / (260,292): |(80,112)| + 26 = 163.6
#define SIDE_L_CX       100
#define SIDE_R_CX       260
#define SIDE_CY         292
#define HINT_W1         100     // line 1 x 130-230, y 296-316 (between the buttons, 4 px gaps): 144.9
#define HINT_W2         124     // line 2 x 118-242, y 316-336 (below the buttons, they end at y 318):
                                // |(62,156)| = 167.9
#define HINT_Y1         296
#define HINT_Y2         316
#define HINT_H          20
#define CHAT_X          54      // viewport x 54-306, y 80-244: top corners |(126,100)| = 160.9
#define CHAT_Y          80
#define CHAT_W          252
#define CHAT_H          164
#define FADE_TOP_H      24      // y 80-104
#define FADE_BOT_H      16      // y 228-244
#define CHAT_GAP        6       // between a user bubble and its reply (turn gap = 6 + 8)

static lv_obj_t *s_scr, *s_pill, *s_pill_lbl, *s_status, *s_orb;
static lv_obj_t *s_left, *s_left_lbl, *s_right, *s_right_lbl, *s_wdot;
static lv_obj_t *s_hint1, *s_hint2, *s_chat;
static bool s_conv;                 // conversation layout (>= 1 message)
static bool s_autoscroll = true;
static uint32_t s_scroll_end_t;
static uint32_t s_press_t;          // orb press time
static bool s_orb_pressed, s_orb_ptt, s_cancel_armed;
static int32_t s_chip_side = -1;    // chip currently pressed
static bool s_chip_talk;            // that press became a recording (>= 400 ms)
static int s_target_side = -1;      // translator: who the device speaks to
static uint32_t s_rec_t0, s_state_t0;
static char s_trans[96];            // transient status text (toast, "Не расслышал", retry, error)
static uint32_t s_trans_until;
static bool s_trans_error;
static lv_timer_t *s_tick;          // 100 ms: rim RECORD, countdown, thinking stages, transient expiry
static lv_timer_t *s_err_timer;     // ERROR -> IDLE / OFFLINE after 2.4 s
static lv_timer_t *s_chip_timer;    // 400 ms chip hold
static lv_timer_t *s_swap_timer;    // A21: side buttons <-> chips swap at 50 %
static lv_anim_t *s_layout_anim;
static int32_t s_layout_v;          // 0 = empty hero layout, 460 = conversation layout
static char s_status_txt[96];
static bool s_status_err;           // s_status_txt is an error (coral)

static int mode(void) { return ui_get(g_s_mode); }
static int32_t state(void) { return ui_get(g_s_state); }

static bool net_down(void)
{
    int32_t n = ui_get(g_s_net);
    return n == UI_NET_OFF || n == UI_NET_FAILED;
}

// No network, or the app put the orb into OFFLINE: hollow orb and the "Нет Wi-Fi ›" pill together (5.3)
static bool offline(void)
{
    return net_down() || ui_get(g_s_state) == UI_ST_OFFLINE;
}

static bool idle_like(int32_t st)
{
    return st == UI_ST_IDLE || st == UI_ST_ANSWERED || st == UI_ST_OFFLINE;
}

// The orb shows the app state, plus two UI-local refinements: offline while idle, and the cancel-armed
// look while the finger is outside the cancel radius (ui.md 5.1).
static int32_t orb_state(void)
{
    int32_t st = state();
    if (st == UI_ST_IDLE && offline()) {
        return UI_ST_OFFLINE;
    }
    if (s_cancel_armed && (st == UI_ST_REC || st == UI_ST_REC_LATCHED)) {
        return UI_ST_REC_CANCEL;
    }
    return st;
}

static void upper(const char *in, char *out)
{
    int i = 0;
    for (; in[i] && i < 3; i++) {
        out[i] = (char)toupper((unsigned char)in[i]);
    }
    out[i] = 0;
}

// ---- Fades (A5/A13 chrome, A22 text swap); style opa never creates a layer

static void opa_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa(var, (lv_opa_t)v, 0);
}

static void hide_done(lv_anim_t *a)
{
    lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void fade(lv_obj_t *o, bool show, uint32_t ms)
{
    bool hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t *running = lv_anim_get(o, opa_exec);
    if (running && running->end_value == (show ? LV_OPA_COVER : LV_OPA_TRANSP)) {
        return;     // already heading there: restarting every refresh would never finish the fade
    }
    lv_anim_delete(o, opa_exec);
    if (show && hidden) {
        lv_obj_set_style_opa(o, 0, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else if (!show && hidden) {
        return;
    }
    lv_opa_t from = lv_obj_get_style_opa(o, LV_PART_MAIN);
    lv_opa_t to = show ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (from == to) {
        if (!show) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, show ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    if (!show) {
        lv_anim_set_completed_cb(&a, hide_done);
    }
    lv_anim_start(&a);
}

// Text and colour change together, while the label is invisible (the old text never flashes coral)
static void status_apply(void)
{
    lv_obj_set_style_text_color(s_status, ui_col(s_status_err ? ui_accent(UI_ACC_ERROR)->top : UI_COL_TEXT2), 0);
    lv_label_set_text(s_status, s_status_txt);
}

static void status_in(lv_anim_t *a)
{
    // A22 second half: swap the text while invisible, then fade the new one in (or stay hidden when the
    // status was cleared during the fade-out)
    (void)a;
    if (!s_status_txt[0]) {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    status_apply();
    fade(s_status, true, 160);
}

static void status_set(const char *txt, bool error)
{
    if (!txt || !txt[0]) {
        s_status_txt[0] = 0;
        fade(s_status, false, 100);
        return;
    }
    // compare the stored (possibly truncated) form, or an over-long text would restart A22 on every tick
    char t[sizeof(s_status_txt)];
    ui_str_copy(t, sizeof(t), txt);
    bool hidden = lv_obj_has_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(t, s_status_txt) == 0 && error == s_status_err && !hidden) {
        return;
    }
    bool shown = !hidden && s_status_txt[0];
    memcpy(s_status_txt, t, sizeof(t));
    s_status_err = error;
    if (!shown) {
        status_apply();
        fade(s_status, true, 160);
        return;
    }
    lv_anim_delete(s_status, opa_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_status);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, lv_obj_get_style_opa(s_status, LV_PART_MAIN), 0);
    lv_anim_set_duration(&a, 100);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, status_in);
    lv_anim_start(&a);
}

// ---- Pill, side buttons, hint

static void pill_refresh(void)
{
    char buf[128], a[4], b[4];
    int32_t f = ui_fault_shown();
    bool fault = f != UI_FAULT_NONE;
    if (fault) {
        const ui_accent_t *acc = ui_accent(ui_fault_warning(f) ? UI_ACC_WARNING : UI_ACC_ERROR);
        snprintf(buf, sizeof(buf), "#%06lX %s " TXT_RSAQUO "#", (unsigned long)acc->top, ui_fault_text(f));
    } else if (mode() == 1) {
        // auto "RU ⇄ EN"; a forced direction shows "RU → EN" / "EN → RU" so it is never hidden (5.5)
        int32_t dir = ui_get(g_s_tr_dir);
        bool b_first = dir == 2;
        upper(b_first ? g_lang_b : g_lang_a, a);
        upper(b_first ? g_lang_a : g_lang_b, b);
        unsigned long ca = (unsigned long)ui_accent(b_first ? UI_ACC_LANG_B : UI_ACC_LANG_A)->top;
        unsigned long cb = (unsigned long)ui_accent(b_first ? UI_ACC_LANG_A : UI_ACC_LANG_B)->top;
        snprintf(buf, sizeof(buf), "#%06lX %s# %s #%06lX %s# " SYM_DOWN, ca, a, dir ? TXT_ARROW : SYM_EXCHANGE, cb, b);
    } else {
        snprintf(buf, sizeof(buf), "%s " SYM_DOWN, ui_s(S_MODE_ASSISTANT));
    }
    lv_label_set_text(s_pill_lbl, buf);
    // fault texts are longer than mode names: ui_16 keeps "Ключ не принят ›" inside the 160 px pill
    lv_obj_set_style_text_font(s_pill_lbl, fault ? &ui_16 : &ui_20, 0);
    lv_obj_set_x(s_pill, fault ? PILL_FAULT_X : PILL_X);
    lv_obj_set_width(s_pill, fault ? PILL_FAULT_W : PILL_W);
}

static void side_refresh(void)
{
    char a[4], b[4];
    lv_obj_remove_style(s_left, &st_chip, 0);
    lv_obj_remove_style(s_left, &st_chip_a, 0);
    lv_obj_remove_style(s_left, &st_chip_a_pr, LV_STATE_PRESSED);
    lv_obj_remove_style(s_left, &st_chip_hold, LV_STATE_CHECKED);
    lv_obj_remove_style(s_right, &st_chip, 0);
    lv_obj_remove_style(s_right, &st_chip_b, 0);
    lv_obj_remove_style(s_right, &st_chip_b_pr, LV_STATE_PRESSED);
    lv_obj_remove_style(s_right, &st_chip_hold, LV_STATE_CHECKED);
    // the same two objects; names follow their role so "ui tap home_chip_a" works in translator mode
    lv_obj_set_name_static(s_left, mode() == 1 ? "home_chip_a" : "home_voice");
    lv_obj_set_name_static(s_right, mode() == 1 ? "home_chip_b" : "home_settings");
    if (mode() == 1) {
        lv_obj_add_style(s_left, &st_chip, 0);
        lv_obj_add_style(s_left, &st_chip_a, 0);
        lv_obj_add_style(s_left, &st_chip_a_pr, LV_STATE_PRESSED);
        lv_obj_add_style(s_left, &st_chip_hold, LV_STATE_CHECKED);
        lv_obj_add_style(s_right, &st_chip, 0);
        lv_obj_add_style(s_right, &st_chip_b, 0);
        lv_obj_add_style(s_right, &st_chip_b_pr, LV_STATE_PRESSED);
        lv_obj_add_style(s_right, &st_chip_hold, LV_STATE_CHECKED);
        upper(g_lang_a, a);
        upper(g_lang_b, b);
        lv_label_set_text(s_left_lbl, a);
        lv_label_set_text(s_right_lbl, b);
        lv_obj_add_flag(s_wdot, LV_OBJ_FLAG_HIDDEN);
    } else {
        bool mute = ui_get(g_s_text_only) || !ui_get(g_s_speaker);
        lv_label_set_text_static(s_left_lbl, mute ? SYM_MUTE : SYM_VOLUME);
        lv_label_set_text_static(s_right_lbl, SYM_COG);
        lv_obj_set_flag(s_wdot, LV_OBJ_FLAG_HIDDEN, ui_get(g_s_speaker) != 0);
    }
}

static void hint_refresh(void)
{
    bool show = ui_get(g_s_hints) > 0 && !s_conv && idle_like(orb_state());
    if (show) {
        char buf[40], a[4], b[4];
        if (mode() == 1) {
            upper(g_lang_a, a);
            upper(g_lang_b, b);
            snprintf(buf, sizeof(buf), ui_s(S_HINT2_TR), a, b);
            lv_label_set_text_static(s_hint1, ui_s(S_HINT1_TR));
            lv_label_set_text(s_hint2, buf);
        } else {
            lv_label_set_text_static(s_hint1, ui_s(S_HINT1));
            lv_label_set_text_static(s_hint2, ui_s(S_HINT2));
        }
    }
    fade(s_hint1, show, UI_T_FAST);
    fade(s_hint2, show, UI_T_FAST);
}

// ---- Status line contents per state (5.1 / 5.3)

static void status_refresh(void)
{
    uint32_t now = lv_tick_get();
    int32_t st = orb_state();
    char buf[96];
    const char *txt = NULL;
    bool err = false;
    if (s_trans[0] && (int32_t)(s_trans_until - now) > 0) {
        txt = s_trans;
        err = s_trans_error;
    } else {
        s_trans[0] = 0;
        uint32_t rec_el = now - s_rec_t0;
        switch (st) {
        case UI_ST_REC:
        case UI_ST_REC_LATCHED:
            if (rec_el >= 25000) {
                // whole seconds left, rounded up: 5 at 25.0 s ... 1 during the last second
                snprintf(buf, sizeof(buf), ui_s(S_ST_LEFT_S), (int)((30000 - LV_MIN(rec_el, 29999) + 999) / 1000));
                txt = buf;
            } else if (st == UI_ST_REC_LATCHED) {
                txt = ui_s(S_ST_LATCHED);
            } else if (s_chip_talk && s_chip_side >= 0) {
                char c[4];
                upper(s_chip_side == UI_SIDE_A ? g_lang_a : g_lang_b, c);
                snprintf(buf, sizeof(buf), ui_s(S_ST_LISTENING_LANG), c);
                txt = buf;
            } else {
                txt = ui_s(S_ST_LISTENING);
            }
            break;
        case UI_ST_REC_CANCEL: txt = ui_s(S_ST_CANCEL); break;
        case UI_ST_UPLOAD:
            snprintf(buf, sizeof(buf), ui_s(S_ST_UPLOAD), (int)ui_get(g_s_upload));
            txt = buf;
            break;
        case UI_ST_THINK: {
            uint32_t el = now - s_state_t0;
            txt = ui_s(el < 6000 ? S_ST_THINK : el < 20000 ? S_ST_THINK2 : S_ST_THINK3);
            break;
        }
        case UI_ST_SPEAK: txt = ui_get(g_s_hints) > 0 ? ui_s(S_ST_SPEAK_HINT) : NULL; break;
        default: break;
        }
    }
    bool pill = idle_like(st) && !txt;
    fade(s_pill, pill, pill ? 200 : UI_T_FAST);
    // settings hides while busy (A5); chip B always stays (hold to talk, tap to stop speech)
    bool right = mode() == 1 || idle_like(st);
    fade(s_right, right, right ? 200 : UI_T_FAST);
    status_set(txt, err);
}

static void transient(const char *txt, uint32_t ms, bool error)
{
    ui_str_copy(s_trans, sizeof(s_trans), txt);
    s_trans_until = lv_tick_get() + ms;
    s_trans_error = error;
    lv_timer_resume(s_tick);
    status_refresh();
}

static void tick_cb(lv_timer_t *t)
{
    int32_t st = state();
    bool rec = st == UI_ST_REC || st == UI_ST_REC_LATCHED;
    if (rec) {
        uint32_t el = lv_tick_get() - s_rec_t0;
        ui_rim_record((int32_t)LV_MIN(el, 30000), el >= 25000);
    }
    status_refresh();
    if (!rec && st != UI_ST_THINK && st != UI_ST_UPLOAD && !s_trans[0]) {
        lv_timer_pause(t);
    }
}

// ---- Layout: empty hero <-> conversation (A15 / A15r)

static void layout_exec(void *var, int32_t v)
{
    (void)var;
    s_layout_v = v;
    float u = v >= 320 ? 1.0f : (float)v / 320.0f;
    u = u < 0.5f ? 4 * u * u * u : 1 - (-2 * u + 2) * (-2 * u + 2) * (-2 * u + 2) / 2;
    lv_obj_set_y(s_pill, PILL_Y_EMPTY + (int32_t)((PILL_Y_CONV - PILL_Y_EMPTY) * u));
    lv_obj_set_pos(s_status, STATUS_X_EMPTY + (int32_t)((STATUS_X_CONV - STATUS_X_EMPTY) * u),
                   STATUS_Y_EMPTY + (int32_t)((STATUS_Y_CONV - STATUS_Y_EMPTY) * u));
    lv_obj_set_width(s_status, STATUS_W_EMPTY + (int32_t)((STATUS_W_CONV - STATUS_W_EMPTY) * u));
    // chat 280-460 ms ease_out
    int32_t c = v <= 280 ? 0 : (v - 280) * 255 / 180;
    float cu = (float)c / 255.0f;
    cu = 1.0f - (1.0f - cu) * (1.0f - cu) * (1.0f - cu);
    lv_obj_set_style_opa(s_chat, (lv_opa_t)(cu * 255.0f), 0);
    lv_obj_set_flag(s_chat, LV_OBJ_FLAG_HIDDEN, v <= 280);
    // the reverse run moves the orb back once the chat has faded (460 -> 320 ms)
    if (!s_conv && v <= 320 && ui_orb_get_preset(s_orb) != ORB_HERO) {
        ui_orb_set_preset(s_orb, ORB_HERO, true);
    }
}

// The rim follows the chat only while H is the active screen (HIST / RD own it otherwise)
static void rim_follow_chat(void)
{
    if (ui_nav_current() == UI_SCR_HOME) {
        ui_rim_attach_scroll(s_conv ? s_chat : NULL);
    }
}

static void layout_done(lv_anim_t *a)
{
    (void)a;
    s_layout_anim = NULL;
    rim_follow_chat();
    hint_refresh();
}

static void set_layout(bool conv, bool anim)
{
    if (conv == s_conv && !anim) {
        return;
    }
    s_conv = conv;
    if (s_layout_anim) {
        lv_anim_delete(&s_layout_v, layout_exec);
        s_layout_anim = NULL;
    }
    if (conv) {
        ui_orb_set_preset(s_orb, ORB_COMPACT, anim);
        fade(s_hint1, false, 100);
        fade(s_hint2, false, 100);
    } else {
        rim_follow_chat();      // the thumb goes with the chat
    }
    int32_t target = conv ? 460 : 0;
    if (!anim || s_layout_v == target) {
        if (!conv) {
            ui_orb_set_preset(s_orb, ORB_HERO, anim);
        }
        layout_exec(&s_layout_v, target);
        layout_done(NULL);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_layout_v);
    lv_anim_set_exec_cb(&a, layout_exec);
    lv_anim_set_values(&a, s_layout_v, target);
    lv_anim_set_duration(&a, (uint32_t)LV_ABS(target - s_layout_v));
    lv_anim_set_completed_cb(&a, layout_done);
    s_layout_anim = lv_anim_start(&a);
}

// ---- Chat

static void scroll_bottom(void)
{
    if (!s_autoscroll || !s_conv) {
        return;
    }
    lv_obj_update_layout(s_chat);
    int32_t bottom = lv_obj_get_scroll_bottom(s_chat);
    if (bottom > 0) {
        lv_obj_scroll_to_y(s_chat, lv_obj_get_scroll_y(s_chat) + bottom, LV_ANIM_ON);   // A20
    }
}

// A card or translator bubble taller than the unfaded window (y 104-228, 124 px: a two-line card title, a long
// translation in ui_28) shows its top first, just below the top fade, instead of sitting at the bottom with its
// first line under the fade; the rest is a scroll down away.
static void scroll_reveal(lv_obj_t *w)
{
    if (!s_autoscroll || !s_conv) {
        return;
    }
    lv_obj_update_layout(s_chat);
    int32_t pad = lv_obj_get_style_pad_top(w, LV_PART_MAIN);
    if (lv_obj_get_height(w) - pad <= CHAT_H - FADE_TOP_H - FADE_BOT_H) {
        scroll_bottom();
        return;
    }
    // y is relative to the content box (below the 24 px fade padding); without the A16 appear offset still running
    int32_t y = lv_obj_get_y(w) - lv_obj_get_style_translate_y(w, LV_PART_MAIN) + pad;
    lv_obj_scroll_to_y(s_chat, y, LV_ANIM_ON);
}

// After new content: translator bubbles and cards keep their top in view, everything else follows the bottom
static void scroll_for(lv_obj_t *w, const cm_entry_t *e)
{
    if (e->kind == CM_TURN || e->kind == CM_CARD) {
        scroll_reveal(w);
    } else {
        scroll_bottom();
    }
}

static void chat_click_cb(lv_event_t *e)
{
    (void)e;
    if (lv_tick_elaps(s_scroll_end_t) < 300) {
        return;     // the tap stopped a fling
    }
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_t *w = ui_msg_at(s_chat, p);
    if (!w) {
        return;
    }
    uint32_t id = ui_msg_id(w);
    ui_orb_look_at(s_orb, p, 900);
    const cm_entry_t *en = cm_find(id);
    if (en && en->kind == CM_TURN && en->text2) {
        ui_nav_push(UI_SCR_READER, id);
    } else {
        ui_nav_push(UI_SCR_HIST, id);
    }
}

static void chat_scroll_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL_BEGIN) {
        // A manual scroll suspends auto-scroll until the next turn. Only a finger drag counts: LVGL also
        // sends SCROLL_BEGIN without an anim for LV_ANIM_OFF scrolls and for the readjust after the oldest
        // message is trimmed, and those must not switch auto-scroll off.
        lv_indev_t *in = lv_indev_active();
        if (in && lv_indev_get_scroll_obj(in) == s_chat) {
            s_autoscroll = false;
        }
    } else if (code == LV_EVENT_SCROLL_END) {
        s_scroll_end_t = lv_tick_get();
    }
}

static lv_obj_t *add_widget(const cm_entry_t *e, uint32_t delay)
{
    if (!s_conv) {
        set_layout(true, true);
    }
    lv_obj_t *w = ui_msg_create(s_chat, e, true, delay, NULL);
    while (lv_obj_get_child_count(s_chat) > CM_LIVE_MAX) {
        lv_obj_delete(lv_obj_get_child(s_chat, 0));
    }
    // new message: the face glances down for 1 s
    ui_orb_look_at(s_orb, (lv_point_t){ 180, 360 }, 1000);
    scroll_for(w, e);
    return w;
}

static void rebuild_chat(void)
{
    lv_obj_clean(s_chat);
    int m = mode();
    int n = cm_count(m);
    for (int i = n > CM_LIVE_MAX ? n - CM_LIVE_MAX : 0; i < n; i++) {
        ui_msg_create(s_chat, cm_at(m, i), false, 0, NULL);
    }
    set_layout(n > 0, false);
    s_autoscroll = true;
    lv_obj_update_layout(s_chat);
    lv_obj_scroll_to_y(s_chat, lv_obj_get_scroll_y(s_chat) + lv_obj_get_scroll_bottom(s_chat), LV_ANIM_OFF);
}

static void refresh_entry(const cm_entry_t *e, bool animate)
{
    lv_obj_t *w = ui_msg_find(s_chat, e->id);
    if (w) {
        ui_msg_refresh(w, e, animate);
        scroll_for(w, e);   // a translation that just grew the bubble past the window keeps its first line
    }
}

static void close_open_reply(int m)
{
    cm_entry_t *last = cm_count(m) ? cm_at(m, cm_count(m) - 1) : NULL;
    if (last && last->kind == CM_BOT) {
        last->pending = false;      // "pending" on a reply = still streaming
    }
}

static void set_side(cm_entry_t *e, int32_t side)
{
    // translator: the source side picks the codes; the translation goes to the other language
    e->side = (int8_t)(side == UI_SIDE_A || side == UI_SIDE_B ? side : UI_SIDE_AUTO);
    bool b = e->side == UI_SIDE_B;
    snprintf(e->src, sizeof(e->src), "%s", b ? g_lang_b : g_lang_a);
    snprintf(e->dst, sizeof(e->dst), "%s", b ? g_lang_a : g_lang_b);
    s_target_side = e->side == UI_SIDE_AUTO ? -1 : (b ? UI_SIDE_A : UI_SIDE_B);
}

void scr_home_user(const char *text, int32_t side)
{
    int m = mode();
    cm_entry_t *e = text ? cm_last_pending(m) : NULL;
    if (!e) {
        close_open_reply(m);
        e = cm_add(m, m == 1 ? CM_TURN : CM_USER);
        e->new_turn = cm_count(m) > 1;
        e->pending = text == NULL;
        if (text) {
            cm_set(&e->text, text);
        }
        if (m == 1) {
            set_side(e, text ? side : UI_SIDE_AUTO);
        }
        add_widget(e, 0);
        return;
    }
    cm_set(&e->text, text);
    e->pending = false;
    if (m == 1) {
        set_side(e, side);
    }
    refresh_entry(e, true);     // A17 crossfade, A18 slide to the speaker's side
}

void scr_home_bot(const char *text, bool tts_failed)
{
    int m = mode();
    if (m == 1) {
        cm_entry_t *e = cm_last_of(m, CM_TURN);
        if (!e) {
            e = cm_add(m, CM_TURN);
            e->new_turn = cm_count(m) > 1;
            snprintf(e->src, sizeof(e->src), "%s", g_lang_a);
            snprintf(e->dst, sizeof(e->dst), "%s", g_lang_b);
            cm_set(&e->text2, text);
            e->pending = false;
            add_widget(e, 0);
            return;
        }
        cm_set(&e->text2, text);
        e->pending = false;
        e->tts_failed = tts_failed;
        refresh_entry(e, true);
        return;
    }
    cm_entry_t *last = cm_count(m) ? cm_at(m, cm_count(m) - 1) : NULL;
    if (last && last->kind == CM_BOT && last->pending) {
        cm_set(&last->text, text);
        last->pending = false;
        last->tts_failed = tts_failed;
        refresh_entry(last, false);
        return;
    }
    cm_entry_t *e = cm_add(m, CM_BOT);
    cm_set(&e->text, text);
    e->tts_failed = tts_failed;
    add_widget(e, 80);      // the reply follows its question 80 ms later (A16)
}

void scr_home_bot_append(const char *chunk)
{
    int m = mode();
    if (m == 1) {
        cm_entry_t *e = cm_last_of(m, CM_TURN);
        if (e) {
            cm_append(&e->text2, chunk);
            refresh_entry(e, false);
        }
        return;
    }
    cm_entry_t *last = cm_count(m) ? cm_at(m, cm_count(m) - 1) : NULL;
    if (last && last->kind == CM_BOT && last->pending) {
        cm_append(&last->text, chunk);
        refresh_entry(last, false);
        return;
    }
    cm_entry_t *e = cm_add(m, CM_BOT);
    cm_set(&e->text, chunk);
    e->pending = true;      // reply still streaming
    add_widget(e, 80);
}

void scr_home_wave(const uint8_t *env32)
{
    cm_entry_t *e = cm_last_pending(mode());
    if (e && env32) {
        memcpy(e->env, env32, sizeof(e->env));
        e->has_env = true;
        lv_obj_t *w = ui_msg_find(s_chat, e->id);
        if (w) {
            lv_obj_invalidate(w);
        }
    }
}

static void err_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (state() == UI_ST_ERROR) {
        ui_set(g_s_state, offline() ? UI_ST_OFFLINE : UI_ST_IDLE);
    }
}

// ERROR lasts 2.4 s (then IDLE or OFFLINE) and shows a one-line coral text for the card just added; the
// card title itself may wrap
static void error_begin(void)
{
    if (!s_err_timer) {
        s_err_timer = lv_timer_create(err_timer_cb, 2400, NULL);
        lv_timer_set_auto_delete(s_err_timer, false);
    }
    lv_timer_set_repeat_count(s_err_timer, 1);
    lv_timer_reset(s_err_timer);
    lv_timer_resume(s_err_timer);
    const cm_entry_t *c = cm_last_of(mode(), CM_CARD);
    static const ui_sid_t shorts[UI_CARD__N] = { S_E_GENERIC, S_F_NO_WIFI, S_F_BAD_KEY, S_F_NO_CREDIT, S_F_REGION,
                                                 S_F_MODEL, S_E_RATE, S_E_SERVER, S_E_TLS, S_E_MIC, S_E_SPK };
    transient(ui_s(c && c->card < UI_CARD__N ? shorts[c->card] : S_E_GENERIC), 2400, true);
}

static void add_card(int32_t card, const char *body)
{
    // the first card switches an empty H to the conversation layout (3.14)
    int m = mode();
    close_open_reply(m);
    cm_entry_t *e = cm_add(m, CM_CARD);
    e->card = (uint8_t)(card >= 0 && card < UI_CARD__N ? card : UI_CARD_GENERIC);
    e->new_turn = cm_count(m) > 1;
    if (body) {
        cm_set(&e->text, body);
    }
    add_widget(e, 0);
}

void scr_home_error(int32_t card, const char *body)
{
    add_card(card, body);
    if (state() == UI_ST_ERROR) {
        error_begin();      // a second error restarts the 2.4 s and shows its own short text
    } else {
        ui_set(g_s_state, UI_ST_ERROR);
    }
}

void scr_home_clear(bool anim)
{
    int m = mode();
    cm_clear(m);
    lv_obj_clean(s_chat);
    s_autoscroll = true;
    set_layout(false, anim);
    hint_refresh();
}

void scr_home_divider(const char *hhmm)
{
    // 3.3: "— новый разговор · 15:10 —" after 15 min idle; nothing to separate in an empty chat
    int m = mode();
    if (cm_count(m) == 0) {
        return;
    }
    close_open_reply(m);
    char buf[96];
    snprintf(buf, sizeof(buf), ui_s(S_DIVIDER), hhmm ? hhmm : "");
    cm_entry_t *e = cm_add(m, CM_DIVIDER);
    e->new_turn = true;
    cm_set(&e->text, buf);
    add_widget(e, 0);
}

void scr_home_toast(const char *text)
{
    transient(text, 2000, false);
}

void scr_home_status_text(const char *text, uint32_t ms)
{
    transient(text, ms, false);
}

void scr_home_retry(int32_t n)
{
    char buf[48];
    snprintf(buf, sizeof(buf), ui_s(S_ST_RETRY), (int)n);
    transient(buf, 3000, false);
}

void scr_home_mood(int32_t mood)
{
    ui_orb_set_mood(s_orb, mood);
}

lv_obj_t *scr_home_orb(void) { return s_orb; }

void scr_home_langs_changed(void)
{
    pill_refresh();
    side_refresh();
    hint_refresh();
}

void scr_home_resume(void)
{
    rim_follow_chat();
}

// ---- Input: orb (5.1), side buttons / chips (5.2, 5.5), pill

static void orb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int32_t st = state();
    uint32_t now = lv_tick_get();
    if (code == LV_EVENT_PRESSED) {
        s_orb_pressed = true;
        s_cancel_armed = false;
        s_press_t = now;
        s_orb_ptt = !(st == UI_ST_ERROR || orb_state() == UI_ST_OFFLINE);
        if (!s_orb_ptt) {
            return;
        }
        if (st == UI_ST_UPLOAD || st == UI_ST_THINK) {
            ui_orb_pulse(s_orb);
            if (ui_get(g_s_hints) > 0) {
                transient(ui_s(S_ST_BUSY_HINT), 1500, false);
            }
        }
        s_autoscroll = true;
        scroll_bottom();
        if (!ui_emit(UI_IN_PTT_DOWN, UI_SIDE_AUTO)) {
            ui_set(g_s_state, UI_ST_REC);   // no app yet: show the listening orb with the live mic
        }
    } else if (code == LV_EVENT_PRESSING && s_orb_ptt) {
        if (st != UI_ST_REC && st != UI_ST_REC_LATCHED) {
            return;
        }
        lv_point_t p, c;
        lv_indev_get_point(lv_indev_active(), &p);
        ui_orb_center(s_orb, &c);
        int32_t dx = p.x - c.x, dy = p.y - c.y, r = ui_orb_cancel_radius(s_orb);
        bool out = dx * dx + dy * dy > r * r;
        if (out != s_cancel_armed) {
            s_cancel_armed = out;
            ui_orb_set_state(s_orb, orb_state());
            status_refresh();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_orb_pressed) {
            return;
        }
        s_orb_pressed = false;
        if (!s_orb_ptt) {
            return;
        }
        uint32_t held = now - s_press_t;
        bool cancel = s_cancel_armed;
        s_cancel_armed = false;
        bool app = ui_emit(cancel ? UI_IN_PTT_CANCEL : UI_IN_PTT_UP, (int32_t)held);
        if (!app) {
            ui_set(g_s_state, UI_ST_IDLE);
            if (cancel) {
                transient(ui_s(S_ST_CANCELLED), 1500, false);
            }
        } else {
            ui_orb_set_state(s_orb, orb_state());
            status_refresh();
        }
    } else if (code == LV_EVENT_CLICKED && !s_orb_ptt) {
        if (st == UI_ST_ERROR) {
            if (!ui_emit(UI_IN_DISMISS, 0)) {
                ui_set(g_s_state, offline() ? UI_ST_OFFLINE : UI_ST_IDLE);
            }
        } else {
            ui_open(UI_OPEN_WIFI);      // the hollow OFFLINE orb opens W (5.3)
        }
    }
}

static void chip_hold_cb(lv_timer_t *t)
{
    (void)t;
    if (s_chip_side < 0) {
        return;
    }
    // held >= 400 ms: record with the source forced to this chip (5.5)
    s_chip_talk = true;
    lv_obj_add_state(s_chip_side == UI_SIDE_A ? s_left : s_right, LV_STATE_CHECKED);
    if (!ui_emit(UI_IN_PTT_DOWN, s_chip_side)) {
        ui_set(g_s_state, UI_ST_REC);
    }
    status_refresh();
}

static void toggle_voice(void)
{
    if (!ui_get(g_s_speaker)) {
        // no speaker: replies are always text; the card explains and offers the sound test (5.2)
        add_card(UI_CARD_NO_SPEAKER, NULL);
        return;
    }
    int32_t to = !ui_get(g_s_text_only);
    if (!ui_emit(UI_IN_TEXT_ONLY, to)) {
        ui_set(g_s_text_only, to);
    }
    ui_toast(ui_s(to ? S_VOICE_OFF : S_VOICE_ON));
    ui_orb_set_expr(s_orb, to ? EXPR_SLEEPY : EXPR_WIDE, 300);     // "shh" / eyes widen
}

// The pointer is still on obj's touch area (for releases of PRESS_LOCK objects)
static bool released_on(lv_obj_t *obj)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) {
        return false;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_click_area(obj, &a);
    return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

static void side_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    int side = btn == s_left ? UI_SIDE_A : UI_SIDE_B;
    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        ui_orb_look_at(s_orb, p, 900);
    }
    if (mode() == 0) {
        if (code != LV_EVENT_CLICKED) {
            return;
        }
        if (side == UI_SIDE_A) {
            toggle_voice();
        } else {
            ui_open(UI_OPEN_SETTINGS);
        }
        return;
    }
    // translator chips: tap = language picker, hold >= 400 ms = talk in this language, while the device
    // speaks a press stops it (the app treats it like the orb)
    int32_t st = state();
    if (code == LV_EVENT_PRESSED) {
        s_chip_side = side;
        s_chip_talk = false;
        s_press_t = lv_tick_get();
        if (st == UI_ST_SPEAK) {
            s_chip_talk = true;
            ui_emit(UI_IN_PTT_DOWN, side);
            return;
        }
        if (!s_chip_timer) {
            s_chip_timer = lv_timer_create(chip_hold_cb, 400, NULL);
            lv_timer_set_auto_delete(s_chip_timer, false);
        }
        lv_timer_set_repeat_count(s_chip_timer, 1);
        lv_timer_reset(s_chip_timer);
        lv_timer_resume(s_chip_timer);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_chip_timer) {
            lv_timer_pause(s_chip_timer);
        }
        lv_obj_remove_state(btn, LV_STATE_CHECKED);
        uint32_t held = lv_tick_get() - s_press_t;
        if (s_chip_talk) {
            if (!ui_emit(UI_IN_PTT_UP, (int32_t)held)) {
                ui_set(g_s_state, UI_ST_IDLE);
            }
        } else if (code == LV_EVENT_RELEASED && released_on(btn)) {
            // a short tap opens the picker; PRESS_LOCK still sends RELEASED after the finger slid away
            ui_open(side == UI_SIDE_A ? UI_OPEN_LANG_A : UI_OPEN_LANG_B);
        }
        s_chip_side = -1;
        s_chip_talk = false;
        status_refresh();
    }
}

static void pill_cb(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    ui_orb_look_at(s_orb, p, 900);
    // always M, also with a fault (its fix is M's first row): the pill is the only way to the mode, typed
    // questions and history, and in translator mode to Settings (3.14)
    ui_open(UI_OPEN_MODE_SHEET);
}

// ---- Subject observers

static void pulse_exec(void *var, int32_t v)
{
    lv_obj_set_style_outline_width(var, v, 0);
}

static void target_pulse(int side)
{
    // A19: the chip of the listener pulses while the device speaks to them
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = i == 0 ? s_left : s_right;
        lv_anim_delete(c, pulse_exec);
        lv_obj_set_style_outline_width(c, 0, 0);
        if (i == side) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, c);
            lv_anim_set_exec_cb(&a, pulse_exec);
            lv_anim_set_values(&a, 1, 3);
            lv_anim_set_duration(&a, 600);
            lv_anim_set_reverse_duration(&a, 600);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_start(&a);
        }
    }
}

static int32_t s_prev_state = UI_ST_IDLE;
static int s_prev_mode = -1;

static void state_obs(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    int32_t prev = s_prev_state;
    int32_t st = lv_subject_get_int(s);
    uint32_t now = lv_tick_get();
    bool rec = st == UI_ST_REC || st == UI_ST_REC_LATCHED;
    bool was_rec = prev == UI_ST_REC || prev == UI_ST_REC_LATCHED || prev == UI_ST_REC_CANCEL;
    if (rec && !was_rec) {
        s_rec_t0 = now;
        s_autoscroll = true;    // the next talk press jumps back to the bottom
    }
    if (!rec) {
        ui_rim_record(-1, false);
        s_cancel_armed = false;
    }
    if (st != prev) {
        s_state_t0 = now;
    }
    if (st == UI_ST_ERROR) {
        error_begin();
    }
    if (st == UI_ST_ANSWERED || (st == UI_ST_IDLE && prev != UI_ST_IDLE)) {
        close_open_reply(mode());
    }
    ui_orb_set_target_side(s_orb, mode() == 1 && st == UI_ST_SPEAK ? s_target_side : -1);
    target_pulse(mode() == 1 && st == UI_ST_SPEAK ? s_target_side : -1);
    s_prev_state = st;
    ui_orb_set_state(s_orb, orb_state());
    if ((st == UI_ST_OFFLINE) != (prev == UI_ST_OFFLINE)) {
        pill_refresh();         // "Нет Wi-Fi ›" follows the hollow orb
    }
    lv_timer_resume(s_tick);
    status_refresh();
    hint_refresh();
}

static void swap_cb(lv_timer_t *t)
{
    (void)t;
    side_refresh();
    pill_refresh();
    status_refresh();       // chip B is never hidden; the settings button follows the state
}

static void mode_obs(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    int m = lv_subject_get_int(s);
    int prev = s_prev_mode;
    ui_theme_set_mode(m);
    ui_orb_set_mode(s_orb, m);
    if (prev < 0) {
        swap_cb(NULL);
    } else if (prev != m) {
        // A21: buttons <-> chips swap half-way through the 400 ms colour morph
        if (!s_swap_timer) {
            s_swap_timer = lv_timer_create(swap_cb, UI_T_MORPH / 2, NULL);
            lv_timer_set_auto_delete(s_swap_timer, false);
        }
        lv_timer_set_repeat_count(s_swap_timer, 1);
        lv_timer_reset(s_swap_timer);
        lv_timer_resume(s_swap_timer);
        rebuild_chat();
    }
    s_prev_mode = m;
    hint_refresh();
}

static void misc_obs(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    if (s == g_s_upload) {
        ui_orb_set_upload(s_orb, lv_subject_get_int(s));
        status_refresh();
        return;
    }
    if (s == g_s_net || s == g_s_fault) {
        if (s == g_s_net && !net_down() && state() == UI_ST_OFFLINE) {
            ui_set(g_s_state, UI_ST_IDLE);      // back online: the hollow orb fills again
        }
        ui_orb_set_state(s_orb, orb_state());
        pill_refresh();
        status_refresh();
        return;
    }
    if (s == g_s_hints) {
        hint_refresh();
        return;
    }
    if (s == g_s_face) {
        ui_orb_set_face(s_orb, lv_subject_get_int(s) != 0);
        return;
    }
    if (s == g_s_tr_dir) {
        pill_refresh();
        return;
    }
    side_refresh();     // text_only, speaker
}

// ---- Build

static lv_obj_t *round_btn(const char *name, int32_t cx)
{
    lv_obj_t *b = lv_obj_create(s_scr);
    ui_theme_plain(b);
    lv_obj_set_name_static(b, name);
    lv_obj_add_style(b, &st_btn_round, 0);
    lv_obj_add_style(b, &st_btn_round_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(b, cx - SIDE_D / 2, SIDE_CY - SIDE_D / 2);
    lv_obj_set_size(b, SIDE_D, SIDE_D);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, side_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(b, side_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(b, side_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(b, side_cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *centred_label(lv_obj_t *parent, const char *name, const lv_style_t *st)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_name_static(l, name);
    if (st) {
        lv_obj_add_style(l, (lv_style_t *)st, 0);
    }
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

lv_obj_t *scr_home_create(void)
{
    lv_anim_delete(&s_layout_v, NULL);
    s_scr = ui_chrome_screen();
    lv_obj_set_name_static(s_scr, "home");
    s_conv = false;
    s_layout_v = 0;
    s_layout_anim = NULL;
    s_prev_mode = -1;
    s_prev_state = UI_ST_IDLE;
    s_cancel_armed = s_orb_pressed = false;
    s_chip_side = -1;

    // Chat viewport (created first: the orb, buttons and fades sit above it)
    s_chat = lv_obj_create(s_scr);
    ui_theme_plain(s_chat);
    lv_obj_set_name_static(s_chat, "home_chat");
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_CLICKABLE);     // tap target for the messages (2.4, 5.4 item 8)
    lv_obj_add_event_cb(s_chat, chat_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(s_chat, CHAT_X, CHAT_Y);
    lv_obj_set_size(s_chat, CHAT_W, CHAT_H);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_chat, CHAT_GAP, 0);
    lv_obj_set_style_pad_top(s_chat, FADE_TOP_H, 0);
    lv_obj_set_style_pad_bottom(s_chat, FADE_BOT_H, 0);
    lv_obj_add_event_cb(s_chat, chat_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_chat, chat_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_HIDDEN);
    ui_chrome_fades(s_scr, CHAT_X, CHAT_Y, CHAT_W, CHAT_H, FADE_TOP_H, FADE_BOT_H);

    // Pill (mode / fault) and the status line that replaces it while busy
    s_pill = lv_obj_create(s_scr);
    ui_theme_plain(s_pill);
    lv_obj_set_name_static(s_pill, "home_mode");
    lv_obj_add_style(s_pill, &st_pill, 0);
    lv_obj_add_style(s_pill, &st_pill_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(s_pill, PILL_X, PILL_Y_EMPTY);
    lv_obj_set_size(s_pill, PILL_W, PILL_H);
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pill, pill_cb, LV_EVENT_CLICKED, NULL);
    s_pill_lbl = centred_label(s_pill, "home_mode_text", NULL);
    lv_label_set_recolor(s_pill_lbl, true);
    lv_obj_center(s_pill_lbl);

    s_status = centred_label(s_scr, "home_state", &st_text16);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_status, STATUS_X_EMPTY, STATUS_Y_EMPTY);
    lv_obj_set_size(s_status, STATUS_W_EMPTY, STATUS_H);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    s_status_txt[0] = 0;
    s_status_err = false;

    // Orb: a sibling of the chat, never a child, so a drag on it never scrolls (2.4)
    s_orb = ui_orb_create(s_scr, ORB_HERO);
    lv_obj_set_name_static(s_orb, "home_orb");
    lv_obj_add_event_cb(s_orb, orb_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_orb, orb_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_orb, orb_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_orb, orb_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_orb, orb_cb, LV_EVENT_CLICKED, NULL);

    // Side buttons after the orb: their boxes overlap its box, its hit test is a circle
    s_left = round_btn("home_voice", SIDE_L_CX);
    s_left_lbl = centred_label(s_left, "home_voice_icon", NULL);
    lv_obj_center(s_left_lbl);
    s_wdot = lv_obj_create(s_left);
    ui_theme_plain(s_wdot);
    lv_obj_set_name_static(s_wdot, "home_voice_dot");
    lv_obj_set_style_bg_color(s_wdot, ui_col(ui_accent(UI_ACC_WARNING)->top), 0);
    lv_obj_set_style_bg_opa(s_wdot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_wdot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(s_wdot, 6, 6);
    lv_obj_set_pos(s_wdot, 37, 9);      // upper right of the 52 px circle, inside it
    lv_obj_remove_flag(s_wdot, LV_OBJ_FLAG_CLICKABLE);
    s_right = round_btn("home_settings", SIDE_R_CX);
    s_right_lbl = centred_label(s_right, "home_settings_icon", NULL);
    lv_obj_center(s_right_lbl);

    // fixed one-line boxes: DOTS only truncates when the height is fixed too
    s_hint1 = centred_label(s_scr, "home_hint1", &st_text16);
    lv_label_set_long_mode(s_hint1, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_hint1, UI_CX - HINT_W1 / 2, HINT_Y1);
    lv_obj_set_size(s_hint1, HINT_W1, HINT_H);
    s_hint2 = centred_label(s_scr, "home_hint2", &st_text16);
    lv_label_set_long_mode(s_hint2, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_hint2, UI_CX - HINT_W2 / 2, HINT_Y2);
    lv_obj_set_size(s_hint2, HINT_W2, HINT_H);

    if (!s_tick) {
        s_tick = lv_timer_create(tick_cb, 100, NULL);   // file-level timers survive a Home rebuild
    }
    lv_timer_pause(s_tick);

    // bind: observers run once now and are removed with the screen
    ui_watch(g_s_mode, mode_obs, s_scr, NULL);
    ui_watch(g_s_text_only, misc_obs, s_scr, NULL);
    ui_watch(g_s_speaker, misc_obs, s_scr, NULL);
    ui_watch(g_s_net, misc_obs, s_scr, NULL);
    ui_watch(g_s_fault, misc_obs, s_scr, NULL);
    ui_watch(g_s_upload, misc_obs, s_scr, NULL);
    ui_watch(g_s_hints, misc_obs, s_scr, NULL);
    ui_watch(g_s_face, misc_obs, s_scr, NULL);
    ui_watch(g_s_tr_dir, misc_obs, s_scr, NULL);
    ui_watch(g_s_state, state_obs, s_scr, NULL);
    rebuild_chat();
    return s_scr;
}
