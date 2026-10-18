// Round keyboard (docs/ui.md 4.1-4.6) on lv_keyboard / lv_buttonmatrix, LVGL 9.5.
//
// Geometry (4.2): 324x209 at (18,115), pad_all 0, pad_row 3, pad_column 4 -> four 50 px rows at y 115/168/221/274.
// update_map() puts key i of a row with n keys and u units at x1 = (324 - (n-1)*4) * units_before / u + i*4;
// every x range of 4.3 and the fit proof of 4.4 follow from that formula. Rows taper to the circle with hidden
// spacer keys (label " ": an empty string would end the map). get_button_from_point() grows each key by
// pad/2 + 1 + (pad & 1) = 3 px on every inner side, so a 50x50 key is 56x56 to the finger.
//
// 9.5 facts this relies on (4.5): lv_keyboard_set_map() writes LVGL's file-static tables (one keyboard at a time)
// and re-applies the current mode at once; with popovers off the ctrl map is copied, so runtime DISABLED flags are
// lost on every swap and kb_apply() re-applies them. VALUE_CHANGED fires on PRESSED for keys without CLICK_TRIG
// (backspace, which also repeats) and on RELEASED for CLICK_TRIG keys; swapping the map inside it is safe.
#include "ui_kb.h"
#include "ui_internal.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"   // button_areas for the console planner

#define SP   " "
#define HID  LV_BUTTONMATRIX_CTRL_HIDDEN
#define TAP  (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT)
#define U(n) ((lv_buttonmatrix_ctrl_t)(n))      // width units 1..15

#define KB_X 18
#define KB_Y 115
#define KB_W 324
#define KB_H 209

typedef enum {
    KA_NONE, KA_CHAR, KA_GROUP, KA_BKSP, KA_SPACE, KA_OK, KA_LANG, KA_DIGITS, KA_SYMBOLS, KA_LETTERS, KA_BACK,
    KA__N
} kb_act_t;
typedef enum { KL_L1, KL_L2, KL_PAGE } kb_kind_t;
typedef enum { RET_LETTERS, RET_SYMBOLS } kb_ret_t;     // where a level-2 layout returns after a key or ‹

typedef struct kb_layout {
    const char *name;                           // console
    const char *const *map;
    const lv_buttonmatrix_ctrl_t *ctrl;
    const uint8_t *act;                         // kb_act_t per button index ("\n" not counted)
    const struct kb_layout *const *sub;         // KA_GROUP target per button index
    const lv_font_t *font;
    uint8_t kind, ret, n;
    int8_t ok_id, lang_id;                      // -1 if the layout has none
} kb_layout_t;

static const kb_layout_t KB_RU_L1, KB_EN_L1, KB_DIG_RU, KB_DIG_EN, KB_SYM_RU, KB_SYM_EN, KB_HEX, KB_PUNCT;
static const kb_layout_t KB_RU_G[8], KB_EN_G[8], KB_SYM_G[9];

// ---- Level 1 (letters, digits page, symbols page): the same 20-key shape (4.3)

static const lv_buttonmatrix_ctrl_t ctrl_l1[20] = {
    HID | U(10), TAP | U(13), TAP | U(13), TAP | U(13), HID | U(10),
    TAP | U(10), TAP | U(13), TAP | U(13), TAP | U(13), U(10),     // ⌫ fires on PRESSED and auto-repeats
    HID | U(10), TAP | U(13), TAP | U(13), TAP | U(13), HID | U(10),
    HID | U(13), TAP | U(9), TAP | U(9), TAP | U(9), HID | U(13),
};

static const char *const map_ru_l1[] = {
    SP, "абвг", "деёжз", "ийкл", SP, "\n",
    "123", "мноп", "рсту", "фхцч", SYM_BACKSPACE, "\n",
    SP, "шщъы", "ьэюя", "пробел", SP, "\n",
    SP, "EN", ".,?!", SYM_CHECK, SP, "",
};
static const char *const map_en_l1[] = {
    SP, "abc", "def", "ghi", SP, "\n",
    "123", "jkl", "mno", "pqrs", SYM_BACKSPACE, "\n",
    SP, "tuv", "wxyz", "space", SP, "\n",
    SP, "RU", ".,?!", SYM_CHECK, SP, "",
};
static const uint8_t act_l1[20] = {
    KA_NONE, KA_GROUP, KA_GROUP, KA_GROUP, KA_NONE,
    KA_DIGITS, KA_GROUP, KA_GROUP, KA_GROUP, KA_BKSP,
    KA_NONE, KA_GROUP, KA_GROUP, KA_SPACE, KA_NONE,
    KA_NONE, KA_LANG, KA_GROUP, KA_OK, KA_NONE,
};
static const kb_layout_t *const sub_ru_l1[20] = {
    NULL, &KB_RU_G[0], &KB_RU_G[1], &KB_RU_G[2], NULL,
    NULL, &KB_RU_G[3], &KB_RU_G[4], &KB_RU_G[5], NULL,
    NULL, &KB_RU_G[6], &KB_RU_G[7], NULL, NULL,
    NULL, NULL, &KB_PUNCT, NULL, NULL,
};
static const kb_layout_t *const sub_en_l1[20] = {
    NULL, &KB_EN_G[0], &KB_EN_G[1], &KB_EN_G[2], NULL,
    NULL, &KB_EN_G[3], &KB_EN_G[4], &KB_EN_G[5], NULL,
    NULL, &KB_EN_G[6], &KB_EN_G[7], NULL, NULL,
    NULL, NULL, &KB_PUNCT, NULL, NULL,
};

// Digits page: single taps (ui_28 digits; the page keys are drawn in ui_20, kb_draw_cb)
static const char *const map_dig_ru[] = {
    SP, "1", "2", "3", SP, "\n",
    "абв", "4", "5", "6", SYM_BACKSPACE, "\n",
    SP, "7", "8", "9", SP, "\n",
    SP, "#+=", "0", SYM_CHECK, SP, "",
};
static const char *const map_dig_en[] = {
    SP, "1", "2", "3", SP, "\n",
    "abc", "4", "5", "6", SYM_BACKSPACE, "\n",
    SP, "7", "8", "9", SP, "\n",
    SP, "#+=", "0", SYM_CHECK, SP, "",
};
static const uint8_t act_dig[20] = {
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_LETTERS, KA_CHAR, KA_CHAR, KA_CHAR, KA_BKSP,
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_SYMBOLS, KA_CHAR, KA_OK, KA_NONE,
};

// Symbols level 1: all 32 printable ASCII symbols plus « » — … in nine two-step groups
static const char *const map_sym_ru[] = {
    SP, "!?.,", "@#$%", "&*^~", SP, "\n",
    "абв", "-_=+", "()[]", "{}<>", SYM_BACKSPACE, "\n",
    SP, "/\\|`", ":;'\"", "пробел", SP, "\n",
    SP, "123", "«»—…", SYM_CHECK, SP, "",
};
static const char *const map_sym_en[] = {
    SP, "!?.,", "@#$%", "&*^~", SP, "\n",
    "abc", "-_=+", "()[]", "{}<>", SYM_BACKSPACE, "\n",
    SP, "/\\|`", ":;'\"", "space", SP, "\n",
    SP, "123", "«»—…", SYM_CHECK, SP, "",
};
static const uint8_t act_sym[20] = {
    KA_NONE, KA_GROUP, KA_GROUP, KA_GROUP, KA_NONE,
    KA_LETTERS, KA_GROUP, KA_GROUP, KA_GROUP, KA_BKSP,
    KA_NONE, KA_GROUP, KA_GROUP, KA_SPACE, KA_NONE,
    KA_NONE, KA_DIGITS, KA_GROUP, KA_OK, KA_NONE,
};
static const kb_layout_t *const sub_sym[20] = {
    NULL, &KB_SYM_G[0], &KB_SYM_G[1], &KB_SYM_G[2], NULL,
    NULL, &KB_SYM_G[3], &KB_SYM_G[4], &KB_SYM_G[5], NULL,
    NULL, &KB_SYM_G[6], &KB_SYM_G[7], NULL, NULL,
    NULL, NULL, &KB_SYM_G[8], NULL, NULL,
};

// ---- Level 2: lower case in R1, upper case in R2, ‹ and the group's digit in R3, one hidden key in R4

#define L2_3(a, b, c, A, B, C, d) \
    { SP, a, b, c, SP, "\n", SP, A, B, C, SP, "\n", SP, SYM_LEFT, d, SP, "\n", SP, "" }
#define L2_4(a, b, c, e, A, B, C, E, d) \
    { SP, a, b, c, e, SP, "\n", SP, A, B, C, E, SP, "\n", SP, SYM_LEFT, d, SP, "\n", SP, "" }
#define L2_5(a, b, c, e, f, A, B, C, E, F, d) \
    { SP, a, b, c, e, f, SP, "\n", SP, A, B, C, E, F, SP, "\n", SP, SYM_LEFT, d, SP, "\n", SP, "" }
#define L2_S(a, b, c, e) \
    { SP, a, b, c, e, SP, "\n", SP, "\n", SP, SYM_LEFT, SP, SP, "\n", SP, "" }

// 3 letters: h6, 14, 14, 14, h6 -> 56-136 · 140-219 · 223-303; R3 h15, 12, 12, h15 -> ‹ 108-178 · digit 182-251
static const lv_buttonmatrix_ctrl_t ctrl_l2_3[15] = {
    HID | U(6), TAP | U(14), TAP | U(14), TAP | U(14), HID | U(6),
    HID | U(6), TAP | U(14), TAP | U(14), TAP | U(14), HID | U(6),
    HID | U(15), TAP | U(12), TAP | U(12), HID | U(15),
    HID | U(1),
};
static const uint8_t act_l2_3[15] = {
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_BACK, KA_CHAR, KA_NONE,
    KA_NONE,
};
// 4 letters: h2, 12 x4, h2 -> 33-103 · 107-178 · 182-252 · 256-326
static const lv_buttonmatrix_ctrl_t ctrl_l2_4[17] = {
    HID | U(2), TAP | U(12), TAP | U(12), TAP | U(12), TAP | U(12), HID | U(2),
    HID | U(2), TAP | U(12), TAP | U(12), TAP | U(12), TAP | U(12), HID | U(2),
    HID | U(15), TAP | U(12), TAP | U(12), HID | U(15),
    HID | U(1),
};
static const uint8_t act_l2_4[17] = {
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_BACK, KA_CHAR, KA_NONE,
    KA_NONE,
};
// 5 keys (деёжз, punctuation): h2, 11 x5, h2 -> 32-88 · 92-148 · 152-207 · 211-267 · 271-327
static const lv_buttonmatrix_ctrl_t ctrl_l2_5[19] = {
    HID | U(2), TAP | U(11), TAP | U(11), TAP | U(11), TAP | U(11), TAP | U(11), HID | U(2),
    HID | U(2), TAP | U(11), TAP | U(11), TAP | U(11), TAP | U(11), TAP | U(11), HID | U(2),
    HID | U(15), TAP | U(12), TAP | U(12), HID | U(15),
    HID | U(1),
};
static const uint8_t act_l2_5[19] = {
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_BACK, KA_CHAR, KA_NONE,
    KA_NONE,
};
// Symbol group: R1 as 4 letters, R2 one hidden key, R3 h15, 12, h12, h15 -> ‹ 108-178
static const lv_buttonmatrix_ctrl_t ctrl_l2_sym[12] = {
    HID | U(2), TAP | U(12), TAP | U(12), TAP | U(12), TAP | U(12), HID | U(2),
    HID | U(1),
    HID | U(15), TAP | U(12), HID | U(12), HID | U(15),
    HID | U(1),
};
static const uint8_t act_l2_sym[12] = {
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE,
    KA_NONE, KA_BACK, KA_NONE, KA_NONE,
    KA_NONE,
};

static const char *const map_ru_g0[] = L2_4("а", "б", "в", "г", "А", "Б", "В", "Г", "2");
static const char *const map_ru_g1[] = L2_5("д", "е", "ё", "ж", "з", "Д", "Е", "Ё", "Ж", "З", "3");
static const char *const map_ru_g2[] = L2_4("и", "й", "к", "л", "И", "Й", "К", "Л", "4");
static const char *const map_ru_g3[] = L2_4("м", "н", "о", "п", "М", "Н", "О", "П", "5");
static const char *const map_ru_g4[] = L2_4("р", "с", "т", "у", "Р", "С", "Т", "У", "6");
static const char *const map_ru_g5[] = L2_4("ф", "х", "ц", "ч", "Ф", "Х", "Ц", "Ч", "7");
static const char *const map_ru_g6[] = L2_4("ш", "щ", "ъ", "ы", "Ш", "Щ", "Ъ", "Ы", "8");
static const char *const map_ru_g7[] = L2_4("ь", "э", "ю", "я", "Ь", "Э", "Ю", "Я", "9");
static const char *const map_en_g0[] = L2_3("a", "b", "c", "A", "B", "C", "2");
static const char *const map_en_g1[] = L2_3("d", "e", "f", "D", "E", "F", "3");
static const char *const map_en_g2[] = L2_3("g", "h", "i", "G", "H", "I", "4");
static const char *const map_en_g3[] = L2_3("j", "k", "l", "J", "K", "L", "5");
static const char *const map_en_g4[] = L2_3("m", "n", "o", "M", "N", "O", "6");
static const char *const map_en_g5[] = L2_4("p", "q", "r", "s", "P", "Q", "R", "S", "7");
static const char *const map_en_g6[] = L2_3("t", "u", "v", "T", "U", "V", "8");
static const char *const map_en_g7[] = L2_4("w", "x", "y", "z", "W", "X", "Y", "Z", "9");
static const char *const map_punct[] = L2_5(".", ",", "?", "!", "-", ":", ";", "(", ")", "\"", "1");
static const char *const map_sym_g0[] = L2_S("!", "?", ".", ",");
static const char *const map_sym_g1[] = L2_S("@", "#", "$", "%");
static const char *const map_sym_g2[] = L2_S("&", "*", "^", "~");
static const char *const map_sym_g3[] = L2_S("-", "_", "=", "+");
static const char *const map_sym_g4[] = L2_S("(", ")", "[", "]");
static const char *const map_sym_g5[] = L2_S("{", "}", "<", ">");
static const char *const map_sym_g6[] = L2_S("/", "\\", "|", "`");
static const char *const map_sym_g7[] = L2_S(":", ";", "'", "\"");
static const char *const map_sym_g8[] = L2_S("«", "»", "—", "…");

// ---- Hex keypad (4.3): "sk-or-v1-" + 64 hex digits, single taps
static const char *const map_hex[] = {
    SP, "1", "2", "3", "4", "5", SP, "\n",
    "6", "7", "8", "9", "0", SYM_BACKSPACE, "\n",
    SP, "a", "b", "c", "d", SP, "\n",
    SP, "e", "f", SYM_CHECK, SP, "",
};
static const lv_buttonmatrix_ctrl_t ctrl_hex[24] = {
    HID | U(1), TAP | U(12), TAP | U(12), TAP | U(12), TAP | U(12), TAP | U(12), HID | U(1),
    TAP | U(1), TAP | U(1), TAP | U(1), TAP | U(1), TAP | U(1), U(1),
    HID | U(3), TAP | U(8), TAP | U(8), TAP | U(8), TAP | U(8), HID | U(3),
    HID | U(13), TAP | U(9), TAP | U(9), TAP | U(9), HID | U(13),
};
static const uint8_t act_hex[24] = {
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_BKSP,
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_CHAR, KA_CHAR, KA_OK, KA_NONE,
};

// ---- Layout table
#define LAY1(nm, m, a, s) { nm, m, ctrl_l1, a, s, &ui_20, KL_L1, RET_LETTERS, 20, 18, 16 }
#define PAGE(nm, m, a, s, f) { nm, m, ctrl_l1, a, s, f, KL_PAGE, RET_LETTERS, 20, 18, -1 }
#define G3(nm, m) { nm, m, ctrl_l2_3, act_l2_3, NULL, &ui_28, KL_L2, RET_LETTERS, 15, -1, -1 }
#define G4(nm, m) { nm, m, ctrl_l2_4, act_l2_4, NULL, &ui_28, KL_L2, RET_LETTERS, 17, -1, -1 }
#define G5(nm, m) { nm, m, ctrl_l2_5, act_l2_5, NULL, &ui_28, KL_L2, RET_LETTERS, 19, -1, -1 }
#define GS(nm, m) { nm, m, ctrl_l2_sym, act_l2_sym, NULL, &ui_28, KL_L2, RET_SYMBOLS, 12, -1, -1 }

static const kb_layout_t KB_RU_L1 = LAY1("ru", map_ru_l1, act_l1, sub_ru_l1);
static const kb_layout_t KB_EN_L1 = LAY1("en", map_en_l1, act_l1, sub_en_l1);
static const kb_layout_t KB_DIG_RU = PAGE("digits", map_dig_ru, act_dig, NULL, &ui_28);
static const kb_layout_t KB_DIG_EN = PAGE("digits", map_dig_en, act_dig, NULL, &ui_28);
static const kb_layout_t KB_SYM_RU = PAGE("symbols", map_sym_ru, act_sym, sub_sym, &ui_20);
static const kb_layout_t KB_SYM_EN = PAGE("symbols", map_sym_en, act_sym, sub_sym, &ui_20);
static const kb_layout_t KB_HEX = { "hex", map_hex, ctrl_hex, act_hex, NULL, &ui_28, KL_PAGE, RET_LETTERS, 24, 22, -1 };
static const kb_layout_t KB_PUNCT = G5("punct", map_punct);
static const kb_layout_t KB_RU_G[8] = {
    G4("ru_abvg", map_ru_g0), G5("ru_dejo", map_ru_g1), G4("ru_ijkl", map_ru_g2), G4("ru_mnop", map_ru_g3),
    G4("ru_rstu", map_ru_g4), G4("ru_fhcc", map_ru_g5), G4("ru_shsh", map_ru_g6), G4("ru_jeja", map_ru_g7),
};
static const kb_layout_t KB_EN_G[8] = {
    G3("en_abc", map_en_g0), G3("en_def", map_en_g1), G3("en_ghi", map_en_g2), G3("en_jkl", map_en_g3),
    G3("en_mno", map_en_g4), G4("en_pqrs", map_en_g5), G3("en_tuv", map_en_g6), G4("en_wxyz", map_en_g7),
};
static const kb_layout_t KB_SYM_G[9] = {
    GS("sym_0", map_sym_g0), GS("sym_1", map_sym_g1), GS("sym_2", map_sym_g2), GS("sym_3", map_sym_g3),
    GS("sym_4", map_sym_g4), GS("sym_5", map_sym_g5), GS("sym_6", map_sym_g6), GS("sym_7", map_sym_g7),
    GS("sym_8", map_sym_g8),
};

// ---- Field rules (4.6)

typedef struct {
    uint16_t max_bytes;
    uint8_t min_len;
    bool ascii, secret, en_start;
} field_rule_t;

static const field_rule_t s_rules[UI_FIELD__N] = {
    [UI_FIELD_QUESTION] = { 500, 1, false, false, false },
    [UI_FIELD_WIFI_PASS] = { 63, 8, true, true, true },
    [UI_FIELD_SSID] = { 32, 1, false, false, true },
    [UI_FIELD_KEY_HEX] = { 200, 1, true, false, true },
    [UI_FIELD_KEY] = { 200, 1, true, true, true },
    [UI_FIELD_URL] = { 128, 9, true, false, true },
    [UI_FIELD_MODEL] = { 96, 1, true, false, true },
    [UI_FIELD_WIFI_PASS_OPT] = { 63, 8, true, true, true },
};

static const field_rule_t *rule(ui_field_t f)
{
    return &s_rules[(unsigned)f < UI_FIELD__N ? f : UI_FIELD_QUESTION];
}

bool ui_field_secret(ui_field_t f)
{
    return rule(f)->secret;
}

static bool is_hex(uint32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

int ui_field_hex_count(const char *text)
{
    size_t pl = strlen(UI_KEY_PREFIX);
    if (!text || strncmp(text, UI_KEY_PREFIX, pl) != 0) {
        return -1;
    }
    return (int)strlen(text + pl);
}

bool ui_field_ok(ui_field_t f, bool hex, const char *text)
{
    size_t n = text ? strlen(text) : 0;
    if (f == UI_FIELD_KEY_HEX && hex) {
        if (ui_field_hex_count(text) != UI_KEY_HEX_LEN) {
            return false;
        }
        for (const char *p = text + strlen(UI_KEY_PREFIX); *p; p++) {
            if (!is_hex((unsigned char)*p)) {
                return false;
            }
        }
        return true;
    }
    if (f == UI_FIELD_URL) {
        return n > 8 && strncmp(text, "https://", 8) == 0;
    }
    if (f == UI_FIELD_QUESTION) {
        for (size_t i = 0; i < n; i++) {
            if (text[i] != ' ') {
                return true;
            }
        }
        return false;
    }
    if (f == UI_FIELD_WIFI_PASS_OPT && n == 0) {
        return true;    // no password: join it as an open network (4.6 "or the network is open")
    }
    return n >= rule(f)->min_len;
}

// ---- State

static struct {
    lv_obj_t *kb, *ta;
    const kb_layout_t *cur, *letters;
    ui_field_t field;
    bool hex;
} s;

static bool ascii_only(void)
{
    return rule(s.field)->ascii;
}

static const kb_layout_t *digits_of(const kb_layout_t *letters)
{
    return letters == &KB_RU_L1 ? &KB_DIG_RU : &KB_DIG_EN;
}

static const kb_layout_t *symbols_of(const kb_layout_t *letters)
{
    return letters == &KB_RU_L1 ? &KB_SYM_RU : &KB_SYM_EN;
}

static const kb_layout_t *return_of(const kb_layout_t *L, const kb_layout_t *letters)
{
    return L->ret == RET_SYMBOLS ? symbols_of(letters) : letters;
}

// The ctrl map was memcpy'd by the swap: re-apply the runtime flags (0.3 item 2)
static void kb_refresh_dynamic(void)
{
    const kb_layout_t *L = s.cur;
    if (!s.kb || !L) {
        return;
    }
    if (L->ok_id >= 0) {
        if (ui_field_ok(s.field, s.hex, lv_textarea_get_text(s.ta))) {
            lv_buttonmatrix_clear_button_ctrl(s.kb, (uint32_t)L->ok_id, LV_BUTTONMATRIX_CTRL_DISABLED);
        } else {
            lv_buttonmatrix_set_button_ctrl(s.kb, (uint32_t)L->ok_id, LV_BUTTONMATRIX_CTRL_DISABLED);
        }
    }
    if (L->lang_id >= 0 && ascii_only()) {
        lv_buttonmatrix_set_button_ctrl(s.kb, (uint32_t)L->lang_id, LV_BUTTONMATRIX_CTRL_DISABLED);
    }
}

static void kb_apply(const kb_layout_t *L)
{
    s.cur = L;
    lv_obj_set_style_text_font(s.kb, L->font, LV_PART_ITEMS);
    lv_keyboard_set_map(s.kb, LV_KEYBOARD_MODE_USER_1, L->map, L->ctrl);    // applies at once (mode USER_1)
    // the old index would point at another key of the new map
    lv_buttonmatrix_set_selected_button(s.kb, LV_BUTTONMATRIX_BUTTON_NONE);
    kb_refresh_dynamic();
}

static uint32_t utf8_cp(const char *t, size_t *len)
{
    const unsigned char *p = (const unsigned char *)t;
    uint32_t c = p[0];
    size_t n = 1;
    if (c >= 0xF0 && p[1] && p[2] && p[3]) {
        c = ((c & 7u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        n = 4;
    } else if (c >= 0xE0 && p[1] && p[2]) {
        c = ((c & 0xFu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        n = 3;
    } else if (c >= 0xC0 && p[1]) {
        c = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        n = 2;
    }
    if (len) {
        *len = n;
    }
    return c;
}

// Inserts one key's text if the field accepts it (character class, byte limit, 64 hex digits)
static void field_insert(const char *txt)
{
    if (!txt || !txt[0]) {
        return;
    }
    const field_rule_t *r = rule(s.field);
    uint32_t cp = utf8_cp(txt, NULL);
    const char *cur = lv_textarea_get_text(s.ta);
    size_t len = strlen(cur), add = strlen(txt);
    bool hex = s.field == UI_FIELD_KEY_HEX && s.hex;
    if ((r->ascii && (cp < 0x20 || cp > 0x7E)) || (hex && !is_hex(cp))) {
        return;
    }
    if (len + add > r->max_bytes) {
        return;
    }
    if (hex && ui_field_hex_count(cur) >= UI_KEY_HEX_LEN) {
        return;
    }
    lv_textarea_add_text(s.ta, txt);
}

static void field_backspace(void)
{
    const char *cur = lv_textarea_get_text(s.ta);
    // the hex keypad never deletes into the protected "sk-or-v1-" prefix
    if (s.field == UI_FIELD_KEY_HEX && s.hex && ui_field_hex_count(cur) == 0) {
        return;
    }
    lv_textarea_delete_char(s.ta);
}

static void kb_value_cb(lv_event_t *e)
{
    const uint32_t *pid = lv_event_get_param(e);
    const kb_layout_t *L = s.cur;
    if (!pid || !L || *pid >= L->n) {
        return;
    }
    uint32_t id = *pid;
    switch ((kb_act_t)L->act[id]) {
    case KA_CHAR:
        field_insert(lv_buttonmatrix_get_button_text(s.kb, id));
        if (L->kind == KL_L2) {
            kb_apply(return_of(L, s.letters));      // a typed letter goes straight back to level 1
        } else {
            kb_refresh_dynamic();
        }
        break;
    case KA_GROUP:
        if (L->sub && L->sub[id]) {
            kb_apply(L->sub[id]);
        }
        break;
    case KA_BKSP:
        field_backspace();
        kb_refresh_dynamic();
        break;
    case KA_SPACE:
        field_insert(" ");
        kb_refresh_dynamic();
        break;
    case KA_OK:
        if (ui_field_ok(s.field, s.hex, lv_textarea_get_text(s.ta))) {
            lv_obj_send_event(s.ta, LV_EVENT_READY, NULL);
        }
        break;
    case KA_LANG:
        s.letters = s.letters == &KB_RU_L1 ? &KB_EN_L1 : &KB_RU_L1;
        kb_apply(s.letters);
        break;
    case KA_DIGITS: kb_apply(digits_of(s.letters)); break;
    case KA_SYMBOLS: kb_apply(symbols_of(s.letters)); break;
    case KA_LETTERS: kb_apply(s.letters); break;
    case KA_BACK: kb_apply(return_of(L, s.letters)); break;
    default: break;
    }
}

static bool text_fn_key(kb_act_t a)
{
    return a == KA_LANG || a == KA_DIGITS || a == KA_SYMBOLS || a == KA_LETTERS;
}

// Per-key colours and fonts without extra objects (4.5): function keys surface-1 / text-2, OK accent with ink,
// disabled OK surface-1 / text-3, "пробел" in ui_16, page keys of the ui_28 digits page in ui_20
static void kb_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *b = lv_draw_task_get_draw_dsc(t);
    if (!b || b->part != LV_PART_ITEMS || !s.cur || b->id1 >= s.cur->n) {
        return;
    }
    kb_act_t a = (kb_act_t)s.cur->act[b->id1];
    lv_draw_task_type_t type = lv_draw_task_get_type(t);
    if (a == KA_GROUP && (s.cur == &KB_SYM_RU || s.cur == &KB_SYM_EN) && type == LV_DRAW_TASK_TYPE_LABEL) {
        // "@#$%" is 66 px in ui_20 on a 67 px key: the symbols page draws its group labels in ui_16 (52 px)
        lv_draw_label_dsc_t *l = lv_draw_task_get_label_dsc(t);
        l->ofs_y += (lv_font_get_line_height(l->font) - lv_font_get_line_height(&ui_16)) / 2;
        l->font = &ui_16;
        l->align = LV_TEXT_ALIGN_CENTER;
        return;
    }
    if (a == KA_CHAR || a == KA_GROUP || a == KA_NONE) {
        return;     // shared key styles
    }
    bool dis = lv_buttonmatrix_has_button_ctrl(s.kb, b->id1, LV_BUTTONMATRIX_CTRL_DISABLED);
    bool pr = lv_obj_has_state(s.kb, LV_STATE_PRESSED) && lv_buttonmatrix_get_selected_button(s.kb) == b->id1;
    const ui_accent_t *acc = ui_accent(ui_mode_accent(ui_get(g_s_mode)));
    if (type == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_fill_dsc_t *f = lv_draw_task_get_fill_dsc(t);
        if (a == KA_OK && !dis) {
            f->color = ui_col(pr ? acc->mid : acc->top);
        } else if (!pr) {
            f->color = ui_col(UI_COL_SURFACE1);
        }
    } else if (type == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_label_dsc_t *l = lv_draw_task_get_label_dsc(t);
        if (a == KA_OK && !dis) {
            l->color = ui_col(UI_COL_INK);
        } else if (!dis) {
            l->color = ui_col(UI_COL_TEXT2);
        }
        const lv_font_t *nf = a == KA_SPACE ? &ui_16 : (text_fn_key(a) && l->font != &ui_20) ? &ui_20 : NULL;
        if (nf && nf != l->font) {
            // the label box was measured with the layout font: centre the smaller text inside it
            l->ofs_y += (lv_font_get_line_height(l->font) - lv_font_get_line_height(nf)) / 2;
            l->font = nf;
            l->align = LV_TEXT_ALIGN_CENTER;
        }
    }
}

static void kb_delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s.kb) {
        memset(&s, 0, sizeof(s));
    }
}

lv_obj_t *ui_kb_create(lv_obj_t *scr, lv_obj_t *ta, ui_field_t field)
{
    s.kb = lv_keyboard_create(scr);
    s.ta = ta;
    s.field = (unsigned)field < UI_FIELD__N ? field : UI_FIELD_QUESTION;
    s.hex = s.field == UI_FIELD_KEY_HEX;
    lv_obj_set_name_static(s.kb, "kb");
    lv_obj_remove_event_cb(s.kb, lv_keyboard_def_event_cb);   // our handler inserts, filters and switches maps
    lv_keyboard_set_popovers(s.kb, false);                     // popovers would be clipped by the bezel
    lv_keyboard_set_textarea(s.kb, ta);                        // adds FOCUSED to ta, so its cursor shows
    lv_keyboard_set_mode(s.kb, LV_KEYBOARD_MODE_USER_1);
    lv_obj_remove_style_all(s.kb);
    lv_obj_add_style(s.kb, &st_kb_main, 0);
    lv_obj_add_style(s.kb, &st_key, LV_PART_ITEMS);
    lv_obj_add_style(s.kb, &st_key_pr, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_style(s.kb, &st_key_dis, LV_PART_ITEMS | LV_STATE_DISABLED);
    // NOT set_pos: the constructor aligned it BOTTOM_MID (0.3 item 3)
    lv_obj_align(s.kb, LV_ALIGN_TOP_LEFT, KB_X, KB_Y);
    lv_obj_set_size(s.kb, KB_W, KB_H);
    lv_obj_remove_flag(s.kb, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s.kb, kb_value_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s.kb, kb_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(s.kb, kb_delete_cb, LV_EVENT_DELETE, NULL);
    bool ru = s.field == UI_FIELD_QUESTION ? ui_str_lang() == 0 : !rule(s.field)->en_start;
    s.letters = ru ? &KB_RU_L1 : &KB_EN_L1;
    kb_apply(s.hex ? &KB_HEX : s.letters);
    return s.kb;
}

void ui_kb_set_hex(bool hex)
{
    if (!s.kb || s.field != UI_FIELD_KEY_HEX || hex == s.hex) {
        return;
    }
    s.hex = hex;
    s.letters = &KB_EN_L1;
    kb_apply(hex ? &KB_HEX : s.letters);
}

bool ui_kb_hex(void)
{
    return s.kb && s.hex;
}

void ui_kb_refresh(void)
{
    kb_refresh_dynamic();
}

// ---- Console helpers

static bool key_visible(const kb_layout_t *L, uint32_t id)
{
    return !(L->ctrl[id] & LV_BUTTONMATRIX_CTRL_HIDDEN) && L->act[id] != KA_NONE;
}

// Button index of a KA_CHAR key whose label is u8 (or the space bar for " "), -1 if none
static int find_char(const kb_layout_t *L, const char *u8)
{
    uint32_t id = 0;
    for (const char *const *m = L->map; **m; m++) {
        if (!strcmp(*m, "\n")) {
            continue;
        }
        if (id < L->n && ((L->act[id] == KA_CHAR && !strcmp(*m, u8)) || (L->act[id] == KA_SPACE && !strcmp(u8, " ")))) {
            return (int)id;
        }
        id++;
    }
    return -1;
}

static const char *label_of(const kb_layout_t *L, uint32_t id)
{
    uint32_t i = 0;
    for (const char *const *m = L->map; **m; m++) {
        if (!strcmp(*m, "\n")) {
            continue;
        }
        if (i++ == id) {
            return *m;
        }
    }
    return "";
}

static void key_centre(uint32_t id, lv_point_t *p)
{
    lv_buttonmatrix_t *bm = (lv_buttonmatrix_t *)s.kb;
    lv_area_t c;
    lv_obj_get_coords(s.kb, &c);
    const lv_area_t *a = &bm->button_areas[id];
    p->x = c.x1 + (a->x1 + a->x2) / 2;
    p->y = c.y1 + (a->y1 + a->y2) / 2;
}

// Where key id of (L, letters) leads; false for keys that do not change the layout
static bool next_state(const kb_layout_t *L, const kb_layout_t *letters, uint32_t id, const kb_layout_t **nL,
                       const kb_layout_t **nlet)
{
    *nlet = letters;
    switch ((kb_act_t)L->act[id]) {
    case KA_GROUP: *nL = L->sub ? L->sub[id] : NULL; break;
    case KA_LANG:
        if (ascii_only()) {
            return false;
        }
        *nlet = letters == &KB_RU_L1 ? &KB_EN_L1 : &KB_RU_L1;
        *nL = *nlet;
        break;
    case KA_DIGITS: *nL = digits_of(letters); break;
    case KA_SYMBOLS: *nL = symbols_of(letters); break;
    case KA_LETTERS: *nL = letters; break;
    case KA_BACK: *nL = return_of(L, letters); break;
    default: return false;
    }
    return *nL != NULL;
}

ui_kb_plan_t ui_kb_plan(uint32_t cp, lv_point_t *p)
{
    if (!s.kb || !s.cur) {
        return UI_KB_NONE;
    }
    char u8[5] = { 0 };
    if (cp < 0x80) {
        u8[0] = (char)cp;
    } else if (cp < 0x800) {
        u8[0] = (char)(0xC0 | (cp >> 6));
        u8[1] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        u8[0] = (char)(0xE0 | (cp >> 12));
        u8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u8[2] = (char)(0x80 | (cp & 0x3F));
    } else {
        return UI_KB_NONE;
    }
    int id = find_char(s.cur, u8);
    if (id >= 0) {
        key_centre((uint32_t)id, p);
        return UI_KB_TYPES;
    }
    if (s.cur == &KB_HEX) {
        return s.field == UI_FIELD_KEY_HEX ? UI_KB_AA : UI_KB_NONE;
    }
    // breadth-first over (layout, letters) states; the first key of the shortest path is the next tap
    typedef struct {
        const kb_layout_t *L, *let;
        int first;
    } bfs_t;
    bfs_t q[48];
    int head = 0, tail = 0;
    q[tail++] = (bfs_t){ s.cur, s.letters, -1 };
    while (head < tail) {
        bfs_t n = q[head++];
        for (uint32_t k = 0; k < n.L->n; k++) {
            const kb_layout_t *nL, *nlet;
            if (!key_visible(n.L, k) || !next_state(n.L, n.let, k, &nL, &nlet)) {
                continue;
            }
            bool seen = false;
            for (int i = 0; i < tail && !seen; i++) {
                seen = q[i].L == nL && q[i].let == nlet;
            }
            if (seen) {
                continue;
            }
            int first = n.first < 0 ? (int)k : n.first;
            if (find_char(nL, u8) >= 0) {
                key_centre((uint32_t)first, p);
                return UI_KB_STEP;
            }
            if (tail < (int)(sizeof(q) / sizeof(q[0]))) {
                q[tail++] = (bfs_t){ nL, nlet, first };
            }
        }
    }
    return UI_KB_NONE;
}

static const char *const s_act_names[KA__N] = {
    "-", "char", "group", "bksp", "space", "ok", "lang", "digits", "symbols", "letters", "back",
};

bool ui_kb_key_point(const char *name, lv_point_t *p)
{
    if (!s.kb || !s.cur || !name) {
        return false;
    }
    for (uint32_t id = 0; id < s.cur->n; id++) {
        if (!key_visible(s.cur, id)) {
            continue;
        }
        kb_act_t a = (kb_act_t)s.cur->act[id];
        if ((a != KA_CHAR && a != KA_GROUP && !strcmp(name, s_act_names[a])) || !strcmp(name, label_of(s.cur, id))) {
            key_centre(id, p);
            return true;
        }
    }
    return false;
}

int ui_kb_dump(char *out, size_t len)
{
    if (!out || !len) {
        return 0;
    }
    out[0] = 0;
    if (!s.kb || !s.cur) {
        return snprintf(out, len, "no keyboard\n");
    }
    lv_buttonmatrix_t *bm = (lv_buttonmatrix_t *)s.kb;
    lv_area_t c;
    lv_obj_get_coords(s.kb, &c);
    size_t o = (size_t)snprintf(out, len, "layout %s letters %s field %d hex %d\n", s.cur->name,
                                s.letters == &KB_RU_L1 ? "ru" : "en", (int)s.field, (int)s.hex);
    for (uint32_t id = 0; id < s.cur->n && o < len; id++) {
        if (!key_visible(s.cur, id)) {
            continue;
        }
        kb_act_t a = (kb_act_t)s.cur->act[id];
        const char *lbl = (a == KA_CHAR || a == KA_GROUP || a == KA_SPACE || text_fn_key(a)) ? label_of(s.cur, id)
                                                                                          : s_act_names[a];
        const lv_area_t *r = &bm->button_areas[id];
        o += (size_t)snprintf(out + o, len - o, "%2u %-7s %-8s %d %d %d %d%s\n", (unsigned)id, s_act_names[a], lbl,
                              (int)(c.x1 + r->x1), (int)(c.y1 + r->y1), (int)(c.x1 + r->x2 + 1),
                              (int)(c.y1 + r->y2 + 1),
                              lv_buttonmatrix_has_button_ctrl(s.kb, id, LV_BUTTONMATRIX_CTRL_DISABLED) ? " D" : "");
    }
    return (int)LV_MIN(o, len - 1);
}
