#pragma once
// Events of the app task (architecture 2.2): every input, worker result and setting change reaches the
// state machine through one queue, in order. p points to PSRAM owned by the receiver, which frees it
// even when it drops the event.
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EV_PTT_DOWN,        // i = forced translator source (UI_SIDE_*), j = app_src_t
    EV_PTT_UP,          // i = held ms (< 0: measured by the app), j = app_src_t
    EV_PTT_CANCEL,      // orb released beyond the cancel radius: discard
    EV_CANCEL,          // console "cancel" (PWR goes through EV_BUTTON)
    EV_TEXT_SUBMIT,     // p = typed text
    EV_AI_UPLOAD_DONE,  // i = upload ms
    EV_AI_RETRY,        // i = retry number
    EV_AI_WEB,          // a request with web search starts; i = 1 auto-mode re-run, 0 first request
    EV_AI_RESULT,       // p = app_result_t
    EV_AI_ERROR,        // p = or_err_t
    EV_AI_RELEASED,     // the turn's audio is no longer read
    EV_TTS_FIRST_AUDIO, // i = ttfb ms
    EV_TTS_DONE,        // every chunk is queued to the speaker
    EV_TTS_ERROR,       // p = or_err_t
    EV_SET_MODE,        // i = 0 assistant, 1 translator; j = 1 save now (console), 0 debounced (UI)
    EV_SET_TEXT_ONLY,   // i = value; j as for EV_SET_MODE
    EV_SET_DIR,         // i = translator direction: 0 auto, 1 A -> B, 2 B -> A
    EV_SETTING,         // p = key that changed (settings_on_change), "*" after a reset
    EV_BUTTON,          // i = board_btn_t, j = board_btn_ev_t, k = held ms
    EV_UI,              // i = ui_input_t (other than push-to-talk and text-only), j = its argument
    EV_WAKE,            // a touch while the screen was dimmed or off
    EV_SYNC,            // no-op: lets a console command wait until its events were handled
} app_evt_type_t;

// Where a push-to-talk press came from
typedef enum { APP_SRC_ORB, APP_SRC_CHIP, APP_SRC_BOOT, APP_SRC_CONSOLE } app_src_t;

typedef struct {
    app_evt_type_t type;
    uint32_t turn;      // AI / TTS events: the turn they belong to
    int32_t i, j, k;
    void *p;
} app_evt_t;
