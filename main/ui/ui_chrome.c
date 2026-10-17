#include "ui_internal.h"
#include "ui_geom.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---- Arc title (ui.md 3.1): text along the top rim, centred at 12 o'clock, in a 200x58 box at (80,14).
// lv_arclabel places glyphs on whole degrees without LV_USE_FLOAT (2.6 px steps at r 148, visibly uneven
// spacing), so the title draws its own glyphs: baseline centres at exact float positions on r 144,
// each rotated in 0.1 deg steps. Caps reach r ~159, Й/Ё r ~162 (<= 162 per 3.1). Static: drawn only when
// the screen is built or the area is redrawn.
#define TITLE_X     80
#define TITLE_Y     14
#define TITLE_W     200
#define TITLE_H     58
#define TITLE_RB    144.0f      // baseline radius
#define TITLE_MAX   165         // arc length available (64 deg at r 148): chevron + text

static void title_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    const char *txt = lv_obj_get_user_data(obj);
    if (!txt) {
        return;
    }
    const lv_font_t *f = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    lv_draw_letter_dsc_t d;
    lv_draw_letter_dsc_init(&d);
    d.font = f;
    d.color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    d.opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);

    // code points and advances (with kerning), truncated with "…" to the available arc length
    uint32_t cps[48];
    int32_t adv[48];
    int n = 0;
    int32_t total = 0;
    uint32_t ell = 0x2026;
    int32_t ell_w = lv_font_get_glyph_width(f, ell, 0);
    for (const unsigned char *s = (const unsigned char *)txt; *s && n < 47;) {
        uint32_t c = *s++;
        if (c >= 0xF0 && s[0] && s[1] && s[2]) {
            c = ((c & 7u) << 18) | ((s[0] & 0x3Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
            s += 3;
        } else if (c >= 0xE0 && s[0] && s[1]) {
            c = ((c & 0xFu) << 12) | ((s[0] & 0x3Fu) << 6) | (s[1] & 0x3Fu);
            s += 2;
        } else if (c >= 0xC0 && s[0]) {
            c = ((c & 0x1Fu) << 6) | (s[0] & 0x3Fu);
            s += 1;
        }
        cps[n++] = c;
    }
    for (int i = 0; i < n; i++) {
        adv[i] = lv_font_get_glyph_width(f, cps[i], i + 1 < n ? cps[i + 1] : 0);
        if (total + adv[i] > TITLE_MAX - (i + 1 < n ? ell_w : 0)) {
            cps[i] = ell;
            adv[i] = ell_w;
            total += ell_w;
            n = i + 1;
            break;
        }
        total += adv[i];
    }

    lv_layer_t *layer = lv_event_get_layer(e);
    float s = -(float)total / 2.0f;     // arc length from 12 o'clock, clockwise positive
    for (int i = 0; i < n; i++) {
        float mid = s + (float)adv[i] / 2.0f;
        float th = -1.5707963f + mid / TITLE_RB;          // 12 o'clock is -90 deg
        float px = (float)UI_CX + TITLE_RB * cosf(th);
        float py = (float)UI_CY + TITLE_RB * sinf(th);
        // lv_draw_letter puts the glyph's baseline centre (pivot adv/2, line_height - base_line) on `point`
        // and rotates around it (lv_draw_label.c / lv_draw_sw_letter.c, as lv_arclabel does)
        lv_point_t p = { (int32_t)lroundf(px), (int32_t)lroundf(py) };
        d.unicode = cps[i];
        d.rotation = (int32_t)lroundf((th * 57.29578f + 90.0f) * 10.0f);
        if (cps[i] != ' ') {
            lv_draw_letter(layer, &d, &p);
        }
        s += (float)adv[i];
    }
}

static void title_free_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_free(lv_obj_get_user_data(obj));
    lv_obj_set_user_data(obj, NULL);
}

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
    lv_obj_t *t = lv_obj_create(scr);
    ui_theme_plain(t);
    lv_obj_set_name_static(t, "title");
    lv_obj_add_style(t, &st_arc_title, 0);
    lv_obj_add_style(t, &st_arc_title_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(t, TITLE_X, TITLE_Y);
    lv_obj_set_size(t, TITLE_W, TITLE_H);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    size_t n = strlen(text) + sizeof(SYM_LEFT) + 2;
    char *buf = lv_malloc(n);
    LV_ASSERT_MALLOC(buf);
    snprintf(buf, n, SYM_LEFT " %s", text);
    lv_obj_set_user_data(t, buf);
    lv_obj_add_event_cb(t, title_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(t, title_free_cb, LV_EVENT_DELETE, NULL);

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
        // one fixed ui_16 line: DOTS only truncates when the height is fixed, else a long app toast wraps
        // and its second line is cut by the 40 px stadium
        lv_obj_set_size(l, LV_PCT(100), lv_font_get_line_height(&ui_16));
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
