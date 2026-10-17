// HIST (docs/ui.md 3.5): the stored conversation of the current mode in a full-height reader.
#include "ui_chat.h"
#include "ui_internal.h"
#include "ui_rim.h"
#include "ui_str.h"
#include "ui_theme.h"

// Viewport x 54-306, y 70-290 (252x220): corners |(126,110)| = 167.3. Fades y 70-94 and 270-290.
#define VP_X    54
#define VP_Y    70
#define VP_W    252
#define VP_H    220
#define FADE_T  24
#define FADE_B  20

static uint32_t s_scroll_end_t;

static void scroll_end_cb(lv_event_t *e)
{
    (void)e;
    s_scroll_end_t = lv_tick_get();
}

static void msg_click_cb(lv_event_t *e)
{
    if (lv_tick_elaps(s_scroll_end_t) < 300) {
        return;
    }
    const cm_entry_t *en = cm_find(ui_msg_id(lv_event_get_current_target(e)));
    if (en && en->kind == CM_TURN && en->text2) {
        ui_nav_push(UI_SCR_READER, en->id);     // translator bubbles open the reader
    }
}

static void new_chat_cb(lv_event_t *e)
{
    (void)e;
    // the confirm dialog (C) comes with T_DIALOG; until then the action applies directly
    ui_emit(UI_IN_NEW_CHAT, 0);
    ui_nav_back();
    scr_home_clear(true);
}

lv_obj_t *scr_hist_build(uint32_t entry_id)
{
    lv_obj_t *scr = ui_chrome_screen();
    lv_obj_set_name_static(scr, "hist");

    lv_obj_t *list = lv_obj_create(scr);
    ui_theme_plain(list);
    lv_obj_set_name_static(list, "hist_list");
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(list, VP_X, VP_Y);
    lv_obj_set_size(list, VP_W, VP_H);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_style_pad_top(list, FADE_T, 0);
    lv_obj_set_style_pad_bottom(list, FADE_B, 0);
    lv_obj_add_event_cb(list, scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    // first child: "Новый разговор" stadium 200x50 centred (x 80-280)
    lv_obj_t *row = lv_obj_create(list);
    ui_theme_plain(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(row, 8, 0);
    lv_obj_t *btn = lv_obj_create(row);
    ui_theme_plain(btn);
    lv_obj_set_name_static(btn, "hist_new");
    lv_obj_add_style(btn, &st_stadium_secondary, 0);
    lv_obj_add_style(btn, &st_stadium_secondary_pr, LV_STATE_PRESSED);
    lv_obj_set_size(btn, 200, 50);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, new_chat_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text_static(bl, ui_s(S_NEW_CHAT));
    lv_obj_center(bl);

    int m = ui_get(g_s_mode);
    for (int i = 0; i < cm_count(m); i++) {
        ui_msg_create(list, cm_at(m, i), false, 0, msg_click_cb);
    }
    ui_chrome_fades(scr, VP_X, VP_Y, VP_W, VP_H, FADE_T, FADE_B);
    ui_chrome_title(scr, ui_s(S_TITLE_HISTORY));
    ui_chrome_swipe_back(scr);

    // open at the tapped message, else at the newest one
    lv_obj_update_layout(list);
    lv_obj_t *target = entry_id ? ui_msg_find(list, entry_id) : NULL;
    if (target) {
        lv_obj_scroll_to_view(target, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(list, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list), LV_ANIM_OFF);
    }
    ui_rim_attach_scroll(list);
    return scr;
}
