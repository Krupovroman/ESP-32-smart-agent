// A, volume / brightness adjuster T_ADJUST (docs/ui.md 3.9): a 270 deg lv_arc through the top, the value in
// ui_48, -/+ in steps of 5 and "Готово". Changes apply live (backlight duty, software gain); the setting is saved
// through the debounced writer (1.5 s, only while idle). No tick sound is played.
#include "ui_internal.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_work.h"
#include "audio_out.h"
#include "board.h"

#include <stdio.h>

// arc x 30-330, y 30-330 (r 150), ring r 134-150, knob <= 158; hit ring r 117-167 with ext_click_area 17.
// -/+ Ø52 at (144,254) / (216,254): |(36,74)| + 26 = 108.3 < 117 (the arc's dead zone).
// "Готово" x 120-240, y 290-340: corner centres (145,315)/(215,315) -> |(35,135)| + 25 = 164.5; it spans
// 61-119 deg from the centre, which the arc's hit test (52.8-127.2 deg rejected at r 117) leaves to it.
#define ARC_XY      30
#define ARC_S       300
#define STEP        5

static lv_obj_t *s_arc, *s_value;
static int s_kind;              // 0 volume, 1 brightness

static const char *key(void)
{
    return s_kind ? "bright" : "volume";
}

static void apply(int32_t v)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)v);
    lv_label_set_text(s_value, buf);
    if (s_kind) {
        board_backlight_set((uint8_t)v);
    } else {
        audio_out_set_volume((uint8_t)v);
    }
    ui_save_int(key(), v);
}

static void arc_cb(lv_event_t *e)
{
    (void)e;
    apply(lv_arc_get_value(s_arc));
}

static void step_cb(lv_event_t *e)
{
    int32_t d = (int32_t)(intptr_t)lv_event_get_user_data(e);
    int32_t v = lv_arc_get_value(s_arc);
    // snap to the 5 grid first, so 63 -> 65 / 60
    int32_t to = d > 0 ? (v / STEP + 1) * STEP : ((v + STEP - 1) / STEP - 1) * STEP;
    to = LV_CLAMP(lv_arc_get_min_value(s_arc), to, lv_arc_get_max_value(s_arc));
    if (to != v) {
        lv_arc_set_value(s_arc, to);
        apply(to);
    }
}

static void done_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_back();
}

static lv_obj_t *button(lv_obj_t *scr, const char *name, int32_t x, int32_t y, int32_t w, int32_t h,
                        const char *text, const lv_font_t *font, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_obj_create(scr);
    ui_theme_plain(b);
    lv_obj_set_name_static(b, name);
    lv_obj_add_style(b, w == h ? &st_btn_round : &st_stadium_secondary, 0);
    lv_obj_add_style(b, w == h ? &st_btn_round_pr : &st_stadium_secondary_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_col(UI_COL_TEXT1), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *scr_adjust_build(uint32_t arg)
{
    s_kind = arg ? 1 : 0;
    lv_obj_t *scr = ui_chrome_screen();
    lv_obj_set_name_static(scr, "adjust");
    ui_chrome_swipe_back(scr);

    s_arc = lv_arc_create(scr);
    lv_obj_remove_style_all(s_arc);
    lv_obj_set_name_static(s_arc, "adj_arc");
    lv_obj_add_style(s_arc, &st_arc_main, 0);
    lv_obj_add_style(s_arc, &st_arc_ind, LV_PART_INDICATOR);
    lv_obj_add_style(s_arc, &st_arc_knob, LV_PART_KNOB);
    lv_obj_set_pos(s_arc, ARC_XY, ARC_XY);
    lv_obj_set_size(s_arc, ARC_S, ARC_S);
    lv_arc_set_rotation(s_arc, 0);
    lv_arc_set_bg_angles(s_arc, 135, 45);       // 270 deg through the top
    lv_arc_set_range(s_arc, s_kind ? 5 : 0, 100);
    lv_arc_set_change_rate(s_arc, 720);
    lv_obj_set_ext_click_area(s_arc, 17);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    int32_t v = LV_CLAMP(s_kind ? 5 : 0, ui_cfg_int(key()), 100);
    lv_arc_set_value(s_arc, v);
    lv_obj_add_event_cb(s_arc, arc_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cap = lv_label_create(scr);
    lv_obj_set_name_static(cap, "adj_caption");
    lv_obj_add_style(cap, &st_text16, 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(cap, 200, lv_font_get_line_height(&ui_16));
    lv_obj_set_pos(cap, 80, 128 - lv_font_get_line_height(&ui_16) / 2);
    lv_label_set_text(cap, ui_s(s_kind ? S_BRIGHT : S_VOLUME));

    s_value = lv_label_create(scr);
    lv_obj_set_name_static(s_value, "adj_value");
    lv_obj_add_style(s_value, &st_text48, 0);
    lv_obj_set_style_text_align(s_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_value, 140, lv_font_get_line_height(&ui_48));
    lv_obj_set_pos(s_value, 110, 176 - lv_font_get_line_height(&ui_48) / 2);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)v);
    lv_label_set_text(s_value, buf);

    // created after the arc so they sit on top of its box
    button(scr, "adj_minus", 144 - 26, 254 - 26, 52, 52, "\xE2\x80\x93", &ui_28, step_cb, -1);    // en dash
    button(scr, "adj_plus", 216 - 26, 254 - 26, 52, 52, "+", &ui_28, step_cb, 1);
    button(scr, "adj_done", 120, 290, 120, 50, ui_s(S_DONE), &ui_20, done_cb, 0);
    return scr;
}
