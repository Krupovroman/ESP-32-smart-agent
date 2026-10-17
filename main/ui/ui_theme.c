#include "ui_theme.h"

static const ui_accent_t s_accents[UI_ACC__N] = {
    [UI_ACC_ASSISTANT] = { 0x7C9CFF, 0x617AC7, 0x44568C, 0x252F4D, 0x111624, 0x1B2238 },
    [UI_ACC_LANG_A]    = { 0xFFA94D, 0xC7843C, 0x8C5D2A, 0x4D3317, 0x24180B, 0x382511 },
    [UI_ACC_LANG_B]    = { 0x6EC8FF, 0x569CC7, 0x3C6E8C, 0x213C4D, 0x0F1C24, 0x182C38 },
    [UI_ACC_LISTEN]    = { 0x34E0C3, 0x29AF98, 0x1D7B6B, 0x10433B, 0x071F1B, 0x0B312B },
    [UI_ACC_SPEAK]     = { 0xB794FF, 0x8F73C7, 0x65518C, 0x372C4D, 0x1A1524, 0x282138 },
    [UI_ACC_ERROR]     = { 0xFF5C5C, 0xC74848, 0x8C3333, 0x4D1C1C, 0x240D0D, 0x381414 },
    [UI_ACC_WARNING]   = { 0xFFD166, 0xC7A350, 0x8C7338, 0x4D3F1F, 0x241D0E, 0x382E16 },
    [UI_ACC_SUCCESS]   = { 0x3DDC84, 0x30AC67, 0x227949, 0x124228, 0x091F12, 0x0D301D },
};

lv_style_t st_screen;
lv_style_t st_pill, st_pill_pr;
lv_style_t st_btn_round, st_btn_round_pr;
lv_style_t st_chip, st_chip_a, st_chip_b, st_chip_a_pr, st_chip_b_pr, st_chip_hold;
lv_style_t st_text16, st_text16_1, st_text16_3, st_text20, st_text20_2, st_text28, st_text48;
lv_style_t st_bubble_user, st_reply, st_turn, st_turn_a, st_turn_b, st_divider;
lv_style_t st_card, st_card_btn;
lv_style_t st_fade_top, st_fade_bottom;
lv_style_t st_stadium_primary, st_stadium_primary_pr, st_stadium_secondary, st_stadium_secondary_pr;
lv_style_t st_arc_title, st_arc_title_pr;
lv_style_t st_toast;
lv_style_t st_dot;
lv_style_t st_row, st_row_pr;
lv_style_t st_kb_main, st_key, st_key_pr, st_key_dis;
lv_style_t st_field, st_field_cursor;
lv_style_t st_arc_main, st_arc_ind, st_arc_knob;
lv_style_t st_qr_panel;

// One shared press transition (A26): only the background colour moves, 80 ms linear.
static const lv_style_prop_t s_trans_props[] = { LV_STYLE_BG_COLOR, LV_STYLE_PROP_INV };
static lv_style_transition_dsc_t s_trans_tap;

const ui_accent_t *ui_accent(ui_acc_t a)
{
    return ((unsigned)a < UI_ACC__N) ? &s_accents[a] : &s_accents[UI_ACC_ASSISTANT];
}

static void text_style(lv_style_t *s, const lv_font_t *f, uint32_t col)
{
    lv_style_init(s);
    lv_style_set_text_font(s, f);
    lv_style_set_text_color(s, ui_col(col));
}

static void fill_style(lv_style_t *s, uint32_t col, int32_t radius)
{
    lv_style_init(s);
    lv_style_set_bg_color(s, ui_col(col));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_radius(s, radius);
    lv_style_set_transition(s, &s_trans_tap);
}

static void pressed_style(lv_style_t *s, uint32_t col)
{
    lv_style_init(s);
    lv_style_set_bg_color(s, ui_col(col));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_transition(s, &s_trans_tap);
}

static void fade_style(lv_style_t *s, bool top)
{
    // Opaque black melting into transparent: hides text under the curved viewport edge without layers
    lv_style_init(s);
    lv_style_set_bg_color(s, ui_col(UI_COL_BG));
    lv_style_set_bg_grad_color(s, ui_col(UI_COL_BG));
    lv_style_set_bg_grad_dir(s, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_bg_main_opa(s, top ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_style_set_bg_grad_opa(s, top ? LV_OPA_TRANSP : LV_OPA_COVER);
}

void ui_theme_init(void)
{
    lv_style_transition_dsc_init(&s_trans_tap, s_trans_props, lv_anim_path_linear, UI_T_TAP, 0, NULL);

    lv_style_init(&st_screen);
    lv_style_set_bg_color(&st_screen, ui_col(UI_COL_BG));
    lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);
    lv_style_set_pad_all(&st_screen, 0);
    lv_style_set_border_width(&st_screen, 0);
    lv_style_set_text_font(&st_screen, &ui_20);
    lv_style_set_text_color(&st_screen, ui_col(UI_COL_TEXT1));

    lv_style_init(&st_pill);
    lv_style_set_radius(&st_pill, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&st_pill, ui_col(UI_COL_BG));
    lv_style_set_bg_opa(&st_pill, LV_OPA_TRANSP);
    lv_style_set_text_font(&st_pill, &ui_20);
    lv_style_set_text_color(&st_pill, ui_col(UI_COL_TEXT1));
    pressed_style(&st_pill_pr, UI_COL_SURFACE1);

    fill_style(&st_btn_round, UI_COL_SURFACE1, LV_RADIUS_CIRCLE);
    lv_style_set_text_font(&st_btn_round, &ui_20);
    lv_style_set_text_color(&st_btn_round, ui_col(UI_COL_TEXT2));
    pressed_style(&st_btn_round_pr, UI_COL_SURFACE3);

    fill_style(&st_chip, UI_COL_SURFACE1, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&st_chip, 3);
    lv_style_set_text_font(&st_chip, &ui_20);
    lv_style_set_outline_pad(&st_chip, 1);
    lv_style_init(&st_chip_a);
    lv_style_set_border_color(&st_chip_a, ui_col(s_accents[UI_ACC_LANG_A].top));
    lv_style_set_text_color(&st_chip_a, ui_col(s_accents[UI_ACC_LANG_A].top));
    lv_style_set_outline_color(&st_chip_a, ui_col(s_accents[UI_ACC_LANG_A].top));
    lv_style_init(&st_chip_b);
    lv_style_set_border_color(&st_chip_b, ui_col(s_accents[UI_ACC_LANG_B].top));
    lv_style_set_text_color(&st_chip_b, ui_col(s_accents[UI_ACC_LANG_B].top));
    lv_style_set_outline_color(&st_chip_b, ui_col(s_accents[UI_ACC_LANG_B].top));
    pressed_style(&st_chip_a_pr, s_accents[UI_ACC_LANG_A].dim);
    pressed_style(&st_chip_b_pr, s_accents[UI_ACC_LANG_B].dim);
    lv_style_init(&st_chip_hold);
    lv_style_set_border_width(&st_chip_hold, 5);

    text_style(&st_text16, &ui_16, UI_COL_TEXT2);
    text_style(&st_text16_1, &ui_16, UI_COL_TEXT1);
    text_style(&st_text16_3, &ui_16, UI_COL_TEXT3);
    text_style(&st_text20, &ui_20, UI_COL_TEXT1);
    text_style(&st_text20_2, &ui_20, UI_COL_TEXT2);
    text_style(&st_text28, &ui_28, UI_COL_TEXT1);
    text_style(&st_text48, &ui_48, UI_COL_TEXT1);

    // Chat (3.3, 3.4): user bubble surface-2 r16 pad 8/12; reply has no bubble; translator turn r18 pad 10/12
    fill_style(&st_bubble_user, UI_COL_SURFACE2, 16);
    lv_style_set_pad_ver(&st_bubble_user, 8);
    lv_style_set_pad_hor(&st_bubble_user, 12);
    lv_style_set_text_font(&st_bubble_user, &ui_16);
    lv_style_set_text_color(&st_bubble_user, ui_col(UI_COL_TEXT2));
    text_style(&st_reply, &ui_20, UI_COL_TEXT1);
    fill_style(&st_turn, UI_COL_SURFACE1, 18);
    lv_style_set_pad_ver(&st_turn, 10);
    lv_style_set_pad_hor(&st_turn, 12);
    lv_style_set_pad_row(&st_turn, 2);
    lv_style_init(&st_turn_a);
    lv_style_set_border_side(&st_turn_a, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&st_turn_a, 3);
    lv_style_set_border_color(&st_turn_a, ui_col(s_accents[UI_ACC_LANG_A].top));
    lv_style_init(&st_turn_b);
    lv_style_set_border_side(&st_turn_b, LV_BORDER_SIDE_RIGHT);
    lv_style_set_border_width(&st_turn_b, 3);
    lv_style_set_border_color(&st_turn_b, ui_col(s_accents[UI_ACC_LANG_B].top));
    text_style(&st_divider, &ui_16, UI_COL_TEXT3);
    lv_style_set_text_align(&st_divider, LV_TEXT_ALIGN_CENTER);

    // Error card (3.14): radius 20, padding 14; the status dim fill is set per card
    lv_style_init(&st_card);
    lv_style_set_radius(&st_card, 20);
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_pad_all(&st_card, 14);
    lv_style_set_pad_row(&st_card, 6);
    fill_style(&st_card_btn, UI_COL_SURFACE1, LV_RADIUS_CIRCLE);
    lv_style_set_text_font(&st_card_btn, &ui_20);
    lv_style_set_text_color(&st_card_btn, ui_col(UI_COL_INK));

    fade_style(&st_fade_top, true);
    fade_style(&st_fade_bottom, false);

    fill_style(&st_stadium_primary, s_accents[UI_ACC_ASSISTANT].top, LV_RADIUS_CIRCLE);
    lv_style_set_text_font(&st_stadium_primary, &ui_20);
    lv_style_set_text_color(&st_stadium_primary, ui_col(UI_COL_INK));
    pressed_style(&st_stadium_primary_pr, s_accents[UI_ACC_ASSISTANT].mid);
    fill_style(&st_stadium_secondary, UI_COL_SURFACE1, LV_RADIUS_CIRCLE);
    lv_style_set_text_font(&st_stadium_secondary, &ui_20);
    lv_style_set_text_color(&st_stadium_secondary, ui_col(UI_COL_TEXT1));
    pressed_style(&st_stadium_secondary_pr, UI_COL_SURFACE3);

    text_style(&st_arc_title, &ui_20, UI_COL_TEXT1);
    text_style(&st_arc_title_pr, &ui_20, UI_COL_TEXT2);

    fill_style(&st_toast, UI_COL_SURFACE2, LV_RADIUS_CIRCLE);
    lv_style_set_text_font(&st_toast, &ui_16);
    lv_style_set_text_color(&st_toast, ui_col(UI_COL_TEXT1));
    lv_style_set_pad_hor(&st_toast, 16);
    lv_style_set_pad_ver(&st_toast, 10);

    fill_style(&st_dot, UI_COL_TEXT1, LV_RADIUS_CIRCLE);

    // List rows (3.7): surface-1 r20, pressed surface-3 through the shared 80 ms transition
    fill_style(&st_row, UI_COL_SURFACE1, 20);
    lv_style_set_text_font(&st_row, &ui_20);
    lv_style_set_text_color(&st_row, ui_col(UI_COL_TEXT1));
    pressed_style(&st_row_pr, UI_COL_SURFACE3);

    // Keyboard (4.2): no background or border, pad_all 0, pad_row 3, pad_column 4; keys r10 surface-2 / text-1,
    // pressed surface-3, disabled text-3 (function keys and OK are recoloured per key in ui_kb.c)
    lv_style_init(&st_kb_main);
    lv_style_set_bg_opa(&st_kb_main, LV_OPA_TRANSP);
    lv_style_set_border_width(&st_kb_main, 0);
    lv_style_set_pad_all(&st_kb_main, 0);
    lv_style_set_pad_row(&st_kb_main, 3);
    lv_style_set_pad_column(&st_kb_main, 4);
    fill_style(&st_key, UI_COL_SURFACE2, 10);
    lv_style_set_text_color(&st_key, ui_col(UI_COL_TEXT1));
    lv_style_set_border_width(&st_key, 0);
    pressed_style(&st_key_pr, UI_COL_SURFACE3);
    lv_style_init(&st_key_dis);
    lv_style_set_text_color(&st_key_dis, ui_col(UI_COL_TEXT3));

    // Text field (3.8): surface-2 r14, ui_20, pad 12/14; cursor = 2 px left border in the accent, 500 ms blink
    fill_style(&st_field, UI_COL_SURFACE2, 14);
    lv_style_set_pad_ver(&st_field, 12);
    lv_style_set_pad_hor(&st_field, 14);
    lv_style_set_text_font(&st_field, &ui_20);
    lv_style_set_text_color(&st_field, ui_col(UI_COL_TEXT1));
    lv_style_set_border_width(&st_field, 0);
    lv_style_init(&st_field_cursor);
    lv_style_set_bg_opa(&st_field_cursor, LV_OPA_TRANSP);
    lv_style_set_border_side(&st_field_cursor, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&st_field_cursor, 2);
    lv_style_set_border_color(&st_field_cursor, ui_col(s_accents[UI_ACC_ASSISTANT].top));
    lv_style_set_anim_duration(&st_field_cursor, 500);

    // Arc adjuster (3.9): 16 px surface-2 track, accent indicator (rounded), text-1 knob Ø32 (pad 8)
    lv_style_init(&st_arc_main);
    lv_style_set_arc_width(&st_arc_main, 16);
    lv_style_set_arc_color(&st_arc_main, ui_col(UI_COL_SURFACE2));
    lv_style_set_arc_rounded(&st_arc_main, true);
    lv_style_set_bg_opa(&st_arc_main, LV_OPA_TRANSP);
    lv_style_init(&st_arc_ind);
    lv_style_set_arc_width(&st_arc_ind, 16);
    lv_style_set_arc_color(&st_arc_ind, ui_col(s_accents[UI_ACC_ASSISTANT].top));
    lv_style_set_arc_rounded(&st_arc_ind, true);
    fill_style(&st_arc_knob, UI_COL_TEXT1, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&st_arc_knob, 8);

    // QR panel (3.10): white rounded rectangle, radius 16
    fill_style(&st_qr_panel, 0xFFFFFF, 16);
}

void ui_theme_set_mode(int mode)
{
    const ui_accent_t *a = ui_accent(ui_mode_accent(mode));
    lv_style_set_bg_color(&st_stadium_primary, ui_col(a->top));
    lv_style_set_bg_color(&st_stadium_primary_pr, ui_col(a->mid));
    lv_style_set_border_color(&st_field_cursor, ui_col(a->top));
    lv_style_set_arc_color(&st_arc_ind, ui_col(a->top));
    lv_obj_report_style_change(&st_stadium_primary);
    lv_obj_report_style_change(&st_stadium_primary_pr);
    lv_obj_report_style_change(&st_field_cursor);
    lv_obj_report_style_change(&st_arc_ind);
}

void ui_theme_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER |
                                LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

void ui_theme_chain_ver(lv_obj_t *obj)
{
    // lv_indev_find_scroll_obj() stops at the first non-scrollable object without the chain flag
    for (; obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE); obj = lv_obj_get_parent(obj)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    }
}
