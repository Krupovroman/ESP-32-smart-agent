#pragma once
// Recording mocks of everything the app layer calls (settings, ai/tts clients, audio, UI, board, net).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ai_client.h"

extern int64_t g_now_us;

// ---- UI
typedef struct {
    int type;
    int32_t i;
    bool has_s;
    char s[240];
} ui_rec_t;
extern ui_rec_t g_ui[1024];
extern int g_nui;
extern int g_ui_ctx;            // ui_context()
extern bool g_boot_ok;          // ui_boot_down() (false as well while g_locked)
extern int g_pwr_short, g_pwr_down, g_pwr_up;
// Lock screen as the UI keeps it: ui_pwr_short() / ui_keylock() flip it and report UI_IN_LOCK like the real UI
extern bool g_locked;
extern int g_boot_refused;      // BOOT presses ui_boot_down() refused because of the lock
extern uint32_t g_inactive_ms;  // lv_display_get_inactive_time()
extern bool g_touch_down;       // the pointer device reads pressed
extern int g_wait_release;
extern int g_ui_level;          // ui_sleep.c level: UI_WAKE / UI_GOTO -> 0, UI_SCREEN_OFF -> 3, reported as UI_IN_SLEEP
// The UI's own idle timer (or its wake catcher) changes the sleep level and reports it
void ui_sleep_to(int level);
extern int g_ui_lang;           // ui_str_lang(): 0 Russian, 1 English
int ui_count(int type);
const ui_rec_t *ui_last(int type);
// index of the first record of type at or after from, -1 if none
int ui_index(int type, int from);

// ---- AI / TTS
typedef struct {
    uint32_t turn;
    int mode;
    const int16_t *pcm;
    size_t samples;
    bool has_text;
    char text[240];
    char src[8];
} submit_rec_t;
extern submit_rec_t g_sub[64];
extern int g_nsub;
extern uint32_t g_cancel_upto, g_tts_cancel_upto;
extern int g_prewarm, g_hist_clear;
extern esp_err_t g_submit_rc;
typedef struct {
    uint32_t turn;
    char text[240];
    char lang[8];
} speak_rec_t;
extern speak_rec_t g_speak[32];
extern int g_nspeak;
// Events through the app's real listener (as the ai / tts tasks would).
void emit_simple(ai_ev_type_t t, uint32_t turn, int32_t i);
void emit_result(uint32_t turn, ai_mode_t mode, const char *transcript, const char *text, const char *lang,
                 const char *src_lang, const char *tts_lang, bool unheard);
// Assistant result made with web search (source "" = none), or the first answer after a failed search.
void emit_result_web(uint32_t turn, const char *transcript, const char *text, const char *source, bool failed);
void emit_error(ai_ev_type_t t, uint32_t turn, const char *id, const char *msg);

// ---- audio
#define REC_MAX (16000 * 30)
extern int16_t g_rec[REC_MAX];
extern size_t g_rec_len;
extern bool g_recording;
extern uint32_t g_preroll, g_max_ms;
extern int g_rec_starts, g_rec_stops;
extern int g_src;               // 0 mic, 1 inject
extern int g_level;
extern bool g_out_busy;
extern int g_out_stops;
extern int g_volume_set;

// ---- settings
void set_int(const char *k, int v);
void set_str(const char *k, const char *v);
extern int g_settings_writes;

// ---- console events
extern int g_nev;
int ev_count(const char *name);
// last "ev" line (name + members) or ""
const char *ev_last(const char *name);

// ---- board, net
extern int g_backlight;
extern int g_power_off, g_latch_on;
extern bool g_boot_down, g_pwr_held;
extern int g_bat_mv;            // board_battery_state(): mv, pct from the mock curve
extern bool g_bat_chg;          // ... charging
extern bool g_bat_drain;        // ... on USB but draining (wins over g_bat_chg)
extern int g_bat_samples, g_bat_settled;    // board_battery_sample() calls, and those with BOARD_BAT_SETTLED
extern bool g_bat_last_settled;
extern int g_bat_last_q;        // board_bat_q_t of the last call, -1 = none
extern int g_bat_poll_ms;       // board_battery_state(): poll_ms (0 = the app's default)
extern int g_bat_vfull_learn;   // ... vfull_learn (-1 none)
extern int g_bat_vfull_set;     // last board_battery_set_vfull() (-1 none)
extern int g_net_state;
extern bool g_portal;

void mocks_reset(void);
