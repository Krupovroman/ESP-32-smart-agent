// App dev-console commands (architecture 7.3): the whole push-to-talk flow without touching the device.
//   app state | say <text> | wait [state] [ms] | vad [snr_db min_ms heard_ms] | dir 0|1|2 | wake   (state = app state)
//   ptt down [a|b] | up [held_ms] | tap      cancel      mode assistant|translator      textonly 0|1
//   btn boot|pwr click|long|down|up          (simulated keys, through the button driver's debounce)
// With injected audio: "mic load ..." + "mic src inject fast", then "ptt tap" (latched; the end of the
// clip sends it) or "ptt down" / "ptt up 800".
#include "app_priv.h"
#include "ai_client.h"
#include "audio_in.h"
#include "devconsole.h"
#include "net.h"
#include "portal.h"
#include "tts_client.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SYNC_MS     1500        // a console event must be handled within this
#define TAP_MS      80
#define SIM_GAP_MS  60          // simulated key edges: longer than the 30 ms debounce

static const char *const s_fault_names[] = {
    "none", "nowifi", "badkey", "nocredit", "lowcredit", "region", "model", "expiring", "mic", "nokey",
};

static int need_app(void)
{
    return app_started() ? 0 : dc_fail("app not started (safe mode?)");
}

static void print_brief(void)
{
    app_status_t st;
    app_status(&st);
    printf("{\"state\":\"%s\",\"mode\":\"%s\",\"text_only\":%d,\"latched\":%d,\"ptt\":%d,\"turn\":%u}\n",
           app_state_name(st.state), st.mode ? "translator" : "assistant", st.text_only, st.latched, st.ptt_held,
           (unsigned)st.turn);
}

static int post_wait(app_evt_type_t type, int32_t i, int32_t j, void *p)
{
    app_evt_t e = { .type = type, .i = i, .j = j, .p = p };
    if (!app_post(&e, pdMS_TO_TICKS(200))) {
        return dc_fail("app queue full");
    }
    if (!app_wait_handled(SYNC_MS)) {
        return dc_fail("app did not handle the event within %d ms", SYNC_MS);
    }
    return 0;
}

static int done(int rc)
{
    if (rc == 0) {
        print_brief();
    }
    return rc;
}

static char *join_args(int argc, char **argv, int from)
{
    size_t n = 1;
    for (int i = from; i < argc; i++) {
        n += strlen(argv[i]) + 1;
    }
    if (from >= argc) {
        return NULL;
    }
    char *s = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!s) {
        return NULL;
    }
    s[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) {
            strcat(s, " ");
        }
        strcat(s, argv[i]);
    }
    return s;
}

static int cmd_state(void)
{
    app_status_t st;
    app_status(&st);
    net_status_t ns;
    net_status(&ns);
    ai_last_t al;
    bool have_last = ai_client_last(&al);
    tts_stats_t ts;
    tts_client_last_stats(&ts);
    char ssid[72], model[136], voice[136];
    app_json_esc(ssid, sizeof(ssid), ns.ssid);
    app_json_esc(model, sizeof(model), have_last ? al.model : "");
    app_json_esc(voice, sizeof(voice), ts.voice);
    printf("{\"state\":\"%s\",\"mode\":\"%s\",\"tr_dir\":%u,\"text_only\":%d,\"latched\":%d,\"ptt\":%d,\"turn\":%u,"
           "\"clip_kept\":%d,\"kept\":%u,\"kept_kb\":%u,\"screen\":\"%s\",\"hints\":%d,\"fault\":\"%s\",\"low_mem\":%d,",
           app_state_name(st.state), st.mode ? "translator" : "assistant", st.tr_dir, st.text_only, st.latched,
           st.ptt_held, (unsigned)st.turn, st.clip_kept, (unsigned)st.kept, (unsigned)st.kept_kb, st.screen, st.hints,
           (unsigned)st.fault < sizeof(s_fault_names) / sizeof(s_fault_names[0]) ? s_fault_names[st.fault] : "?",
           st.low_mem);
    printf("\"net\":{\"state\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"portal\":%d},",
           net_state_name(ns.state), ssid, ns.rssi, ns.ip, portal_active());
    printf("\"ai\":{\"rec_ms\":%u,\"upload_ms\":%u,\"ai_ms\":%u,\"total_ms\":%u,\"last_err\":\"%s\",\"model\":\"%s\","
           "\"cost_session\":%.6f,\"history\":%d},",
           (unsigned)st.t_rec_ms, (unsigned)st.t_upload_ms, (unsigned)st.t_ai_ms, (unsigned)st.t_total_ms,
           st.last_err, model, ai_client_session_cost(), ai_client_history_len());
    printf("\"tts\":{\"voice\":\"%s\",\"rate\":%u,\"ttfb_ms\":%u,\"chunks\":%u,\"bytes\":%u},", voice,
           (unsigned)ts.rate, (unsigned)st.t_tts_ttfb_ms, (unsigned)ts.chunks_done, (unsigned)ts.bytes);
    printf("\"gate\":{\"verdict\":\"%s\",\"ms\":%u,\"speech_ms\":%u,\"floor\":%.1f,\"thr\":%.1f,\"peak\":%.1f},",
           st.gate, (unsigned)st.gate_ms, (unsigned)st.gate_speech_ms, st.gate_floor_x10 / 10.0,
           st.gate_thr_x10 / 10.0, st.gate_peak_x10 / 10.0);
    if (st.ambient_x10 == VAD_NO_HINT) {
        printf("\"ambient\":null,");
    } else {
        printf("\"ambient\":%.1f,", st.ambient_x10 / 10.0);
    }
    printf("\"mic_src\":\"%s\",\"bat_mv\":%d,\"bat_pct\":%d,\"heap\":{\"int\":%u,\"int_min\":%u,\"psram\":%u}}\n",
           audio_in_get_source() == AUDIO_SRC_INJECT ? "inject" : "mic", st.bat_mv, st.bat_pct,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_wait(int argc, char **argv)
{
    const char *want = argc > 2 ? argv[2] : "idle";
    uint32_t ms = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 30000;
    int target = -1;
    for (int i = 0; i < APP__N; i++) {
        if (!strcmp(want, app_state_name((app_state_t)i))) {
            target = i;
        }
    }
    if (target < 0) {
        return dc_fail("states: idle listening uploading thinking speaking error");
    }
    for (uint32_t waited = 0;; waited += 20) {
        if ((int)app_state() == target) {
            return done(0);
        }
        if (waited >= ms) {
            dc_fail("still %s after %u ms", app_state_name(app_state()), (unsigned)ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static int cmd_vad(int argc, char **argv)
{
    vad_cfg_t c;
    app_vad_cfg(&c, NULL);
    if (argc > 2) {
        c.snr_db_x10 = (int)(strtof(argv[2], NULL) * 10.0f);
        if (argc > 3) {
            c.min_speech_ms = (uint32_t)strtoul(argv[3], NULL, 10);
        }
        if (argc > 4) {
            c.heard_ms = (uint32_t)strtoul(argv[4], NULL, 10);
        }
        if (c.snr_db_x10 < 30 || c.snr_db_x10 > 400 || c.heard_ms > c.min_speech_ms) {
            return dc_fail("snr 3..40 dB, heard_ms <= min_ms");
        }
        app_vad_cfg(NULL, &c);      // RAM only: for tuning on the bench
    }
    printf("{\"snr_db\":%.1f,\"hang_db\":%.1f,\"abs_min_db\":%.1f,\"min_speech_ms\":%u,\"heard_ms\":%u}\n",
           c.snr_db_x10 / 10.0, c.hang_db_x10 / 10.0, c.abs_min_db_x10 / 10.0, (unsigned)c.min_speech_ms,
           (unsigned)c.heard_ms);
    return 0;
}

static int cmd_app(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    int rc = need_app();
    if (rc) {
        return rc;
    }
    if (!strcmp(sub, "state")) {
        return cmd_state();
    }
    if (!strcmp(sub, "say")) {
        char *t = join_args(argc, argv, 2);
        if (!t) {
            return dc_fail("usage: app say <text...>");
        }
        return done(post_wait(EV_TEXT_SUBMIT, 0, 0, t));
    }
    if (!strcmp(sub, "wait")) {
        return cmd_wait(argc, argv);
    }
    if (!strcmp(sub, "vad")) {
        return cmd_vad(argc, argv);
    }
    if (!strcmp(sub, "dir") && argc > 2) {
        return done(post_wait(EV_SET_DIR, atoi(argv[2]), 0, NULL));
    }
    if (!strcmp(sub, "wake")) {
        // scripted UI taps after a pause: the first touch on a dimmed screen would only wake it
        return done(post_wait(EV_WAKE, 0, 0, NULL));
    }
    return dc_fail("usage: app state | say <text> | wait [state] [ms] | vad [snr_db min_ms heard_ms] | dir 0|1|2 | "
                   "wake");
}

// architecture 7.3 "state": the same JSON as "app state"
static int cmd_state_alias(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int rc = need_app();
    return rc ? rc : cmd_state();
}

static int cmd_ptt(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    int rc = need_app();
    if (rc) {
        return rc;
    }
    if (!strcmp(sub, "down")) {
        int side = UI_SIDE_AUTO;
        if (argc > 2) {
            side = !strcmp(argv[2], "a") ? UI_SIDE_A : !strcmp(argv[2], "b") ? UI_SIDE_B : UI_SIDE_AUTO;
        }
        return done(post_wait(EV_PTT_DOWN, side, APP_SRC_CONSOLE, NULL));
    }
    if (!strcmp(sub, "up")) {
        int32_t held = argc > 2 ? (int32_t)strtol(argv[2], NULL, 10) : -1;
        return done(post_wait(EV_PTT_UP, held, APP_SRC_CONSOLE, NULL));
    }
    if (!strcmp(sub, "tap")) {
        rc = post_wait(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_CONSOLE, NULL);
        if (rc) {
            return rc;
        }
        vTaskDelay(pdMS_TO_TICKS(TAP_MS));
        return done(post_wait(EV_PTT_UP, TAP_MS, APP_SRC_CONSOLE, NULL));
    }
    return dc_fail("usage: ptt down [a|b] | up [held_ms] | tap");
}

static int cmd_cancel(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int rc = need_app();
    return rc ? rc : done(post_wait(EV_CANCEL, 0, 0, NULL));
}

static int cmd_mode(int argc, char **argv)
{
    int rc = need_app();
    if (rc) {
        return rc;
    }
    const char *m = argc > 1 ? argv[1] : "";
    int v = !strcmp(m, "translator") || !strcmp(m, "1") ? 1 : !strcmp(m, "assistant") || !strcmp(m, "0") ? 0 : -1;
    if (v < 0) {
        return dc_fail("usage: mode assistant|translator");
    }
    return done(post_wait(EV_SET_MODE, v, 1, NULL));
}

static int cmd_textonly(int argc, char **argv)
{
    int rc = need_app();
    if (rc) {
        return rc;
    }
    if (argc < 2 || (strcmp(argv[1], "0") && strcmp(argv[1], "1"))) {
        return dc_fail("usage: textonly 0|1");
    }
    return done(post_wait(EV_SET_TEXT_ONLY, argv[1][0] == '1', 1, NULL));
}

static int cmd_btn(int argc, char **argv)
{
    int rc = need_app();
    if (rc) {
        return rc;
    }
    const char *bn = argc > 1 ? argv[1] : "", *ev = argc > 2 ? argv[2] : "click";
    board_btn_t b;
    if (!strcmp(bn, "boot")) {
        b = BOARD_BTN_BOOT;
    } else if (!strcmp(bn, "pwr")) {
        b = BOARD_BTN_PWR;
    } else {
        return dc_fail("usage: btn boot|pwr click|long|down|up");
    }
    // the simulated level goes through the same poller, debounce and event mapping as the real key
    if (!strcmp(ev, "down")) {
        board_buttons_simulate(b, true);
    } else if (!strcmp(ev, "up")) {
        board_buttons_simulate(b, false);
    } else if (!strcmp(ev, "click") || !strcmp(ev, "long")) {
        uint32_t hold = !strcmp(ev, "click") ? 120 : (b == BOARD_BTN_PWR ? 3200 : 1200);
        board_buttons_simulate(b, true);
        vTaskDelay(pdMS_TO_TICKS(hold));
        board_buttons_simulate(b, false);
    } else {
        return dc_fail("usage: btn boot|pwr click|long|down|up");
    }
    vTaskDelay(pdMS_TO_TICKS(SIM_GAP_MS));
    return done(post_wait(EV_SYNC, 0, 0, NULL));
}

void app_console_register(void)
{
    devconsole_register("app",
                        "app state | say <text> | wait [state] [ms] | vad [snr min_ms heard_ms] | dir 0|1|2 | wake",
                        cmd_app);
    devconsole_register("state", "state - app, net, ai and tts status (same as app state)", cmd_state_alias);
    devconsole_register("ptt", "ptt down [a|b] | up [held_ms] | tap - push-to-talk as the orb / BOOT", cmd_ptt);
    devconsole_register("cancel", "cancel - stop the recording, turn or speech", cmd_cancel);
    devconsole_register("mode", "mode assistant|translator", cmd_mode);
    devconsole_register("textonly", "textonly 0|1 - replies in text only / spoken", cmd_textonly);
    devconsole_register("btn", "btn boot|pwr click|long|down|up - simulated keys", cmd_btn);
}
