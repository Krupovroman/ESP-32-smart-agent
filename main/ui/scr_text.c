// K, text entry T_TEXT (docs/ui.md 3.8, 4.6): prompt label, cancel circle, one-line field, reveal / "Aa" circle
// and the round keyboard. BOOT is ignored here (ui_boot_down); PWR and the cancel circle leave with the draft.
#include "ui_internal.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

// prompt x 105-255, y 30-50: |(75,150)| = 167.7; circles Ø50 at (76,86) / (284,86): |(104,94)| + 25 = 165.2;
// field x 105-255, y 61-111 (150x50), corners |(75,119)| = 140.7 (r14: 135.4)
#define PROMPT_X    105
#define PROMPT_Y    30
#define PROMPT_W    150
#define FIELD_X     105
#define FIELD_Y     61
#define FIELD_W     150
#define FIELD_H     50
#define CIRCLE_D    50
#define CANCEL_CX   76
#define RIGHT_CX    284
#define CIRCLE_CY   86
#define TEXT_MAX    512

static ui_text_req_t s_req;
static char s_prompt[64];
static char *s_init;            // PSRAM copy of the initial text
static char *s_draft;           // latest text of non-secret fields (handed back on cancel)
static bool s_armed;            // ui_text_open() filled s_req for the next build
static bool s_finished;         // done already called
static lv_obj_t *s_scr, *s_ta, *s_prompt_lbl, *s_right_lbl;

static void str_drop(char **p)
{
    if (*p) {
        memset(*p, 0, strlen(*p));
        heap_caps_free(*p);
        *p = NULL;
    }
}

static char *str_copy(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *d = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (d) {
        memcpy(d, s ? s : "", n + 1);
    }
    return d;
}

void ui_text_open(const ui_text_req_t *r)
{
    if (!r) {
        return;
    }
    s_req = *r;
    ui_str_copy(s_prompt, sizeof(s_prompt), r->prompt ? r->prompt : "");
    s_req.prompt = s_prompt;
    str_drop(&s_init);
    s_init = r->init ? str_copy(r->init) : NULL;
    s_armed = true;
    ui_nav_push(UI_SCR_TEXT, (uint32_t)r->field);
    s_armed = false;    // consumed by the builder; a failed push (stack full) must not leave it for "ui goto"
}

static void prompt_refresh(void)
{
    const char *t = lv_textarea_get_text(s_ta);
    if (s_req.field == UI_FIELD_KEY_HEX && ui_kb_hex()) {
        char buf[48];
        int n = ui_field_hex_count(t);
        snprintf(buf, sizeof(buf), ui_s(S_K_KEY_FMT), n < 0 ? 0 : n);
        lv_label_set_text(s_prompt_lbl, buf);
    } else {
        lv_label_set_text(s_prompt_lbl, s_req.prompt && s_req.prompt[0] ? s_req.prompt : ui_s(S_K_INPUT));
    }
}

static void finish(bool ok)
{
    if (s_finished) {
        return;
    }
    s_finished = true;
    char *text = str_copy(lv_textarea_get_text(s_ta));
    ui_text_done_t done = s_req.done;
    void *ctx = s_req.ctx;
    bool secret = ui_field_secret(s_req.field);
    ui_nav_back();      // K is closed first, so done may open the next screen
    if (done) {
        done(ok ? (text ? text : "") : (secret ? "" : (text ? text : "")), ok, ctx);
    }
    str_drop(&text);
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        finish(true);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        prompt_refresh();
        if (!ui_field_secret(s_req.field)) {
            str_drop(&s_draft);
            s_draft = str_copy(lv_textarea_get_text(s_ta));
        }
    }
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_send_event(s_ta, LV_EVENT_CANCEL, NULL);
    finish(false);
}

static void right_cb(lv_event_t *e)
{
    (void)e;
    if (s_req.field == UI_FIELD_KEY_HEX) {
        // "Aa": hex keypad <-> full EN keyboard for keys of another format (4.1)
        bool to_hex = !ui_kb_hex();
        if (to_hex && ui_field_hex_count(lv_textarea_get_text(s_ta)) < 0) {
            // the EN keyboard may have deleted the prefix, which the hex keypad cannot type: OK would stay off
            lv_textarea_set_text(s_ta, UI_KEY_PREFIX);
        }
        ui_kb_set_hex(to_hex);
        lv_label_set_text_static(s_right_lbl, ui_kb_hex() ? "Aa" : "0-f");
        prompt_refresh();
        return;
    }
    bool hidden = lv_textarea_get_password_mode(s_ta);
    lv_textarea_set_password_mode(s_ta, !hidden);
    lv_label_set_text_static(s_right_lbl, hidden ? SYM_EYE_SLASH : SYM_EYE);
}

static void delete_cb(lv_event_t *e)
{
    // deleted asynchronously: a new K may already have been built (done opened it), whose state this is
    if (lv_event_get_current_target(e) != s_scr) {
        return;
    }
    // every other way out (PWR, BOOT jump, UI_GOTO) is a cancel that keeps the draft for the caller
    if (!s_finished) {
        s_finished = true;
        if (s_req.done) {
            s_req.done(s_draft ? s_draft : "", false, s_req.ctx);
        }
    }
    str_drop(&s_draft);
    str_drop(&s_init);
    s_scr = s_ta = s_prompt_lbl = s_right_lbl = NULL;
}

static void test_done(const char *text, bool ok, void *ctx)
{
    (void)ctx;
    if (ok) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%u)", ui_s(S_DONE), (unsigned)strlen(text));
        ui_toast(buf);
    }
}

static lv_obj_t *circle(lv_obj_t *scr, const char *name, int32_t cx, const char *sym, lv_event_cb_t cb,
                        lv_obj_t **lbl)
{
    lv_obj_t *b = lv_obj_create(scr);
    ui_theme_plain(b);
    lv_obj_set_name_static(b, name);
    lv_obj_add_style(b, &st_btn_round, 0);
    lv_obj_add_style(b, &st_btn_round_pr, LV_STATE_PRESSED);
    lv_obj_set_pos(b, cx - CIRCLE_D / 2, CIRCLE_CY - CIRCLE_D / 2);
    lv_obj_set_size(b, CIRCLE_D, CIRCLE_D);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text_static(l, sym);
    lv_obj_center(l);
    if (lbl) {
        *lbl = l;
    }
    return b;
}

lv_obj_t *scr_text_build(uint32_t arg)
{
    if (!s_armed) {
        // "ui goto text [field]": a scratch field for keyboard tests
        memset(&s_req, 0, sizeof(s_req));
        s_req.field = arg < UI_FIELD__N ? (ui_field_t)arg : UI_FIELD_QUESTION;
        s_req.done = test_done;
        s_prompt[0] = 0;
        s_req.prompt = s_prompt;
        str_drop(&s_init);
    }
    s_armed = false;
    s_finished = false;
    str_drop(&s_draft);

    s_scr = ui_chrome_screen();
    lv_obj_set_name_static(s_scr, "text");
    lv_obj_add_event_cb(s_scr, delete_cb, LV_EVENT_DELETE, NULL);

    s_prompt_lbl = lv_label_create(s_scr);
    lv_obj_set_name_static(s_prompt_lbl, "text_prompt");
    lv_obj_add_style(s_prompt_lbl, &st_text16, 0);
    lv_obj_set_style_text_align(s_prompt_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_prompt_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_prompt_lbl, PROMPT_X, PROMPT_Y);
    lv_obj_set_size(s_prompt_lbl, PROMPT_W, lv_font_get_line_height(&ui_16));

    circle(s_scr, "text_cancel", CANCEL_CX, SYM_TIMES, cancel_cb, NULL);

    s_ta = lv_textarea_create(s_scr);
    lv_obj_remove_style_all(s_ta);
    lv_obj_set_name_static(s_ta, "text_field");
    lv_obj_add_style(s_ta, &st_field, 0);
    lv_obj_add_style(s_ta, &st_field_cursor, LV_PART_CURSOR);
    lv_textarea_set_one_line(s_ta, true);
    lv_obj_set_pos(s_ta, FIELD_X, FIELD_Y);
    lv_obj_set_size(s_ta, FIELD_W, FIELD_H);
    // the field is not clickable, so the cursor always stays at the end (3.8)
    lv_obj_remove_flag(s_ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_scrollbar_mode(s_ta, LV_SCROLLBAR_MODE_OFF);
    lv_textarea_set_max_length(s_ta, TEXT_MAX);
    if (ui_field_secret(s_req.field)) {
        lv_textarea_set_password_bullet(s_ta, TXT_BULLET);
        lv_textarea_set_password_show_time(s_ta, 1500);     // each new character shows for 1.5 s
        lv_textarea_set_password_mode(s_ta, true);
    }
    const char *init = s_init ? s_init : (s_req.field == UI_FIELD_KEY_HEX ? UI_KEY_PREFIX : "");
    lv_textarea_set_text(s_ta, init);
    lv_obj_add_event_cb(s_ta, ta_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ta, ta_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_right_lbl = NULL;
    if (s_req.field == UI_FIELD_KEY_HEX) {
        circle(s_scr, "text_aa", RIGHT_CX, "Aa", right_cb, &s_right_lbl);
    } else if (ui_field_secret(s_req.field)) {
        circle(s_scr, "text_reveal", RIGHT_CX, SYM_EYE, right_cb, &s_right_lbl);
    }

    ui_kb_create(s_scr, s_ta, s_req.field);
    prompt_refresh();
    if (!ui_field_secret(s_req.field)) {
        s_draft = str_copy(init);
    }
    return s_scr;
}
