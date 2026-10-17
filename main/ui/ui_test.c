// M2 bring-up test screen ("ui goto test"): rim ring with the live mic level, a rotating arc as a frame-rate
// check, panel id, colour bars, touch coordinates and a 5 s recording button. It ignores the round-screen
// rules on purpose (the ring marks the panel edge).
#include "ui_test.h"
#include "ui_internal.h"
#include "ui_theme.h"
#include "audio_in.h"
#include "display.h"

#include <stdio.h>

static lv_obj_t *s_touch_label;
static lv_obj_t *s_dot;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_level_arc;
static lv_timer_t *s_timer;
static uint32_t s_rec_until;
static bool s_rec_shown = true;

static void screen_touch_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_label_set_text_fmt(s_touch_label, "touch %d, %d", (int)p.x, (int)p.y);
    lv_obj_set_pos(s_dot, p.x - 8, p.y - 8);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

static void btn_cb(lv_event_t *e)
{
    (void)e;
    if (audio_in_recording()) {
        return;
    }
    audio_in_start(5000);
    s_rec_until = lv_tick_get() + 5000;
    s_rec_shown = false;
    lv_label_set_text(s_btn_label, "Speak now...");
}

// 20 Hz: live mic level on the ring, and the result once a recording finishes
static void level_timer_cb(lv_timer_t *t)
{
    (void)t;
    lv_arc_set_value(s_level_arc, audio_in_level());
    if (!s_rec_shown && !audio_in_recording()) {
        const audio_clip_t *c = audio_in_last();
        if (c) {
            lv_label_set_text_fmt(s_btn_label, "%.1f dB  pk %d", c->rms_dbfs_x10 / 10.0, c->peak);
        }
        s_rec_shown = true;
    } else if (!s_rec_shown) {
        int left = (int)(s_rec_until - lv_tick_get()) / 1000 + 1;
        lv_label_set_text_fmt(s_btn_label, "Speak now... %d", left < 0 ? 0 : left);
    }
}

static void spin_exec(void *var, int32_t v)
{
    lv_arc_set_rotation(var, v);
}

static void delete_cb(lv_event_t *e)
{
    (void)e;
    lv_timer_delete(s_timer);
    s_timer = NULL;
}

static lv_obj_t *bar(lv_obj_t *parent, lv_color_t c, int x)
{
    lv_obj_t *o = lv_obj_create(parent);
    ui_theme_plain(o);
    lv_obj_set_size(o, 44, 44);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, x, -20);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *ring(lv_obj_t *scr, int32_t size, int32_t width, uint32_t track, uint32_t ind)
{
    lv_obj_t *a = lv_arc_create(scr);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(track), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, track ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(ind), LV_PART_INDICATOR);
    return a;
}

lv_obj_t *ui_test_build(uint32_t arg)
{
    (void)arg;
    lv_obj_t *scr = ui_chrome_screen();
    lv_obj_set_name_static(scr, "test");
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05070D), 0);
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);

    // Rim ring: shows where the visible circle is
    s_level_arc = ring(scr, 352, 4, 0x1E3A5F, 0x66BB6A);
    lv_arc_set_bg_angles(s_level_arc, 0, 360);
    lv_arc_set_range(s_level_arc, 0, 100);
    lv_arc_set_value(s_level_arc, 0);

    // Rotating 90 deg arc: animation / frame-rate check (no lv_spinner in this build)
    lv_obj_t *spin = ring(scr, 320, 6, 0, 0x4FC3F7);
    lv_arc_set_bg_angles(spin, 0, 360);
    lv_arc_set_angles(spin, 0, 90);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, spin);
    lv_anim_set_exec_cb(&a, spin_exec);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, 1600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_add_style(title, &st_text20, 0);
    lv_label_set_text(title, "Round Assistant test");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -88);

    const display_info_t *d = display_info();
    lv_obj_t *id = lv_label_create(scr);
    lv_obj_add_style(id, &st_text16, 0);
    lv_label_set_text_fmt(id, "panel %02X %02X %02X %02X  batch %d", d->panel_id[0], d->panel_id[1],
                          d->panel_id[2], d->panel_id[3], d->panel_batch);
    lv_obj_align(id, LV_ALIGN_CENTER, 0, -60);

    // Colour order check: red, green, blue, white
    bar(scr, lv_color_hex(0xFF0000), -78);
    bar(scr, lv_color_hex(0x00FF00), -26);
    bar(scr, lv_color_hex(0x0000FF), 26);
    bar(scr, lv_color_hex(0xFFFFFF), 78);

    s_touch_label = lv_label_create(scr);
    lv_obj_add_style(s_touch_label, &st_text16, 0);
    lv_label_set_text(s_touch_label, "touch the screen");
    lv_obj_align(s_touch_label, LV_ALIGN_CENTER, 0, 22);

    lv_obj_t *btn = lv_obj_create(scr);
    ui_theme_plain(btn);
    lv_obj_set_name_static(btn, "test_rec");
    lv_obj_set_size(btn, 190, 56);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_radius(btn, 28, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xC62828), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x8E1C1C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_label = lv_label_create(btn);
    lv_obj_add_style(s_btn_label, &st_text20, 0);
    lv_label_set_text(s_btn_label, "REC 5 s");
    lv_obj_center(s_btn_label);

    s_dot = lv_obj_create(scr);
    ui_theme_plain(s_dot);
    lv_obj_set_size(s_dot, 16, 16);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xFFD54F), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);

    s_rec_shown = true;
    s_timer = lv_timer_create(level_timer_cb, 50, NULL);
    return scr;
}
