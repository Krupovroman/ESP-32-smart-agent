// D.touch (docs/ui.md 3.15): black screen, a 12 px dot at the last sample and "x · y · Hz" in the centre.
// Moving one small object keeps each update at about 2 x 14^2 px. The ✕ circle at the bottom exits (PWR locks).
#include "ui_internal.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <stdio.h>

static lv_obj_t *s_dot, *s_info;
static lv_point_t s_last;
static uint32_t s_samples, s_hz;
static lv_timer_t *s_rate;

static void show(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), ui_s(S_TOUCH_FMT), (int)s_last.x, (int)s_last.y, (int)s_hz);
    lv_label_set_text(s_info, buf);
}

static void touch_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (p.x != s_last.x || p.y != s_last.y) {
        s_samples++;            // count distinct reports, i.e. the controller's rate
    }
    s_last = p;
    lv_obj_set_pos(s_dot, p.x - 6, p.y - 6);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    show();
}

static void rate_cb(lv_timer_t *t)
{
    (void)t;
    if (s_hz != s_samples) {
        s_hz = s_samples;
        show();
    }
    s_samples = 0;
}

static void delete_cb(lv_event_t *e)
{
    (void)e;
    lv_timer_delete(s_rate);
    s_rate = NULL;
}

static void exit_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_back();
}

lv_obj_t *scr_touch_build(uint32_t arg)
{
    (void)arg;
    lv_obj_t *scr = ui_chrome_screen();
    lv_obj_set_name_static(scr, "touch");
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);

    // centre (180,180), width 200: x 80-280, y 170-190 -> corner |(100,10)| = 100.5
    s_info = lv_label_create(scr);
    lv_obj_set_name_static(s_info, "touch_info");
    lv_obj_add_style(s_info, &st_text16, 0);
    lv_obj_set_width(s_info, 200);
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_info, 80, 170);
    lv_label_set_text_static(s_info, ui_s(S_TOUCH_HELP));

    s_dot = lv_obj_create(scr);
    ui_theme_plain(s_dot);
    lv_obj_set_name_static(s_dot, "touch_dot");
    lv_obj_add_style(s_dot, &st_dot, 0);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    // ✕ circle Ø52 at (180,300): |(0,120)| + 26 = 146; its own touches are not samples
    lv_obj_t *x = lv_obj_create(scr);
    ui_theme_plain(x);
    lv_obj_set_name_static(x, "touch_exit");
    lv_obj_add_style(x, &st_btn_round, 0);
    lv_obj_add_style(x, &st_btn_round_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(x, 180 - 26, 300 - 26);
    lv_obj_set_size(x, 52, 52);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(x, exit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text_static(xl, SYM_TIMES);
    lv_obj_center(xl);

    s_samples = s_hz = 0;
    s_last = (lv_point_t){ 0, 0 };
    s_rate = lv_timer_create(rate_cb, 1000, NULL);
    return scr;
}
