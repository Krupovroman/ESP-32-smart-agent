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
    S_ST_CANCELLED, S_ST_RETRY, S_ST_BUSY_HINT, S_ST_WEB,
    // first-use hints (3.2)
    S_HINT1, S_HINT2, S_HINT1_TR, S_HINT2_TR,
    // titles and chat
    S_TITLE_HISTORY, S_NEW_CHAT, S_DIVIDER, S_TTS_NOTE, S_WEB_SOURCE_FMT,
    // toasts
    S_VOICE_OFF, S_VOICE_ON, S_FINISH_INPUT, S_USB_POWER, S_DONE, S_NOT_YET,
    // touch test (3.15)
    S_TOUCH_FMT, S_TOUCH_HELP,
    // persistent faults in the pill (3.14, 5.6)
    S_F_NO_WIFI, S_F_BAD_KEY, S_F_NO_CREDIT, S_F_LOW_CREDIT, S_F_REGION, S_F_MODEL, S_F_MODEL_EXP, S_F_MIC,
    S_F_NO_KEY,
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
    S_C_NOKEY_T, S_C_NOKEY_A,
    S_C_ERROR_T,
    // short errors for the status line
    S_E_GENERIC, S_E_SERVER, S_E_RATE, S_E_TLS, S_E_MIC, S_E_SPK,

    // ---- second half (lists, keyboard, QR, dialog, steps, ambient)
    S_ON, S_OFF, S_NOT_SAVED, S_CANCEL, S_LOADING, S_ERR_LOAD, S_AUTO, S_NEVER, S_SEC_FMT, S_MIN_FMT,
    // arc titles
    S_T_MODE, S_T_SETTINGS, S_T_WIFI, S_T_PROVIDER, S_T_MODELS, S_T_KEY, S_T_VOICE, S_T_TTS, S_T_VOICES,
    S_T_SPEED, S_T_LANGS, S_T_LANG_A, S_T_LANG_B, S_T_DIR, S_T_DISPLAY, S_T_ABOUT, S_T_LICENSES, S_T_PHONE,
    // mode sheet
    S_M_ASSISTANT_SUB, S_M_TYPE, S_M_NEW_SUB, S_M_FIX, S_VOLUME, S_BRIGHT, S_VOICE_REPLY, S_VOICE_TR,
    // settings
    S_S_NOT_CONNECTED, S_S_CONNECTING, S_S_PHONE_SUB, S_S_SPEAKER, S_S_SPK_YES, S_S_SPK_NO, S_S_DIM_FMT,
    S_S_DIM_NEVER, S_S_FACE, S_S_UILANG, S_LANG_SELF, S_S_CLEAR,
    // Wi-Fi
    S_W_RESCAN, S_W_SCANNING, S_W_CONNECTED, S_W_SECURED, S_W_OPEN, S_W_SAVED, S_W_AWAY, S_W_OTHER, S_W_PHONE,
    S_W_NONE, S_N_CONNECT, S_N_PASSWORD, S_N_FORGET, S_N_STATUS, S_N_IP, S_N_SIGNAL, S_N_FORGET_Q, S_N_FORGET_BTN,
    S_N_FORGOTTEN, S_K_PASS_FMT, S_K_SSID,
    S_WC_TITLE, S_WC_SUB_FMT, S_WC_OK, S_WC_BAD_PASS, S_WC_NOT_FOUND, S_WC_FAILED, S_WC_AGAIN, S_WC_OTHER,
    // provider, models, key, voice
    S_P_URL, S_P_KEY_NONE, S_P_CHECK, S_P_PHONE, S_K_URL, S_K_KEY_FMT, S_K_KEY, S_K_MODEL, S_P_NEED_HTTPS,
    S_P_WEB, S_WEB_ALWAYS,
    S_AI_LOAD, S_AI_CUSTOM, S_AI_PRICE_FMT, S_AI_DEFAULT, S_AI_EXPIRES_FMT,
    S_KI_REMAINING, S_KI_OF_FMT, S_KI_NO_LIMIT, S_KI_RESET, S_KI_TODAY, S_KI_MONTH, S_KI_TOTAL, S_KI_FREE,
    S_KI_DAILY, S_KI_WEEKLY, S_KI_MONTHLY,
    S_V_MODEL, S_V_SPEED, S_V_NO_VOICES, S_V_PRICE_FMT,
    // about
    S_A_FW, S_A_ROLLBACK, S_A_PANEL, S_A_UPTIME, S_A_MEM, S_A_LATENCY, S_A_LAT_FMT, S_A_LAST_ERR, S_A_NONE,
    S_A_MIC, S_A_MIC_FMT, S_A_SPK_TEST, S_A_SPK_SUB,
    S_A_TOUCH, S_A_HINTS, S_A_HINTS_SUB, S_A_HINTS_ON, S_A_REBOOT, S_A_REBOOT_Q, S_A_YES, S_A_RESET, S_A_RESET_Q,
    S_A_RESET_B, S_A_RESET_BTN, S_H_CLEAR_Q, S_H_CLEAR_B, S_H_DELETE, S_H_CLEARED,
    // phone setup
    S_Q_NET_FMT, S_Q_PASS_FMT, S_Q_OPEN, S_Q_WAIT, S_Q_STARTING, S_Q_WIFI_OK, S_Q_KEY_OK, S_Q_MODEL_OK, S_Q_ERROR,
    // onboarding
    S_O1_T, S_O1_S, S_O1_P, S_O1_B, S_O2_T, S_O2_S, S_O2_B, S_O3_T, S_O3_S, S_O3_B, S_O4_T, S_O4_S, S_O4_P,
    S_O4_B, S_O5_T, S_O5_S, S_O5_P,
    // text entry
    S_K_QUESTION, S_K_INPUT, S_NEW_CHAT_Q,
    // battery and lock screen (3.16), button guide on O5
    S_HB_FMT, S_BAT_LOW, S_LK_HINT, S_LK_PCT, S_LK_FULL, S_LK_DRAIN, S_O5_G1, S_O5_G2,
    S__N
} ui_sid_t;

// Measured string slots for the fit test (docs/ui.md 7.9).
typedef enum {
    SLOT_NONE, SLOT_PILL, SLOT_FAULT, SLOT_STATUS, SLOT_TITLE, SLOT_HINT, SLOT_HINT2, SLOT_TOAST, SLOT_TOAST_WIDE,
    SLOT_BTN, SLOT_BTN_WIDE,
    SLOT_ROW,           // list row title (ui_20, 164)
    SLOT_ROW_SUB,       // list row subtitle / info caption (ui_16, 184)
    SLOT_TOGGLE,        // "Вкл" / "Выкл" in the right slot (ui_16, 44)
    SLOT_PROMPT,        // K prompt label (ui_16, 150)
    SLOT_STEP_T,        // T_STEP title (ui_28, 280)
    SLOT_STEP_S,        // T_STEP subtitle (ui_16, 290)
    SLOT_STEP_P,        // T_STEP primary stadium (ui_20, 204)
    SLOT_STEP_B,        // T_STEP secondary stadium (ui_20, 168)
    SLOT_DLG_T,         // dialog title (ui_20, 260)
    SLOT_DLG_BTN,       // dialog stadium (ui_20, 104)
    SLOT_QR1,           // Q caption 1 (ui_16, 220)
    SLOT_QR2,           // Q caption 2 (ui_16, 150)
    SLOT_QR_TITLE,      // Q arc title (ui_20, 100)
    SLOT_QR_LINE,       // Q "received" lines (ui_20, 220)
    SLOT_HOME_BAT,      // Home battery glyph + percent above the pill (ui_16, 70)
    SLOT_LOCK_LINE,     // lock screen date / hint line, USB note (ui_16, 220)
    SLOT_LOCK_BAT,      // lock screen battery line after the bolt / plug / check (ui_20, 134)
    SLOT_GUIDE1,        // O5 button guide, line 1 (ui_16, 240)
    SLOT_GUIDE2,        // O5 button guide, line 2 (ui_16, 170)
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
