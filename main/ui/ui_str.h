#pragma once
// UI strings in Russian (default) and English, plus the model-text sanitiser (docs/ui.md 1.3).
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

typedef enum {
    // pill / modes
    S_MODE_ASSISTANT, S_MODE_TRANSLATOR,
    // status line (5.1, 5.3)
    S_ST_LISTENING, S_ST_LISTENING_LANG, S_ST_LATCHED, S_ST_CANCEL, S_ST_LEFT_S, S_ST_UPLOAD,
    S_ST_THINK, S_ST_THINK2, S_ST_THINK3, S_ST_SPEAK_HINT, S_ST_NOT_HEARD, S_ST_TOO_SHORT,
    S_ST_CANCELLED, S_ST_RETRY, S_ST_BUSY_HINT,
    // first-use hints (3.2)
    S_HINT1, S_HINT2, S_HINT1_TR, S_HINT2_TR,
    // titles and chat
    S_TITLE_HISTORY, S_NEW_CHAT, S_DIVIDER, S_TTS_NOTE,
    // toasts
    S_VOICE_OFF, S_VOICE_ON, S_FINISH_INPUT, S_USB_POWER, S_DONE, S_NOT_YET,
    // touch test (3.15)
    S_TOUCH_FMT, S_TOUCH_HELP,
    // persistent faults in the pill (3.14, 5.6)
    S_F_NO_WIFI, S_F_BAD_KEY, S_F_NO_CREDIT, S_F_LOW_CREDIT, S_F_REGION, S_F_MODEL, S_F_MODEL_EXP, S_F_MIC,
    // error cards: title, body, action (5.6)
    S_C_WIFI_T, S_C_WIFI_B, S_C_WIFI_A,
    S_C_KEY_T, S_C_KEY_A,
    S_C_CREDIT_T, S_C_CREDIT_B, S_C_CREDIT_A,
    S_C_REGION_T, S_C_REGION_B, S_C_REGION_A,
    S_C_MODEL_T, S_C_MODEL_A,
    S_C_RATE_T,
    S_C_SERVER_T, S_C_RETRY_A,
    S_C_TLS_T, S_C_DIAG_A,
    S_C_MIC_T,
    S_C_SPK_T, S_C_SPK_B, S_C_SPK_A,
    S_C_ERROR_T,
    // short errors for the status line
    S_E_GENERIC, S_E_SERVER, S_E_RATE, S_E_TLS, S_E_MIC, S_E_SPK,
    S__N
} ui_sid_t;

// Measured string slots for the fit test (docs/ui.md 7.9).
typedef enum {
    SLOT_NONE, SLOT_PILL, SLOT_FAULT, SLOT_STATUS, SLOT_TITLE, SLOT_HINT, SLOT_HINT2, SLOT_TOAST, SLOT_TOAST_WIDE,
    SLOT_BTN, SLOT_BTN_WIDE
} ui_slot_t;

// Text of id in the current UI language.
const char *ui_s(ui_sid_t id);
// 0 = Russian (default), 1 = English.
void ui_str_set_lang(int lang);
int ui_str_lang(void);

// Prints every string that overflows its slot (both languages) through out(); returns the overflow count.
int ui_str_fit_check(void (*out)(const char *line));

// Model text -> label text: strips markdown (# * _ ` >), "- " list marks to bullets, emoji, U+2028/2029 to
// newlines, collapses 3+ newlines and drops code points font cannot draw. out is always NUL-terminated.
size_t ui_str_sanitize(const char *in, char *out, size_t out_len, const lv_font_t *font);

// True when every code point of s exists in font (used to pick ui_28 for short translations).
bool ui_str_font_has_all(const char *s, const lv_font_t *font);
// Number of UTF-8 code points in s.
size_t ui_str_cp_count(const char *s);
// Copies src into dst (always NUL-terminated), cutting only at a code point boundary; returns the length.
size_t ui_str_copy(char *dst, size_t len, const char *src);
