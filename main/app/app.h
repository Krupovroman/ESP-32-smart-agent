#pragma once
// Application layer (architecture 1.12 / 2.3, docs/ui.md 5): the push-to-talk state machine for the
// assistant and the translator, on top of audio_in, ai_client, tts_client and the UI. One "app" task
// owns all of it; buttons, the UI, the AI workers, settings and the console only post events.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "app_evt.h"

typedef enum { APP_IDLE, APP_LISTENING, APP_UPLOADING, APP_THINKING, APP_SPEAKING, APP_ERROR, APP__N } app_state_t;

typedef struct {
    app_state_t state;
    uint8_t mode;                   // 0 assistant, 1 translator
    uint8_t tr_dir;                 // 0 auto, 1 A -> B, 2 B -> A
    bool text_only, latched, ptt_held, clip_kept;
    uint8_t kept;                   // recent turns the reader / retry cards can act on
    uint32_t kept_kb;               // PSRAM of their copied clips
    uint32_t turn;
    char last_err[8];               // error id of the last failed turn ("" = none)
    uint32_t t_rec_ms, t_upload_ms, t_ai_ms, t_tts_ttfb_ms, t_total_ms;
    // speech gate of the last recording
    const char *gate;               // "ok", "too_short", "no_speech", "silent", "" before the first
    uint32_t gate_speech_ms, gate_ms;
    int gate_floor_x10, gate_thr_x10, gate_peak_x10;
    int ambient_x10;                // idle microphone floor, VAD_NO_HINT before the first reading
    int hints;
    int fault;                      // ui_fault_t set by the app
    // device
    const char *screen;             // "on", "dim", "off"
    int bat_mv, bat_pct;            // -1 = unknown / no battery (board_battery_state)
    bool bat_charging, bat_full;
    bool locked;                    // the lock screen is up (ui_keylocked)
    bool low_mem;
} app_status_t;

// App task, input callbacks (UI, buttons, settings, AI events). Call after ui_init() and ai_client_init().
esp_err_t app_start(void);
bool app_started(void);
// Any task, never blocks longer than wait. The receiver owns e->p (freed here when the queue is full).
bool app_post(const app_evt_t *e, TickType_t wait);
// Waits until every event posted before the call has been handled; false on timeout.
bool app_wait_handled(uint32_t timeout_ms);
app_state_t app_state(void);
const char *app_state_name(app_state_t s);
// Snapshot for the console (any task).
void app_status(app_status_t *out);
// Registers the app dev-console commands: app, state, ptt, cancel, mode, textonly, btn, lock, unlock, bat.
void app_console_register(void);
