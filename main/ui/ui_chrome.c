#include "ui_internal.h"
#include "ui_geom.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <stdio.h>

// ---- Arc title (ui.md 3.1): lv_arclabel at (80,14) 200x58, centre offset +137 -> (180,180), r 148,
// 64 deg centred at 12 o'clock. The API takes unsigned offsets, so the object sits above the centre.
#define TITLE_X     80
#define TITLE_Y     14
#define TITLE_W     200
#define TITLE_H     58
#define TITLE_R     148
#define TITLE_OFS_Y (UI_CY - (TITLE_Y + TITLE_H / 2))     // 137

static void back_zone_cb(lv_event_t *e)
{
    lv_obj_t *title = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_obj_add_state(title, LV_STATE_PRESSED);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_remove_state(title, LV_STATE_PRESSED);
    } else if (code == LV_EVENT_CLICKED) {
        ui_nav_back();
    }
}

lv_obj_t *ui_chrome_title(lv_obj_t *scr, const char *text)
{
    lv_obj_t *t = lv_arclabel_create(scr);
    lv_obj_set_name_static(t, "title");
    lv_obj_add_style(t, &st_arc_title, 0);
    lv_obj_add_style(t, &st_arc_title_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(t, TITLE_X, TITLE_Y);
    lv_obj_set_size(t, TITLE_W, TITLE_H);
    lv_arclabel_set_center_offset_y(t, TITLE_OFS_Y);
    lv_arclabel_set_radius(t, TITLE_R);
    lv_arclabel_set_angle_start(t, 238);
    lv_arclabel_set_angle_size(t, 64);
    lv_arclabel_set_dir(t, LV_ARCLABEL_DIR_CLOCKWISE);
    lv_arclabel_set_text_horizontal_align(t, LV_ARCLABEL_TEXT_ALIGN_CENTER);
    lv_arclabel_set_text_vertical_align(t, LV_ARCLABEL_TEXT_ALIGN_CENTER);
    lv_arclabel_set_overflow(t, LV_ARCLABEL_OVERFLOW_ELLIPSIS);
    char buf[96];
    snprintf(buf, sizeof(buf), SYM_LEFT " %s", text);
    lv_arclabel_set_text(t, buf);

    // Invisible back zone x 96-264, y 10-68, created after the title so it is on top
    lv_obj_t *z = lv_obj_create(scr);
    ui_theme_plain(z);
    lv_obj_set_name_static(z, "title_back");
    lv_obj_set_pos(z, 96, 10);
    lv_obj_set_size(z, 168, 58);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(z, back_zone_cb, LV_EVENT_PRESSED, t);
    lv_obj_add_event_cb(z, back_zone_cb, LV_EVENT_RELEASED, t);
    lv_obj_add_event_cb(z, back_zone_cb, LV_EVENT_PRESS_LOST, t);
    lv_obj_add_event_cb(z, back_zone_cb, LV_EVENT_CLICKED, t);
    return t;
}

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in && lv_indev_get_gesture_dir(in) == LV_DIR_RIGHT) {
        lv_indev_wait_release(in);      // the row under the finger must not get a CLICKED
        ui_nav_back();
    }
}

void ui_chrome_swipe_back(lv_obj_t *scr)
{
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
}

lv_obj_t *ui_chrome_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &st_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

void ui_chrome_fades(lv_obj_t *scr, int32_t x, int32_t y, int32_t w, int32_t h, int32_t top_h, int32_t bot_h)
{
    lv_obj_t *t = lv_obj_create(scr);
    ui_theme_plain(t);
    lv_obj_set_name_static(t, "fade_top");
    lv_obj_add_style(t, &st_fade_top, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_size(t, w, top_h);
    lv_obj_t *b = lv_obj_create(scr);
    ui_theme_plain(b);
    lv_obj_set_name_static(b, "fade_bottom");
    lv_obj_add_style(b, &st_fade_bottom, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(b, x, y + h - bot_h);
    lv_obj_set_size(b, w, bot_h);
}

// ---- Toast for non-Home screens (Home swaps it into its status slot): stadium x 64-296, y 272-312 on the
// top layer, ui_16 text <= 200 px. Fit: corner-circle centres (84,292) / (276,292): |(96,112)| + 20 = 167.5.
#define TOAST_X 64
#define TOAST_Y 272
#define TOAST_W 232
#define TOAST_H 40

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static void toast_hide(lv_timer_t *t)
{
    (void)t;
    if (s_toast) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_chrome_toast(const char *text)
{
    if (!s_toast) {
        s_toast = lv_obj_create(lv_layer_top());
        ui_theme_plain(s_toast);
        lv_obj_set_name_static(s_toast, "toast");
        lv_obj_add_style(s_toast, &st_toast, 0);
        lv_obj_set_pos(s_toast, TOAST_X, TOAST_Y);
        lv_obj_set_size(s_toast, TOAST_W, TOAST_H);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = lv_label_create(s_toast);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        s_toast_timer = lv_timer_create(toast_hide, 2000, NULL);
        lv_timer_set_auto_delete(s_toast_timer, false);
    }
    lv_label_set_text(lv_obj_get_child(s_toast, 0), text ? text : "");
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    lv_timer_set_repeat_count(s_toast_timer, 1);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
}
