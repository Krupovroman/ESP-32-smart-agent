#pragma once
// Visual language (docs/ui.md 1): neutral tokens, accents, fonts, motion tokens and the shared styles.
// No LVGL theme is used, so every part a screen shows is styled from here (ui.md 7.5).
#include <stdint.h>
#include "lvgl.h"

LV_FONT_DECLARE(ui_16)
LV_FONT_DECLARE(ui_20)
LV_FONT_DECLARE(ui_28)
LV_FONT_DECLARE(ui_48)

// ---- Neutral tokens (1.1). Glow colours are pre-mixed opaque colours: no large alpha blends.
#define UI_COL_BG       0x000000
#define UI_COL_SURFACE1 0x111316    // rows, function keys, cards, translator bubbles, side buttons
#define UI_COL_SURFACE2 0x1A1D22    // letter keys, user bubble, arc track, text field
#define UI_COL_SURFACE3 0x2C3138    // pressed keys, buttons and rows
#define UI_COL_TEXT1    0xF2F4F7    // primary text
#define UI_COL_TEXT2    0xA6ADB8    // secondary text
#define UI_COL_TEXT3    0x6B7380    // disabled and decorative only
#define UI_COL_INK      0x0B0D10    // text, icons and eyes on bright accent fills
#define UI_COL_OFFLINE  0x6B7380    // outline only

// ---- Accents (1.2): six steps, each a fixed multiple of core-top.
typedef enum {
    UI_ACC_ASSISTANT,   // assistant idle / thinking
    UI_ACC_LANG_A,      // translator mode and language A ("my language")
    UI_ACC_LANG_B,      // language B (the other person)
    UI_ACC_LISTEN,      // microphone open
    UI_ACC_SPEAK,       // device speaking
    UI_ACC_ERROR,
    UI_ACC_WARNING,
    UI_ACC_SUCCESS,
    UI_ACC__N
} ui_acc_t;

typedef struct {
    uint32_t top, bot, mid, halo1, halo2, dim;   // core-top, core-bottom x0.78, x0.55, x0.30, x0.14, dim x0.22
} ui_accent_t;

// Accent table entry (never NULL; out-of-range ids return the assistant accent).
const ui_accent_t *ui_accent(ui_acc_t a);
// Accent of a mode: 0 assistant, 1 translator.
static inline ui_acc_t ui_mode_accent(int mode)
{
    return mode ? UI_ACC_LANG_A : UI_ACC_ASSISTANT;
}
// lv_color_hex shorthand for the tokens above.
static inline lv_color_t ui_col(uint32_t hex)
{
    return lv_color_hex(hex);
}

// ---- Motion tokens (1.6), ms
#define UI_T_TAP        80      // press feedback, linear
#define UI_T_FAST       150     // chrome fades
#define UI_T_BASE       220     // message appear, state entries (ease_out)
#define UI_T_SLOW       360     // orb hero <-> compact
#define UI_T_MORPH      400     // colour morphs
#define UI_T_BREATH     3000    // each way, idle halo breathing
#define UI_T_BLINK_IN   70
#define UI_T_BLINK_HOLD 40
#define UI_T_BLINK_OUT  110
#define UI_TICK_MS      33      // orb ticker, all continuous motion (30 Hz)

// ---- Shared styles (7.5); initialised once by ui_theme_init().
extern lv_style_t st_screen;            // bg #000 cover, pad 0, ui_20 text-1 (inherited)
extern lv_style_t st_pill, st_pill_pr;  // transparent stadium, surface-1 when pressed
extern lv_style_t st_btn_round, st_btn_round_pr;    // Ø52 surface-1 circle, surface-3 pressed
extern lv_style_t st_chip, st_chip_a, st_chip_b;    // language chips: base, A/B colours
extern lv_style_t st_chip_a_pr, st_chip_b_pr;       // pressed / held: language dim fill
extern lv_style_t st_chip_hold;                     // held and recording (LV_STATE_CHECKED): 5 px border
extern lv_style_t st_text16, st_text16_1, st_text16_3, st_text20, st_text20_2, st_text28, st_text48;
extern lv_style_t st_bubble_user, st_reply, st_turn, st_turn_a, st_turn_b, st_divider;
extern lv_style_t st_card, st_card_btn;
extern lv_style_t st_fade_top, st_fade_bottom;
extern lv_style_t st_stadium_primary, st_stadium_primary_pr, st_stadium_secondary, st_stadium_secondary_pr;
extern lv_style_t st_arc_title, st_arc_title_pr;
extern lv_style_t st_toast;
extern lv_style_t st_dot;

// Builds every style above; call once in taskLVGL before any screen is built.
void ui_theme_init(void);
// Re-tints the mode-accent styles (primary stadium) and reports the change to all users (A21).
void ui_theme_set_mode(int mode);
// Removes every style and the scroll / focus flags a plain (non-scrolling) object must not have.
void ui_theme_plain(lv_obj_t *obj);
// Re-enables vertical scroll chaining from obj up to its scrolling ancestor (clickable children of lists).
void ui_theme_chain_ver(lv_obj_t *obj);
