#pragma once
// Round Assistant UI (docs/ui.md, architecture 1.13). Only taskLVGL touches widgets; every other task
// talks to the UI through ui_post() and gets user intent back through the input callback.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

// ---- Messages into the UI (architecture 1.13; the second block extends it for docs/ui.md)
typedef enum {
    UI_SET_STATE,       // i = ui_state_t (app FSM mirror)
    UI_SET_MODE,        // i = 0 assistant, 1 translator (colour morph, chips, chat of that mode)
    UI_USER_MSG,        // s = transcript, or NULL for the waveform placeholder at SEND;
                        // i = translator source side (UI_SIDE_*), ignored in assistant mode
    UI_BOT_MSG,         // s = reply (assistant) or translation (translator); i = 1 when TTS failed
    UI_BOT_APPEND,      // s = chunk appended to the last reply (streaming / per TTS sentence)
    UI_TOAST,           // s = text, or s = NULL and i = ui_sid_t (ui_str.h)
    UI_ERROR,           // i = ui_card_t, s = optional body text; orb ERROR for 2.4 s, card in the chat
    UI_SET_NET,         // i = ui_net_t
    UI_PORTAL,          // i = portal state (stored for the QR screen; not shown yet)
    UI_GOTO,            // i = ui_screen_t
    UI_SET_TEXT_ONLY,   // i = 1 text only (voice button shows F6A9), 0 voice replies on
    UI_RETRY,           // i = attempt n: status "Повтор n/2…"

    UI_SET_MOOD,        // i = ui_mood_t: face expression for 1.5 s (LLM "mood")
    UI_SET_FAULT,       // i = ui_fault_t: persistent fault in the pill (UI_FAULT_NONE clears)
    UI_UPLOAD_PCT,      // i = 0..100 upload progress (orb arc + status)
    UI_USER_WAVE,       // s = 32 bytes of RMS envelope (0..255) for the pending waveform bubble
    UI_CLEAR,           // new conversation in the current mode (H back to the empty hero layout)
    UI_BACK,            // one navigation level back
    UI_PWR_RING,        // i = 1 PWR pressed (rim ring after 600 ms), 0 released (retract)
    UI_SET_LANG,        // i = 0 Russian, 1 English (screens are rebuilt)
    UI_STATUS,          // s = transient status text for 2 s (NULL with i = ui_sid_t)
    UI_SET_SPEAKER,     // i = 1 a speaker is connected, 0 none (replay disabled, voice button dot)
    UI_SET_LANGS,       // s = "ru|en": translator language A | B (ISO 639-1)
    UI_SET_HINTS,       // i = 0..3 remaining first-use hints
    UI_DIVIDER,         // s = "15:10": "— новый разговор · 15:10 —" line (model context reset after 15 min idle)
    UI_SET_FACE,        // i = 1 eyes and mouth on the orb ("Лицо шара", default), 0 plain orb
    UI_SET_TR_DIR,      // i = 0 auto (pill "RU ⇄ EN"), 1 A -> B, 2 B -> A (pill "RU → EN" / "EN → RU")

    UI_WAKE,            // leave dim / ambient / screen off (ui.md 5.8); also posted by ui_boot_down / ui_pwr_short
    UI_SCREEN_OFF,      // screen off now (PWR at H idle): backlight 0 until the next touch or button
    UI_WORK_DONE,       // internal (ui_work.c): s = finished background job, its done callback runs in taskLVGL
    UI_MSG__N
} ui_msg_type_t;

typedef struct {
    ui_msg_type_t type;
    int32_t i;
    char *s;            // heap_caps_malloc(MALLOC_CAP_SPIRAM) or NULL; the UI frees it, also when dropped
} ui_msg_t;

// UI view of the app FSM (ui.md 7.6 s_ui_state)
typedef enum {
    UI_ST_IDLE, UI_ST_REC, UI_ST_REC_LATCHED, UI_ST_REC_CANCEL, UI_ST_UPLOAD, UI_ST_THINK, UI_ST_SPEAK,
    UI_ST_ANSWERED, UI_ST_ERROR, UI_ST_OFFLINE, UI_ST__N
} ui_state_t;

typedef enum { UI_MOOD_NEUTRAL, UI_MOOD_HAPPY, UI_MOOD_SAD, UI_MOOD_SURPRISED, UI_MOOD_THINKING } ui_mood_t;

typedef enum {
    UI_FAULT_NONE, UI_FAULT_NO_WIFI, UI_FAULT_BAD_KEY, UI_FAULT_NO_CREDIT, UI_FAULT_LOW_CREDIT, UI_FAULT_REGION,
    UI_FAULT_MODEL, UI_FAULT_MODEL_EXPIRING, UI_FAULT_MIC, UI_FAULT__N
} ui_fault_t;

// Chat error cards (ui.md 3.14 / 5.6)
typedef enum {
    UI_CARD_GENERIC, UI_CARD_NO_WIFI, UI_CARD_BAD_KEY, UI_CARD_NO_CREDIT, UI_CARD_REGION, UI_CARD_MODEL,
    UI_CARD_RATE, UI_CARD_SERVER, UI_CARD_TLS, UI_CARD_MIC, UI_CARD_NO_SPEAKER, UI_CARD__N
} ui_card_t;

typedef enum { UI_NET_OFF, UI_NET_CONNECTING, UI_NET_UP, UI_NET_FAILED } ui_net_t;

typedef enum { UI_SIDE_AUTO = -1, UI_SIDE_A = 0, UI_SIDE_B = 1 } ui_side_t;

// Screens (ui.md 2.1). Each has a { name, builder, context } row in s_screens (ui.c); the builder returns a new
// screen built with ui_chrome_screen() and is deleted by the navigation stack when the user leaves it.
// UI_GOTO takes the id in i and an optional decimal screen argument in s (e.g. "1" = brightness for ADJUST).
typedef enum {
    UI_SCR_HOME, UI_SCR_HIST, UI_SCR_READER, UI_SCR_TOUCH, UI_SCR_TEST,
    UI_SCR_MODE,        // M: mode sheet (pill)
    UI_SCR_SETTINGS,    // S
    UI_SCR_WIFI,        // W: scan list, saved networks
    UI_SCR_WIFI_NET,    // one network: connect / forget (opened from W)
    UI_SCR_WIFI_CONN,   // WC: connecting (T_STEP, opened from W / K)
    UI_SCR_TEXT,        // K: text entry with the round keyboard (arg = ui_field_t for console tests)
    UI_SCR_QR,          // Q: phone setup through the SoftAP portal
    UI_SCR_PROVIDER,    // P: base URL, API key, model, key check
    UI_SCR_MODELS,      // AI: chat model picker
    UI_SCR_KEYINFO,     // GET /key: remaining credit and spend
    UI_SCR_VOICE,       // V: voice replies, TTS model, voice, speed
    UI_SCR_TTS_MODELS,  // TTS model picker
    UI_SCR_VOICES,      // voice picker of the selected TTS model
    UI_SCR_SPEED,       // speech speed
    UI_SCR_LANGS,       // L: translator languages
    UI_SCR_LANG_PICK,   // LP (arg 0 = language A, 1 = language B)
    UI_SCR_DIR,         // translator direction
    UI_SCR_ADJUST,      // A (arg 0 = volume, 1 = brightness)
    UI_SCR_DISPLAY,     // screen and sleep timers
    UI_SCR_ABOUT,       // D: about / diagnostics
    UI_SCR_LICENSES,
    UI_SCR_DIALOG,      // C: confirm dialog
    UI_SCR_ONBOARD,     // O1-O5 first boot (arg = step 1..5)
    UI_SCR_AMBIENT,     // Z: clock and dot orb
    UI_SCR__N
} ui_screen_t;

// ---- User intent out of the UI. The callback runs in taskLVGL and must only post (never block).
typedef enum {
    UI_IN_PTT_DOWN,     // orb or chip pressed; i = forced source side (UI_SIDE_AUTO for the orb)
    UI_IN_PTT_UP,       // i = held ms (the app decides hold vs latch)
    UI_IN_PTT_CANCEL,   // released beyond the cancel radius (discard)
    UI_IN_TEXT_ONLY,    // voice button: i = new text_only value
    UI_IN_NEW_CHAT,     // "Новый разговор" (history and model context reset)
    UI_IN_CARD_ACTION,  // i = ui_card_t whose action stadium was tapped
    UI_IN_WRONG_LANG,   // reader: re-send the cached clip, i = forced source side
    UI_IN_REPLAY,       // reader: speak the translation again
    UI_IN_OPEN,         // no longer emitted: the UI opens these screens itself (ui_open_t kept for ui_open())
    UI_IN_DISMISS,      // error orb tapped

    UI_IN_SET_MODE,     // mode sheet: i = 0 assistant, 1 translator (the UI already switched and saves "mode")
    UI_IN_TEXT,         // typed question (M -> "Написать текстом"): i = byte length, text from ui_typed_text()
    UI_IN_TR_DIR,       // translator direction: i = 0 auto, 1 A -> B, 2 B -> A (RAM only)
    UI_IN_LANGS,        // translator languages changed (tr_lang_a / tr_lang_b saved)
    UI_IN_SPEAKER,      // Settings -> "Динамик": i = 1 connected, 0 none (RAM only)
    UI_IN_SLEEP,        // i = 0 awake, 1 dimmed, 2 ambient, 3 screen off (the app may stop the mic at 3)
} ui_input_t;

// Screens the UI opens on its own (pill, settings button, chips, offline orb, fault pill, card actions)
typedef enum {
    UI_OPEN_MODE_SHEET,     // pill (M)
    UI_OPEN_SETTINGS,       // settings button (S)
    UI_OPEN_WIFI,           // offline orb (W)
    UI_OPEN_LANG_A,         // chip A tapped (LP for slot A)
    UI_OPEN_LANG_B,         // chip B tapped (LP for slot B)
    UI_OPEN_FAULT_FIX,      // fault pill: the screen that fixes g_s_fault (W | Q | P | AI)
} ui_open_t;

typedef void (*ui_input_cb_t)(ui_input_t ev, int32_t i, void *ctx);

// Where the user is, for routing the physical buttons (ui.md 2.3)
typedef enum {
    UI_CTX_HOME, UI_CTX_READ, UI_CTX_LIST, UI_CTX_QR, UI_CTX_ADJUST, UI_CTX_TEXT, UI_CTX_DIALOG, UI_CTX_STEP,
    UI_CTX_TOUCH, UI_CTX_TEST, UI_CTX_AMBIENT
} ui_ctx_t;

// Theme, fonts, subjects, rim, Home screen, ui_pump (20 ms) timer. Call once after display_init().
esp_err_t ui_init(lv_display_t *disp);
// Any task, non-blocking (queue of 32). Ownership of m->s passes to the UI even if the queue is full.
void ui_post(const ui_msg_t *m);
// ui_post() with a PSRAM copy of text (text may be NULL).
void ui_post_text(ui_msg_type_t type, int32_t i, const char *text);
// Registers the app's input callback (one; NULL removes it).
void ui_set_input_cb(ui_input_cb_t cb, void *ctx);

// LVGL lock for rare direct calls from other tasks; never hold it while waiting on I/O or injection.
bool ui_lock(uint32_t ms);
void ui_unlock(void);

// Renders obj_name (NULL = active screen) into dst (RGB565) inside taskLVGL. Call without the lock.
esp_err_t ui_snapshot(const char *obj_name, lv_draw_buf_t *dst);
// Named object on the active screen, then on the top layer; NULL if absent. Call under the lock.
lv_obj_t *ui_find(const char *name);
// Named objects of the active screen + top layer as a JSON array; returns the length. Call under the lock.
int ui_tree_json(char *out, size_t len);
// Geometry audit (no clip_corner, Rmax <= 168, clickable >= 50x50) as JSON; returns the violation count.
int ui_audit_json(char *out, size_t len);

// ---- Physical buttons (ui.md 2.3). Thread-safe: they read an atomic context and post to the UI queue.
ui_ctx_t ui_context(void);
// BOOT pressed. false: ignored here (K, C, O1-O4; a toast explains). true: talk; from T_READ / T_LIST / T_QR /
// T_ADJUST the UI jumps to H first.
bool ui_boot_down(void);
// PWR short press. true: the UI went back one level. false: already at H (cancel a turn or screen off).
bool ui_pwr_short(void);
// PWR pressed / released: the rim POWER ring appears at 600 ms and fills until 3000 ms; release retracts it.
void ui_pwr_down(void);
void ui_pwr_up(void);

// ---- Sleep (ui.md 5.8). Thread-safe.
// Screen off now (PWR at H idle); the next touch, BOOT or PWR wakes it.
void ui_screen_off(void);
// True while dimmed, ambient or off (a BOOT / PWR press is then a wake first).
bool ui_asleep(void);

// Copies the last typed question (UI_IN_TEXT) into out; returns its length. Thread-safe.
size_t ui_typed_text(char *out, size_t len);

// Registers the "ui" dev-console command (call before devconsole_start()).
void ui_console_register(void);
