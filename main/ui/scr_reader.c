// RD (docs/ui.md 3.6): large translation for the other person, flip 180 deg, wrong language, replay.
#include "ui_chat.h"
#include "ui_internal.h"
#include "ui_rim.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <ctype.h>
#include <stdio.h>

// Text box x 54-306, y 76-260 (252x184): corners |(126,104)| = 163.4. Buttons Ø52:
// (124,296) and (236,296): |(56,116)| + 26 = 154.8; (180,318): 138 + 26 = 164.0; 8 px apart.
#define BOX_X   54
#define BOX_Y   76
#define BOX_W   252
#define BOX_H   184
#define BTN_D   52

static uint32_t s_entry;
static lv_obj_t *s_box;
static bool s_flipped;
static uint32_t s_scroll_end_t;

static void box_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL_END) {
        s_scroll_end_t = lv_tick_get();
    } else if (code == LV_EVENT_CLICKED && lv_tick_elaps(s_scroll_end_t) >= 300) {
        ui_nav_back();      // tapping the text also goes back
    }
}

static void wrong_cb(lv_event_t *e)
{
    (void)e;
    const cm_entry_t *en = cm_find(s_entry);
    int32_t other = en && en->side == UI_SIDE_A ? UI_SIDE_B : UI_SIDE_A;
    if (!ui_emit(UI_IN_WRONG_LANG, other)) {
        ui_toast(ui_s(S_NOT_YET));
    }
}

static void replay_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_emit(UI_IN_REPLAY, (int32_t)s_entry)) {
        ui_toast(ui_s(S_NOT_YET));
    }
}

static void flip_cb(lv_event_t *e)
{
    (void)e;
    // A34: one static 180 deg rotation (a single ARGB8888 layer render); no scrolling while flipped
    s_flipped = !s_flipped;
    lv_obj_set_style_transform_rotation(s_box, s_flipped ? 1800 : 0, 0);
    lv_obj_set_flag(s_box, LV_OBJ_FLAG_SCROLLABLE, !s_flipped);
}

static lv_obj_t *button(lv_obj_t *scr, const char *name, const char *sym, int32_t cx, int32_t cy, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(scr);
    ui_theme_plain(b);
    lv_obj_set_name_static(b, name);
    lv_obj_add_style(b, &st_btn_round, 0);
    lv_obj_add_style(b, &st_btn_round_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(b, cx - BTN_D / 2, cy - BTN_D / 2);
    lv_obj_set_size(b, BTN_D, BTN_D);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text_static(l, sym);
    lv_obj_center(l);
    return b;
}

// Replay needs a speaker (5.2); follows UI_SET_SPEAKER while the reader is open
static void speaker_obs(lv_observer_t *o, lv_subject_t *s)
{
    lv_obj_t *b = lv_observer_get_target_obj(o);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    bool on = lv_subject_get_int(s) != 0;
    lv_obj_set_flag(b, LV_OBJ_FLAG_CLICKABLE, on);
    lv_obj_set_state(b, LV_STATE_DISABLED, !on);
    if (on) {
        lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_COLOR, 0);
    } else {
        lv_obj_set_style_text_color(l, ui_col(UI_COL_TEXT3), 0);   // disabled: text-3, fill unchanged
    }
}

lv_obj_t *scr_reader_build(uint32_t entry_id)
{
    // entry 0 ("ui goto reader"): the newest translated turn
    const cm_entry_t *en = entry_id ? cm_find(entry_id) : cm_last_of(1, CM_TURN);
    if (!en || en->kind != CM_TURN) {
        return NULL;
    }
    entry_id = en->id;
    s_entry = entry_id;
    s_flipped = false;
    lv_obj_t *scr = ui_chrome_screen();
    lv_obj_set_name_static(scr, "reader");

    s_box = lv_obj_create(scr);
    ui_theme_plain(s_box);
    lv_obj_set_name_static(s_box, "rd_text");
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC |
                               LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(s_box, BOX_X, BOX_Y);
    lv_obj_set_size(s_box, BOX_W, BOX_H);
    lv_obj_set_style_transform_pivot_x(s_box, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_box, LV_PCT(50), 0);
    lv_obj_add_event_cb(s_box, box_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(s_box, box_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(s_box);
    lv_obj_set_name_static(l, "rd_label");
    const char *tr = en->text2 ? en->text2 : "";
    lv_obj_add_style(l, ui_str_font_has_all(tr, &ui_28) ? &st_text28 : &st_text20, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, tr);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_update_layout(s_box);
    if (lv_obj_get_height(l) < BOX_H) {
        lv_obj_center(l);       // short text sits in the middle of the box
    }

    button(scr, "rd_wrong", SYM_EXCHANGE, 124, 296, wrong_cb);
    lv_obj_t *replay = button(scr, "rd_replay", SYM_PLAY, 236, 296, replay_cb);
    ui_watch(g_s_speaker, speaker_obs, replay, NULL);   // runs once now; removed with the button
    button(scr, "rd_flip", SYM_SYNC, 180, 318, flip_cb);

    // "‹ RU → EN" is the direction label too
    char title[24], s[4] = { 0 }, d[4] = { 0 };
    for (int i = 0; i < 3 && en->src[i]; i++) {
        s[i] = (char)toupper((unsigned char)en->src[i]);
    }
    for (int i = 0; i < 3 && en->dst[i]; i++) {
        d[i] = (char)toupper((unsigned char)en->dst[i]);
    }
    snprintf(title, sizeof(title), "%s " TXT_ARROW " %s", s, d);
    ui_chrome_title(scr, title);
    ui_chrome_swipe_back(scr);
    ui_rim_attach_scroll(s_box);
    return scr;
}
