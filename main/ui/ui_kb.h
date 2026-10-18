#pragma once
// The round keyboard (docs/ui.md 4): a two-step "group -> letter" keypad on the Russian phone-keypad groups, the
// EN layout, digits and symbols pages and the single-tap hex keypad for OpenRouter keys, all on one stock
// lv_keyboard (LV_KEYBOARD_MODE_USER_1) whose map is swapped in place. Keys are classified by button index through
// action tables, never by their UTF-8 label. taskLVGL only; one keyboard at a time (the maps are LVGL statics).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

// Field kinds of ui.md 4.6
typedef enum {
    UI_FIELD_QUESTION,      // any text <= 500 bytes; starts in the UI language
    UI_FIELD_WIFI_PASS,     // printable ASCII, 8..63 characters, secret (reveal button)
    UI_FIELD_SSID,          // <= 32 UTF-8 bytes
    UI_FIELD_KEY_HEX,       // "sk-or-v1-" (protected) + 64 hex digits on the hex keypad; "Aa" = full EN keyboard
    UI_FIELD_KEY,           // other API key: printable ASCII <= 200, secret
    UI_FIELD_URL,           // base URL: printable ASCII <= 128, must start with https://
    UI_FIELD_MODEL,         // model id: printable ASCII <= 96
    UI_FIELD_WIFI_PASS_OPT, // as UI_FIELD_WIFI_PASS, or empty: a hidden network ("Другая сеть…") may be open
    UI_FIELD__N
} ui_field_t;

#define UI_KEY_PREFIX   "sk-or-v1-"
#define UI_KEY_HEX_LEN  64

// Creates the keyboard at x 18-342, y 115-324 on scr, typing into ta (one-line textarea). The textarea gets
// LV_EVENT_READY when OK is tapped while the field is valid.
lv_obj_t *ui_kb_create(lv_obj_t *scr, lv_obj_t *ta, ui_field_t field);
// UI_FIELD_KEY_HEX: true = hex keypad, false = full EN keyboard (the "Aa" button).
void ui_kb_set_hex(bool hex);
bool ui_kb_hex(void);
// Re-evaluates OK after the text changed from outside the keyboard.
void ui_kb_refresh(void);

// Field rules (hex = the key field is on the hex keypad)
bool ui_field_secret(ui_field_t f);
bool ui_field_ok(ui_field_t f, bool hex, const char *text);
// "Ключ · 37/64": hex digits typed after the prefix, -1 when text does not start with it.
int ui_field_hex_count(const char *text);

// ---- Console helpers (call under ui_lock)
typedef enum {
    UI_KB_NONE,     // cp cannot be typed on this field
    UI_KB_STEP,     // tap *p (group, page or language key), then plan again
    UI_KB_TYPES,    // tap *p and cp is inserted
    UI_KB_AA,       // the hex keypad lacks cp: tap the "Aa" button first
} ui_kb_plan_t;
// Next tap (absolute key centre in *p) on the way to typing code point cp from the current layout.
ui_kb_plan_t ui_kb_plan(uint32_t cp, lv_point_t *p);
// Centre of a key of the current layout: "ok", "bksp", "space", "lang", "digits", "symbols", "letters", "back"
// or a key label ("абвг", "123", "a", ...). false when the layout has no such visible key.
bool ui_kb_key_point(const char *name, lv_point_t *p);
// Current layout and its visible keys (id, action, label, x1 y1 x2 y2, D = disabled), one per line.
int ui_kb_dump(char *out, size_t len);
