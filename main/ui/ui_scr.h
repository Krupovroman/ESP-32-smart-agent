#pragma once
// Second-half screens of docs/ui.md (settings tree, keyboard, QR, dialog, steps, onboarding, ambient) and the
// small APIs they share. taskLVGL only.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui.h"
#include "ui_kb.h"
#include "ui_theme.h"

typedef void (*ui_cb_t)(void *ctx);

// ---- Builders (s_screens in ui.c); arg as documented at ui_screen_t
lv_obj_t *scr_mode_build(uint32_t arg);
lv_obj_t *scr_settings_build(uint32_t arg);
lv_obj_t *scr_wifi_build(uint32_t arg);
lv_obj_t *scr_wifi_net_build(uint32_t arg);
lv_obj_t *scr_wifi_conn_build(uint32_t arg);
lv_obj_t *scr_text_build(uint32_t arg);
lv_obj_t *scr_qr_build(uint32_t arg);
lv_obj_t *scr_provider_build(uint32_t arg);
lv_obj_t *scr_models_build(uint32_t arg);
lv_obj_t *scr_keyinfo_build(uint32_t arg);
lv_obj_t *scr_voice_build(uint32_t arg);
lv_obj_t *scr_tts_models_build(uint32_t arg);
lv_obj_t *scr_voices_build(uint32_t arg);
lv_obj_t *scr_speed_build(uint32_t arg);
lv_obj_t *scr_langs_build(uint32_t arg);
lv_obj_t *scr_lang_pick_build(uint32_t arg);
lv_obj_t *scr_dir_build(uint32_t arg);
lv_obj_t *scr_adjust_build(uint32_t arg);
lv_obj_t *scr_display_build(uint32_t arg);
lv_obj_t *scr_about_build(uint32_t arg);
lv_obj_t *scr_licenses_build(uint32_t arg);
lv_obj_t *scr_dialog_build(uint32_t arg);
lv_obj_t *scr_onboard_build(uint32_t arg);
lv_obj_t *scr_ambient_build(uint32_t arg);

// ---- Text entry K (3.8). done runs in taskLVGL: ok with the text after OK (K is already closed, so done may open
// the next screen), or !ok with the draft after the cancel button, PWR or any other way out.
typedef void (*ui_text_done_t)(const char *text, bool ok, void *ctx);
typedef struct {
    ui_field_t field;
    const char *prompt;     // "Пароль · HomeNet" (copied)
    const char *init;       // initial text (copied), NULL = empty ("sk-or-v1-" for the hex key field)
    ui_text_done_t done;
    void *ctx;
} ui_text_req_t;
void ui_text_open(const ui_text_req_t *r);

// ---- Confirm dialog C (3.11). on_confirm runs after the dialog has closed.
typedef struct {
    const char *title, *body, *confirm;     // copied
    bool danger;                            // error icon and confirm fill (else warning)
    ui_cb_t on_confirm;
    void *ctx;
} ui_dialog_t;
void ui_dialog_open(const ui_dialog_t *d);

// ---- Step template T_STEP (3.12)
typedef struct {
    lv_obj_t *scr, *orb, *title, *sub, *primary, *secondary;
} ui_step_t;
// Mini orb, title, subtitle, primary and secondary stadiums (named <name>_primary / <name>_secondary) and,
// for dot > 0, the five onboarding dots with dot 1..5 lit. Buttons call cb with ud 1 (primary) / 2 (secondary).
lv_obj_t *ui_step_screen(const char *name, ui_step_t *st, int dot, lv_event_cb_t cb);
// Texts; NULL hides that element. A title wider than 280 px in ui_28 switches to ui_20.
void ui_step_set(ui_step_t *st, const char *title, const char *sub, const char *primary, const char *secondary);

// ---- Wi-Fi connecting WC (3.12): opens the connecting step for ssid (pass NULL = use the saved password)
void ui_wifi_connect(const char *ssid, const char *pass);

// ---- Onboarding (5.7)
// Once after boot (system ready): opens O1 when no Wi-Fi network is saved or no API key is set.
void ui_onboard_check(void);
// True while the first-boot flow runs (Q / W / K continue with the next step instead of returning).
bool ui_onboarding(void);
// Continues the onboarding with the first step still missing (Wi-Fi, key, speaker, done).
void ui_onboard_resume(void);

// ---- Sleep (5.8): dim after dim_s, ambient 60 s later, screen off 5 min after that
void ui_sleep_init(void);
// 0 awake, 1 dimmed, 2 ambient, 3 off. Thread-safe.
int ui_sleep_level(void);
// Wake now (touch catcher, UI_WAKE); leaves the ambient screen.
void ui_sleep_wake(void);
// Screen off at once (UI_SCREEN_OFF).
void ui_sleep_off(void);

// ---- Small helpers shared by the screens
// "‹ Title" stays readable: the model / voice id shortened for subtitles ("google/gemini-3.1-flash-lite" ->
// "gemini-3.1-flash-lite", "ru-RU-Masha:MAI-Voice-2.1-Flash" -> "Masha").
void ui_short_model(const char *id, char *out, size_t len);
void ui_short_voice(const char *voice, char *out, size_t len);
// Host part of a URL ("https://openrouter.ai/api/v1" -> "openrouter.ai").
void ui_url_host(const char *url, char *out, size_t len);
// Translator language table (protocol 4.2): count, code and the name in the current UI language.
int ui_lang_count(void);
const char *ui_lang_code(int i);
const char *ui_lang_name(const char *code);
// Replaces codes outside the table (char[4] each) with en, or ru when the other slot is en; true when changed.
bool ui_lang_fix(char *a, char *b);
// The pair for a subtitle: "Русский — Английский", or "RU — EN" when the names do not fit 184 px.
void ui_lang_pair(char *out, size_t len);
// Applies a new translator pair (UI copy, pill and chips, settings, UI_IN_LANGS).
void ui_set_langs(const char *a, const char *b);
// The rim busy comet, reference-counted by owner (several screens may wait at once).
void ui_busy(bool on);
