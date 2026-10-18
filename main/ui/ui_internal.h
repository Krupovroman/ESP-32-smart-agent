#pragma once
// Shared between the UI modules only (taskLVGL context unless noted). Other components use ui.h.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui.h"
#include "ui_subj.h"

// ---- Subjects (ui.md 7.6); written only by ui_pump / UI handlers
extern ui_subj_t g_s_state;     // ui_state_t
extern ui_subj_t g_s_mode;      // 0 assistant, 1 translator
extern ui_subj_t g_s_text_only; // 1 = replies in text only
extern ui_subj_t g_s_speaker;   // 1 = speaker connected
extern ui_subj_t g_s_fault;     // ui_fault_t
extern ui_subj_t g_s_net;       // ui_net_t
extern ui_subj_t g_s_upload;    // 0..100
extern ui_subj_t g_s_hints;     // 0..3 first-use hints left
extern ui_subj_t g_s_portal;    // portal state for the QR screen
extern ui_subj_t g_s_tr_dir;    // translator direction: 0 auto, 1 A -> B, 2 B -> A
extern ui_subj_t g_s_face;      // 1 = face on the orb
extern ui_subj_t g_s_battery;   // UI_BAT_MAKE() value of the last UI_SET_BATTERY (UI_BAT_NONE until the first)

// Translator language pair (ISO 639-1, lower case)
extern char g_lang_a[4], g_lang_b[4];

// ---- Navigation (ui.md 7.8): H is permanent, other screens are built on entry and deleted on exit
#define UI_NAV_MAX 8        // deepest path: H -> S -> W -> network -> K -> WC
void ui_nav_push(ui_screen_t id, uint32_t arg);   // arg: screen-specific (chat entry id for HIST / RD)
// One level back (PWR, title zone, swipe). A back handler of the current screen may take it over.
void ui_nav_back(void);
void ui_nav_home(void);
// Replaces the current screen (onboarding steps, WC retries) without growing the stack.
void ui_nav_replace(ui_screen_t id, uint32_t arg);
// Back handler of the current screen (cleared when it is left); return true when the screen handled back.
void ui_nav_set_back(bool (*fn)(void));
ui_screen_t ui_nav_current(void);
// True when screen id is anywhere on the navigation stack.
bool ui_nav_has(ui_screen_t id);
const char *ui_screen_name(ui_screen_t id);
// Opens the screen for a UI_IN_OPEN reason (pill, settings button, chips, offline orb, fault pill).
void ui_open(ui_open_t what);
// Card action stadium: opens the fixing screen; false when the action belongs to the app (retry).
bool ui_open_card(int32_t card);
// Persistent fault the pill shows (3.14): g_s_fault, or UI_FAULT_NO_WIFI while offline; its pill / M-row text;
// warning (yellow) rather than error (coral).
int32_t ui_fault_shown(void);
const char *ui_fault_text(int32_t f);
bool ui_fault_warning(int32_t f);
// Switches the UI language now: H is rebuilt (its chat survives) and the stack is dropped.
void ui_apply_lang(int lang);
// Stores the typed question for ui_typed_text() and emits UI_IN_TEXT; false without an app.
bool ui_send_typed(const char *text);

// Sends user intent to the app (input callback); returns false when no app is registered.
bool ui_emit(ui_input_t ev, int32_t i);

// ---- Queue bookkeeping (any task)
// Waits until every message posted before the call has been dispatched; false on timeout.
bool ui_wait_pumped(uint32_t timeout_ms);
// Messages dropped because the queue was full or out of memory.
unsigned ui_dropped(void);
// ui_post() that waits up to ms for room instead of dropping (background tasks only); false = dropped, and
// then m->s is NOT freed (the caller still owns it).
bool ui_post_wait(const ui_msg_t *m, uint32_t ms);

// ---- Inspection helpers for the console (call under ui_lock)
#define UI_NAME_MAX 48
typedef void (*walk_fn_t)(lv_obj_t *obj, void *ctx);
// Calls fn for every object with a name on the active screen, then on the top layer.
void ui_walk_named(walk_fn_t fn, void *ctx);
// LVGL class name ("lv_label", "lv_obj", ...).
const char *ui_obj_type(const lv_obj_t *obj);
// Not hidden and not fully transparent, including every ancestor.
bool ui_obj_visible(lv_obj_t *obj);
// Coords clipped by all ancestors; false when nothing is visible.
bool ui_obj_visible_area(lv_obj_t *obj, lv_area_t *out);
// Rmax of the rounded rectangle obj draws inside area a (ui.md 0.4).
float ui_obj_rmax(lv_obj_t *obj, const lv_area_t *a);
// Toast in the current screen's status slot (Home) or a bottom stadium elsewhere.
void ui_toast(const char *text);

// ---- Shared chrome (ui.md 3.1)
// Arc title "‹ text" along the top rim plus its transparent back zone; returns the arclabel.
lv_obj_t *ui_chrome_title(lv_obj_t *scr, const char *text);
// Swipe right = back on scrolling templates (T_READ / T_LIST); the swipe is never a click.
void ui_chrome_swipe_back(lv_obj_t *scr);
// A plain screen with the shared screen style (black, ui_20 text-1), not scrollable.
lv_obj_t *ui_chrome_screen(void);
// Bottom toast on the top layer (non-Home screens), 2 s.
void ui_chrome_toast(const char *text);
// Top + bottom fades over a viewport (x, y, w, h in screen coords; heights of the two bands).
void ui_chrome_fades(lv_obj_t *scr, int32_t x, int32_t y, int32_t w, int32_t h, int32_t top_h, int32_t bot_h);

// ---- Screens
lv_obj_t *scr_home_create(void);            // permanent H / H'
void scr_home_resume(void);                 // H is the active screen again
void scr_home_user(const char *text, int32_t side);
void scr_home_bot(const char *text, bool tts_failed);
void scr_home_bot_append(const char *chunk);
void scr_home_wave(const uint8_t *env32);
void scr_home_error(int32_t card, const char *body);
void scr_home_clear(bool anim);
void scr_home_divider(const char *hhmm);
void scr_home_toast(const char *text);
void scr_home_status_text(const char *text, uint32_t ms);
void scr_home_retry(int32_t n);
void scr_home_web(bool on);                 // "Ищу в интернете…" replaces the thinking stages while on
void scr_home_bot_source(const char *host); // "Источник: host" under the last assistant reply
void scr_home_mood(int32_t mood);
void scr_home_langs_changed(void);
lv_obj_t *scr_home_orb(void);

// ---- Lock screen (scr_lock.c, ui.md 3.16): one overlay on the top layer above every screen, toast below it
void ui_lockscr_request(int32_t req);       // UI_LOCK (ui_lock_req_t)
bool ui_lockscr_active(void);               // the overlay is up (locking, locked or unlocking)
lv_obj_t *ui_lockscr_obj(void);             // the overlay or NULL (the audit exempts its backdrop)
void ui_lockscr_woke(int from_level);       // ui_sleep: the backlight is coming back from that sleep level
void ui_lockscr_lang(void);                 // the texts again after a language switch
// Console screenshots: shows frame ms of the lock (unlock = false) or unlock animation and holds it; ms < 0
// finishes that animation. Returns false when the request does not fit the current state.
bool ui_lockscr_freeze(bool unlock, int32_t ms);
// Battery glyph for a level (F240..F244)
const char *ui_bat_glyph(int pct);

lv_obj_t *scr_hist_build(uint32_t entry_id);
lv_obj_t *scr_reader_build(uint32_t entry_id);
lv_obj_t *scr_touch_build(uint32_t arg);
lv_obj_t *ui_test_build(uint32_t arg);
