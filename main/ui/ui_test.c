#include "ui_test.h"
#include "display.h"

#include <stdio.h>
#include "esp_lvgl_port.h"

static lv_obj_t *s_touch_label;
static lv_obj_t *s_dot;
static lv_obj_t *s_btn_label;
static int s_taps;

static void screen_touch_cb(lv_event_t *e)
{
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
    lv_label_set_text_fmt(s_btn_label, "Taps: %d", ++s_taps);
}

static lv_obj_t *bar(lv_obj_t *parent, lv_color_t c, int x)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 44, 44);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, x, -20);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void ui_test_create(void)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05070D), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSING, NULL);

    // Rim ring: shows where the visible circle is
    lv_obj_t *ring = lv_arc_create(scr);
    lv_obj_set_size(ring, 352, 352);
    lv_obj_center(ring);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_value(ring, 0);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x1E3A5F), LV_PART_MAIN);

    // Spinner: animation / frame-rate check
    lv_obj_t *spin = lv_spinner_create(scr);
    lv_obj_set_size(spin, 320, 320);
    lv_obj_center(spin);
    lv_spinner_set_anim_params(spin, 1600, 90);
    lv_obj_set_style_arc_width(spin, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(spin, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8EEF7), 0);
    lv_label_set_text(title, "Round Assistant M1");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -88);

    const display_info_t *d = display_info();
    lv_obj_t *id = lv_label_create(scr);
    lv_obj_set_style_text_font(id, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(id, lv_color_hex(0x8FA3BF), 0);
    lv_label_set_text_fmt(id, "panel %02X %02X %02X %02X  batch %d", d->panel_id[0], d->panel_id[1],
                          d->panel_id[2], d->panel_id[3], d->panel_batch);
    lv_obj_align(id, LV_ALIGN_CENTER, 0, -60);

    // Colour order check: red, green, blue, white
    bar(scr, lv_color_hex(0xFF0000), -78);
    bar(scr, lv_color_hex(0x00FF00), -26);
    bar(scr, lv_color_hex(0x0000FF), 26);
    bar(scr, lv_color_hex(0xFFFFFF), 78);

    s_touch_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_touch_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0x8FA3BF), 0);
    lv_label_set_text(s_touch_label, "touch the screen");
    lv_obj_align(s_touch_label, LV_ALIGN_CENTER, 0, 22);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 150, 56);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_radius(btn, 28, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2962FF), 0);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_label = lv_label_create(btn);
    lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_btn_label, "Taps: 0");
    lv_obj_center(s_btn_label);

    s_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 16, 16);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xFFD54F), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lvgl_port_unlock();
}
