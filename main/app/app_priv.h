#pragma once
// Shared by the app modules (app.c state machine, app_io.c device side, dev_cmds.c console).
// Everything here runs in the app task unless the comment says otherwise.
#include <stdbool.h>
#include <stdint.h>
#include "app.h"
#include "ai_client.h"
#include "board.h"
#include "vad.h"

#define APP_TAP_MS          400     // ui.md 2.3: tap / hold threshold
#define APP_LATCH_IDLE_MS   5000    // latched recording without speech: "Не расслышал"
#define APP_VAD_SPEECH_MS   400     // auto-send after this much speech ...
#define APP_VAD_SILENCE_MS  1200    // ... followed by this much silence (ui.md 5.1)
#define APP_ERROR_MS        4000    // ERROR -> IDLE (architecture 2.3)
#define APP_TURN_MAX_MS     75000   // no result at all (ai: 45 s per attempt + 20 s of retries)
#define APP_TTS_FIRST_MS    15000   // voice on, no first audio: text only, "голос недоступен"
#define APP_REVEAL_MS       2500    // voice on: the reply appears with its audio, at the latest after this
#define APP_SPEAK_MAX_MS    180000  // playback watchdog
#define APP_SAVE_DELAY_MS   2000    // UI-originated settings are written 2 s later, in IDLE
#define APP_SPK_TAIL_MS     150     // speaker DMA (80 ms) + flush + room echo: the mic still hears the
                                    // device's own speech this long after it was stopped

typedef enum { APP_SCREEN_ON, APP_SCREEN_DIM, APP_SCREEN_OFF } app_screen_t;

// AI result copied out of the ai task (strings in PSRAM)
typedef struct {
    char *transcript, *text;
    char lang[8], src_lang[8], tts_lang[8];
    bool unheard, truncated, other_lang, stt_used;
    ai_timings_t t;
    double cost;
    char model[64];
} app_result_t;

void app_evt_free(app_evt_t *e);
uint32_t app_now_ms(void);
// Setting with a default for keys that are not in the table (yet).
int32_t app_cfg_int(const char *key, int32_t def);
// JSON string body of in (quotes, backslashes and control characters escaped); returns the length.
size_t app_json_esc(char *out, size_t len, const char *in);
// Speech gate thresholds (any task): copies the current ones to get, then replaces them with set.
void app_vad_cfg(vad_cfg_t *get, const vad_cfg_t *set);

// ---- app.c, called by app_io.c (app task)
void app_ptt_down(int8_t side, app_src_t src);
void app_ptt_up(int32_t held_ms, app_src_t src);
// PWR short press (ui.md 2.3): back off other screens; on H cancel / stop speech / dismiss, or screen off.
void app_pwr_short(void);
// Ends whatever runs (recording, turn, speech) without a message; the state becomes IDLE.
void app_abort_all(void);
void app_net_changed(bool up);
void app_portal_changed(bool on);
// The app is IDLE (or ERROR) with no press held: settings may be written, the screen may dim.
bool app_quiet(void);

// ---- app_io.c (app task unless noted)
void app_io_init(void);
void app_io_tick(uint32_t now);
void app_io_button(board_btn_t b, board_btn_ev_t ev, uint32_t held_ms);
// Applies a changed setting that belongs to the device side (volume, brightness, languages, mic).
void app_io_setting(const char *key);
// Persists an int setting: now (console) or APP_SAVE_DELAY_MS later once the app is quiet (UI).
void app_io_save(const char *key, int32_t v, bool now);
// Input activity (resets the dim timer). Returns true when the screen was dimmed or off and woke up.
bool app_io_activity(void);
void app_io_screen_off(void);
// PWR held 3 s: power off on battery; on USB the screen goes off with "Питание от USB".
void app_io_power_off(void);
bool app_io_net_up(void);
bool app_io_portal(void);
bool app_io_low_mem(void);
// Idle microphone noise floor (dBFS x10) or VAD_NO_HINT.
int app_io_ambient(void);
// Any task
app_screen_t app_io_screen(void);
int app_io_bat_mv(void);
