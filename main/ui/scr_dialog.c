// C, confirm dialog T_DIALOG (docs/ui.md 3.11): icon, title, up to three body lines, "Отмена" and the
// destructive action. PWR = "Отмена"; BOOT is ignored (ui_boot_down).
#include "ui_internal.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <string.h>

// icon centre (180,96); title centre (180,132) w <= 260; body x 60-300, y 150-210 (corners |(120,30)| = 123.7);
// stadiums x 56-176 / 184-304, y 226-276: corner centres (81,251) / (279,251) -> |(99,71)| + 25 = 146.8
#define BTN_Y   226
#define BTN_W   120
#define BTN_H   50

static ui_dialog_t s_d;
static char s_title[96], s_body[192], s_confirm[32];
static bool s_armed;

void ui_dialog_open(const ui_dialog_t *d)
{
    if (!d) {
        return;
    }
    s_d = *d;
    ui_str_copy(s_title, sizeof(s_title), d->title ? d->title : "");
    ui_str_copy(s_body, sizeof(s_body), d->body ? d->body : "");
    ui_str_copy(s_confirm, sizeof(s_confirm), d->confirm ? d->confirm : ui_s(S_A_YES));
    s_armed = true;
    ui_nav_push(UI_SCR_DIALOG, 0);
    s_armed = false;    // consumed by the builder; a failed push must not leave it for "ui goto dialog"
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_back();
}

static void confirm_cb(lv_event_t *e)
{
    (void)e;
    ui_cb_t cb = s_d.on_confirm;
    void *ctx = s_d.ctx;
    s_d.on_confirm = NULL;      // a second tap before the async delete must not run it twice
    ui_nav_back();              // the action runs after the dialog has closed (it may navigate)
    if (cb) {
        cb(ctx);
    }
}

static lv_obj_t *stadium(lv_obj_t *scr, const char *name, int32_t x, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(scr);
    ui_theme_plain(b);
    lv_obj_set_name_static(b, name);
    lv_obj_add_style(b, &st_stadium_secondary, 0);
    lv_obj_add_style(b, &st_stadium_secondary_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(b, x, BTN_Y);
    lv_obj_set_size(b, BTN_W, BTN_H);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static void sample_confirm(void *ctx)
{
    (void)ctx;
    ui_toast(ui_s(S_DONE));
}

lv_obj_t *scr_dialog_build(uint32_t arg)
{
    (void)arg;
    if (!s_armed) {
        // "ui goto dialog": a harmless sample
        memset(&s_d, 0, sizeof(s_d));
        s_d.danger = true;
        s_d.on_confirm = sample_confirm;
        ui_str_copy(s_title, sizeof(s_title), ui_s(S_H_CLEAR_Q));
        ui_str_copy(s_body, sizeof(s_body), ui_s(S_H_CLEAR_B));
        ui_str_copy(s_confirm, sizeof(s_confirm), ui_s(S_H_DELETE));
    }
    s_armed = false;
    const ui_accent_t *acc = ui_accent(s_d.danger ? UI_ACC_ERROR : UI_ACC_WARNING);
    lv_obj_t *scr = ui_chrome_screen();
    lv_obj_set_name_static(scr, "dialog");

    lv_obj_t *icon = lv_label_create(scr);
    lv_obj_set_name_static(icon, "dlg_icon");
    lv_obj_add_style(icon, &st_text28, 0);
    lv_obj_set_style_text_color(icon, ui_col(acc->top), 0);
    lv_label_set_text_static(icon, SYM_WARN);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 96 - lv_font_get_line_height(&ui_28) / 2);

    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_name_static(t, "dlg_title");
    lv_obj_add_style(t, &st_text20, 0);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(t, 260, lv_font_get_line_height(&ui_20));
    lv_obj_set_pos(t, 50, 132 - lv_font_get_line_height(&ui_20) / 2);
    lv_label_set_text(t, s_title);

    lv_obj_t *b = lv_label_create(scr);
    lv_obj_set_name_static(b, "dlg_body");
    lv_obj_add_style(b, &st_text16, 0);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);     // three 20 px lines at most
    lv_obj_set_pos(b, 60, 150);
    lv_obj_set_size(b, 240, 60);
    lv_label_set_text(b, s_body);

    stadium(scr, "dlg_cancel", 56, ui_s(S_CANCEL), cancel_cb);
    lv_obj_t *ok = stadium(scr, "dlg_ok", 184, s_confirm, confirm_cb);
    lv_obj_set_style_bg_color(ok, ui_col(acc->top), 0);
    lv_obj_set_style_bg_color(ok, ui_col(acc->mid), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ok, ui_col(UI_COL_INK), 0);
    return scr;
}
