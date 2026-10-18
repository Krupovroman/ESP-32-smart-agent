// T_STEP (docs/ui.md 3.12): mini orb, title, subtitle, primary and secondary stadiums, onboarding dots.
#include "ui_internal.h"
#include "ui_orb.h"
#include "ui_scr.h"
#include "ui_theme.h"

#include <stdio.h>

// title centre (180,153) w <= 280; subtitle centre (180,182) w <= 290; primary x 62-298, y 204-260
// (corner centres (90,232)/(270,232): |(90,52)| + 28 = 131.9); secondary x 80-280, y 268-324
// (|(72,116)| + 28 = 164.5); dots Ø6 at y 338, x 156..204 (|(24,158)| + 3 = 162.8)
#define TITLE_W     280
#define SUB_W       290
#define PRI_X       62
#define PRI_Y       204
#define PRI_W       236
#define SEC_X       80
#define SEC_Y       268
#define SEC_W       200
#define BTN_H       56

static lv_obj_t *centred(lv_obj_t *scr, const char *name, const lv_style_t *st, int32_t cy, int32_t w)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_name_static(l, name);
    lv_obj_add_style(l, (lv_style_t *)st, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    int32_t h = lv_font_get_line_height(f);
    lv_obj_set_size(l, w, h);
    lv_obj_set_pos(l, 180 - w / 2, cy - h / 2);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *stadium(lv_obj_t *scr, const char *name, bool primary, int32_t x, int32_t y, int32_t w,
                         lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_obj_create(scr);
    ui_theme_plain(b);
    lv_obj_set_name(b, name);
    lv_obj_add_style(b, primary ? &st_stadium_primary : &st_stadium_secondary, 0);
    lv_obj_add_style(b, primary ? &st_stadium_primary_pr : &st_stadium_secondary_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, BTN_H);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, w - 24);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_step_screen(const char *name, ui_step_t *st, int dot, lv_event_cb_t cb)
{
    char n[40];
    st->scr = ui_chrome_screen();
    lv_obj_set_name_static(st->scr, name);
    st->orb = ui_orb_create(st->scr, ORB_MINI);
    lv_obj_set_name_static(st->orb, "step_orb");
    lv_obj_remove_flag(st->orb, LV_OBJ_FLAG_CLICKABLE);     // decorative here
    ui_orb_set_mode(st->orb, ui_get(g_s_mode));
    ui_orb_set_face(st->orb, ui_get(g_s_face) != 0);
    st->title = centred(st->scr, "step_title", &st_text28, 153, TITLE_W);
    st->sub = centred(st->scr, "step_sub", &st_text16, 182, SUB_W);
    snprintf(n, sizeof(n), "%s_primary", name);
    st->primary = stadium(st->scr, n, true, PRI_X, PRI_Y, PRI_W, cb, 1);
    snprintf(n, sizeof(n), "%s_secondary", name);
    st->secondary = stadium(st->scr, n, false, SEC_X, SEC_Y, SEC_W, cb, 2);
    if (dot > 0) {
        const ui_accent_t *acc = ui_accent(ui_mode_accent(ui_get(g_s_mode)));
        for (int i = 0; i < 5; i++) {
            lv_obj_t *d = lv_obj_create(st->scr);
            ui_theme_plain(d);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(d, ui_col(i + 1 == dot ? acc->top : UI_COL_SURFACE3), 0);
            lv_obj_set_size(d, 6, 6);
            lv_obj_set_pos(d, 156 + 12 * i - 3, 338 - 3);
        }
    }
    return st->scr;
}

static void set_label(lv_obj_t *owner, lv_obj_t *label, const char *text)
{
    lv_obj_set_flag(owner, LV_OBJ_FLAG_HIDDEN, !text);
    if (text) {
        lv_label_set_text(label, text);
    }
}

void ui_step_set(ui_step_t *st, const char *title, const char *sub, const char *primary, const char *secondary)
{
    if (title) {
        // long titles fall back to ui_20 (3.12)
        lv_point_t sz;
        lv_text_get_size(&sz, title, &ui_28, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const lv_font_t *f = sz.x > TITLE_W ? &ui_20 : &ui_28;
        int32_t h = lv_font_get_line_height(f);
        lv_obj_set_style_text_font(st->title, f, 0);
        lv_obj_set_height(st->title, h);
        lv_obj_set_y(st->title, 153 - h / 2);
    }
    set_label(st->title, st->title, title);
    set_label(st->sub, st->sub, sub);
    set_label(st->primary, lv_obj_get_child(st->primary, 0), primary);
    set_label(st->secondary, lv_obj_get_child(st->secondary, 0), secondary);
}
