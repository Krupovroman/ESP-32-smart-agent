// App state machine (architecture 1.12 / 2.3, docs/ui.md 5.1-5.6): push-to-talk (hold and tap), the
// speech gate, assistant and translator turns, speech output, cancel, barge-in and errors. The state
// lives in the app task only; the UI, buttons, AI workers, settings and console post events.
#include "app_priv.h"
#include "audio_in.h"
#include "audio_out.h"
#include "devconsole.h"
#include "orhttp.h"
#include "settings.h"
#include "tts_client.h"
#include "ui.h"
#include "ui_str.h"
#include "ui_sym.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app";

#define APP_Q_DEPTH     24
#define APP_STACK       6144        // internal RAM: the app task writes settings (NVS)
#define APP_PRIO        5
#define APP_CORE        1
#define TICK_FAST_MS    20          // recording or a press pending: live speech detection, 400 ms hold
#define TICK_MS         100
#define CARD_REPEAT_MS  20000       // the same error card again within this time: orb and status only

typedef enum { TURN_NONE, TURN_VOICE, TURN_TEXT, TURN_REPLAY } turn_kind_t;

typedef enum { RDY_OK, RDY_PORTAL, RDY_NET, RDY_KEY, RDY_MEM, RDY_AI } ready_t;

typedef struct {
    app_state_t st;
    uint32_t st_t0;
    uint32_t turn;                  // turn recorded / in flight, 0 = none (late events are dropped)
    turn_kind_t kind;
    int mode;                       // RAM value; persisted with a debounce
    bool text_only;
    int tr_dir;
    // push-to-talk
    bool held;                      // a press (orb, chip, BOOT, console) is down
    app_src_t held_src;             // its source: only that source's release ends it
    uint32_t press_t;
    int8_t press_side;
    bool ignore_up;                 // that press sent a latched recording: its release does nothing
    bool pending;                   // pressed while busy: becomes a recording if still held at APP_TAP_MS
    uint32_t pending_preroll;       // pre-roll of that recording
    bool boot_ignored;              // the UI context ignored BOOT: swallow its release as well
    // recording
    bool latched, rec_vad;
    int8_t rec_side;                // forced translator source of this recording, UI_SIDE_AUTO = detect
    uint32_t rec_t0, rec_max_ms;
    vad_live_t vad;
    size_t vad_pos;
    // turn in flight
    bool placeholder;               // a waveform bubble waits for its transcript
    bool want_tts, tts_first, tts_done, revealed;
    uint32_t spk_quiet_t;           // when the speaker last went quiet after a turn's speech (0 = never)
    char *reply;                    // voice on: shown when its audio starts
    uint32_t t_submit, t_result, t_first;
    uint32_t est_upload_ms;
    int upload_pct;
    // kept for "Не тот язык", "Повторить" and the reader's replay (valid until the next recording)
    bool clip_ok;
    int clip_mode;
    int8_t clip_side;
    const int16_t *clip_pcm;
    size_t clip_samples;
    uint32_t clip_rate;
    turn_kind_t last_kind;
    char *last_text;
    char *last_tr;
    char last_tr_lang[8];
    // gate, faults, hints, chat
    vad_result_t gate;
    bool gate_valid;
    int fault;
    int hints;
    int last_card;
    uint32_t last_card_t;
    uint32_t last_end_t;
    bool chat_used[2];
    // timings of the last turn
    uint32_t t_rec_ms, t_upload_ms, t_ai_ms, t_ttfb_ms, t_total_ms;
    char last_err[8];
} app_t;

static app_t s_a;
static QueueHandle_t s_q;
static TaskHandle_t s_task;
static atomic_uint s_posted, s_handled;
static atomic_uint s_turn_pub;      // current turn, for the AI listener's filter
static atomic_int s_state_pub = APP_IDLE;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static app_status_t s_pub;          // under s_mux
static vad_cfg_t s_vcfg;            // under s_mux (the console tunes it)

// ---- small helpers

uint32_t app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int32_t app_cfg_int(const char *key, int32_t def)
{
    return settings_find(key) ? settings_get_int(key) : def;
}

static char *ps_strdup(const char *t)
{
    size_t n = strlen(t) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (d) {
        memcpy(d, t, n);
    }
    return d;
}

static const char *loc(const char *ru, const char *en)
{
    return ui_str_lang() ? en : ru;
}

size_t app_json_esc(char *out, size_t len, const char *in)
{
    size_t o = 0;
    if (!len) {
        return 0;
    }
    for (; in && *in && o + 7 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, len - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return o;
}

const char *app_state_name(app_state_t st)
{
    static const char *const n[APP__N] = { "idle", "listening", "uploading", "thinking", "speaking", "error" };
    return (unsigned)st < APP__N ? n[st] : "?";
}

static const char *gate_name(vad_verdict_t v)
{
    static const char *const n[] = { "ok", "too_short", "no_speech", "silent" };
    return (unsigned)v < sizeof(n) / sizeof(n[0]) ? n[v] : "?";
}

void app_evt_free(app_evt_t *e)
{
    if (!e->p) {
        return;
    }
    if (e->type == EV_AI_RESULT) {
        app_result_t *r = e->p;
        free(r->transcript);
        free(r->text);
    }
    free(e->p);
    e->p = NULL;
}

void app_vad_cfg(vad_cfg_t *get, const vad_cfg_t *set)
{
    taskENTER_CRITICAL(&s_mux);
    if (set) {
        s_vcfg = *set;
    }
    if (get) {
        *get = s_vcfg;
    }
    taskEXIT_CRITICAL(&s_mux);
}

// ---- status for other tasks

static void publish(void)
{
    app_status_t p;
    memset(&p, 0, sizeof(p));
    p.state = s_a.st;
    p.mode = (uint8_t)s_a.mode;
    p.tr_dir = (uint8_t)s_a.tr_dir;
    p.text_only = s_a.text_only;
    p.latched = s_a.latched;
    p.ptt_held = s_a.held;
    p.clip_kept = s_a.clip_ok;
    p.turn = s_a.turn;
    strlcpy(p.last_err, s_a.last_err, sizeof(p.last_err));
    p.t_rec_ms = s_a.t_rec_ms;
    p.t_upload_ms = s_a.t_upload_ms;
    p.t_ai_ms = s_a.t_ai_ms;
    p.t_tts_ttfb_ms = s_a.t_ttfb_ms;
    p.t_total_ms = s_a.t_total_ms;
    p.gate = s_a.gate_valid ? gate_name(s_a.gate.verdict) : "";
    p.gate_speech_ms = s_a.gate.speech_ms;
    p.gate_ms = s_a.gate.total_ms;
    p.gate_floor_x10 = s_a.gate.floor_db_x10;
    p.gate_thr_x10 = s_a.gate.thr_db_x10;
    p.gate_peak_x10 = s_a.gate.peak_db_x10;
    p.ambient_x10 = app_io_ambient();
    p.hints = s_a.hints;
    p.fault = s_a.fault;
    p.low_mem = app_io_low_mem();
    taskENTER_CRITICAL(&s_mux);
    s_pub = p;
    taskEXIT_CRITICAL(&s_mux);
    atomic_store(&s_state_pub, (int)s_a.st);
    atomic_store(&s_turn_pub, s_a.turn);
}

void app_status(app_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_pub;
    taskEXIT_CRITICAL(&s_mux);
    app_screen_t sc = app_io_screen();
    out->screen = sc == APP_SCREEN_ON ? "on" : sc == APP_SCREEN_DIM ? "dim" : "off";
    out->bat_mv = app_io_bat_mv();
    out->bat_pct = board_battery_pct(out->bat_mv);
}

app_state_t app_state(void)
{
    return (app_state_t)atomic_load(&s_state_pub);
}

bool app_started(void)
{
    return s_task != NULL;
}

bool app_quiet(void)
{
    return (s_a.st == APP_IDLE || s_a.st == APP_ERROR) && !s_a.held && !s_a.pending;
}

static void set_state(app_state_t st)
{
    if (st != s_a.st) {
        devcon_event("state", "\"from\":\"%s\",\"to\":\"%s\",\"turn\":%u", app_state_name(s_a.st), app_state_name(st),
                     (unsigned)s_a.turn);
        s_a.st = st;
        s_a.st_t0 = app_now_ms();
    }
    publish();
}

// ---- UI shortcuts

static void ui_i(ui_msg_type_t t, int32_t i)
{
    ui_post(&(ui_msg_t){ .type = t, .i = i });
}

static void ui_state(ui_state_t st)
{
    ui_i(UI_SET_STATE, st);
}

static void ui_status_id(ui_sid_t id)
{
    ui_i(UI_STATUS, id);       // s = NULL: the UI picks the string in its language
}

static void ui_toast_id(ui_sid_t id)
{
    ui_i(UI_TOAST, id);
}

static void clear_fault(void)
{
    if (s_a.fault != UI_FAULT_NONE) {
        s_a.fault = UI_FAULT_NONE;
        ui_i(UI_SET_FAULT, UI_FAULT_NONE);
    }
}

static void tr_langs(char *a, char *b)
{
    settings_get_str("tr_lang_a", a, 8);
    settings_get_str("tr_lang_b", b, 8);
}

// ---- errors (ui.md 3.14 / 5.6): or_err_t id -> chat card, pill fault, and whether msg_ru is the body

typedef struct {
    const char *id;
    int8_t card, fault;
    bool body;
} err_map_t;

static const err_map_t ERR_MAP[] = {
    { "N0", UI_CARD_NO_WIFI, -1, false },
    { "N1", UI_CARD_TLS, -1, true }, { "N3", UI_CARD_TLS, -1, true }, { "N4", UI_CARD_TLS, -1, true },
    { "N2", UI_CARD_SERVER, -1, true }, { "N5", UI_CARD_SERVER, -1, true }, { "N6", UI_CARD_SERVER, -1, true },
    { "N7", UI_CARD_SERVER, -1, true }, { "N8", UI_CARD_SERVER, -1, true }, { "408", UI_CARD_SERVER, -1, true },
    { "500", UI_CARD_SERVER, -1, true }, { "502", UI_CARD_SERVER, -1, true }, { "503", UI_CARD_SERVER, -1, true },
    { "504", UI_CARD_SERVER, -1, true }, { "529", UI_CARD_SERVER, -1, true },
    { "401", UI_CARD_BAD_KEY, UI_FAULT_BAD_KEY, true },
    { "K0", UI_CARD_GENERIC, UI_FAULT_BAD_KEY, true },
    { "402", UI_CARD_NO_CREDIT, UI_FAULT_NO_CREDIT, false },
    { "402k", UI_CARD_NO_CREDIT, UI_FAULT_NO_CREDIT, true },
    { "403b", UI_CARD_NO_CREDIT, UI_FAULT_NO_CREDIT, true },
    { "402w", UI_CARD_RATE, -1, true }, { "429", UI_CARD_RATE, -1, true },
    { "403r", UI_CARD_REGION, UI_FAULT_REGION, false },
    { "404", UI_CARD_MODEL, UI_FAULT_MODEL, true },
    { "MIC", UI_CARD_MIC, UI_FAULT_MIC, false },
};

static void show_error(const char *id, const char *msg)
{
    int card = UI_CARD_GENERIC, fault = -1;
    bool body = true;
    if (id[0] == 'H' && id[1] == '5') {
        card = UI_CARD_SERVER;      // 5xx without a row of its own
    }
    for (size_t i = 0; i < sizeof(ERR_MAP) / sizeof(ERR_MAP[0]); i++) {
        if (!strcmp(ERR_MAP[i].id, id)) {
            card = ERR_MAP[i].card;
            fault = ERR_MAP[i].fault;
            body = ERR_MAP[i].body;
            break;
        }
    }
    strlcpy(s_a.last_err, id, sizeof(s_a.last_err));
    char esc[200];
    app_json_esc(esc, sizeof(esc), msg ? msg : "");
    devcon_event("error", "\"id\":\"%s\",\"msg\":\"%s\",\"turn\":%u", id, esc, (unsigned)s_a.turn);
    uint32_t now = app_now_ms();
    if (card == s_a.last_card && now - s_a.last_card_t < CARD_REPEAT_MS) {
        ui_state(UI_ST_ERROR);      // a repeated press: no second identical card
    } else {
        ui_post_text(UI_ERROR, card, body ? msg : NULL);
        s_a.chat_used[s_a.mode] = true;
    }
    s_a.last_card = card;
    s_a.last_card_t = now;
    if (fault >= 0 && fault != s_a.fault) {
        s_a.fault = fault;
        ui_i(UI_SET_FAULT, fault);
    }
    set_state(APP_ERROR);
}

static void show_local_error(const char *id)
{
    or_err_t e;
    or_err_local(&e, id);
    show_error(e.id, e.msg_ru);
}

static ready_t ready(void)
{
    if (app_io_portal()) {
        return RDY_PORTAL;
    }
    if (!app_io_net_up()) {
        return RDY_NET;
    }
    if (!settings_is_set("ai_key")) {
        return RDY_KEY;
    }
    if (!ai_client_ready()) {
        return RDY_AI;
    }
    if (app_io_low_mem()) {
        return RDY_MEM;
    }
    return RDY_OK;
}

static void show_not_ready(ready_t r)
{
    switch (r) {
    case RDY_PORTAL: show_error("PT", loc("Закройте настройку сети", "Close the phone setup first")); break;
    case RDY_NET: show_local_error("N0"); break;
    case RDY_KEY: show_local_error("K0"); break;
    case RDY_MEM: show_local_error("M0"); break;
    default: show_error("AI", loc("ИИ не запущен", "AI is not running")); break;
    }
}

static bool s_no_speaker;   // Settings -> "Динамик": none (RAM only, ui.md 5.2)

static bool voice_on(void)
{
    return !s_a.text_only && !s_no_speaker && app_cfg_int("volume", 60) > 0;
}

// ---- turn bookkeeping

static void close_placeholder(void)
{
    // a waveform bubble that never got its transcript, so a later typed text does not land in it
    if (s_a.placeholder) {
        ui_post_text(UI_USER_MSG, UI_SIDE_AUTO, TXT_ELLIPSIS);
        s_a.placeholder = false;
    }
}

static void turn_reset(void)
{
    if (s_a.want_tts && (s_a.tts_first || audio_out_busy())) {
        s_a.spk_quiet_t = app_now_ms();     // the speaker played: its tail must stay out of the next pre-roll
    }
    free(s_a.reply);
    s_a.reply = NULL;
    s_a.want_tts = s_a.tts_first = s_a.tts_done = s_a.revealed = false;
    s_a.latched = false;
    s_a.turn = 0;
    s_a.kind = TURN_NONE;
    publish();
}

// A new turn id, visible to the AI listener before anything is submitted (an immediate error of the
// ai task must not be filtered out as a stale turn).
static void begin_turn(turn_kind_t kind)
{
    s_a.turn = ai_client_new_turn();
    s_a.kind = kind;
    atomic_store(&s_turn_pub, s_a.turn);
}

static void finish_turn(ui_state_t ui)
{
    uint32_t now = app_now_ms();
    if (s_a.t_submit) {
        s_a.t_total_ms = now - s_a.t_submit;
    }
    s_a.last_end_t = now;
    turn_reset();
    ui_state(ui);
    set_state(APP_IDLE);
}

static void reveal(bool tts_failed)
{
    if (!s_a.revealed && s_a.reply) {
        ui_post_text(UI_BOT_MSG, tts_failed ? 1 : 0, s_a.reply);
        s_a.revealed = true;
    }
}

// Stops every worker of the current turn: chat, speech, recording.
static void cancel_turn(void)
{
    if (s_a.turn) {
        ai_client_cancel(s_a.turn);     // chat and TTS of every turn up to this one
    }
    if (s_a.want_tts) {
        audio_out_stop();
    }
    if (audio_in_recording()) {
        audio_in_stop(NULL);
    }
    close_placeholder();
    reveal(false);                  // a reply that already arrived (waiting for its audio) stays on screen
    turn_reset();
}

// ui.md 3.3: after the idle time the model context resets and a divider marks the new conversation
static void maybe_divider(void)
{
    int32_t idle_min = app_cfg_int("hist_idle_min", 10);
    uint32_t now = app_now_ms();
    if (!s_a.chat_used[s_a.mode] || !s_a.last_end_t || idle_min <= 0 ||
        now - s_a.last_end_t < (uint32_t)idle_min * 60000u) {
        return;
    }
    char hhmm[8] = "";
    time_t t = time(NULL);
    if (t > 1735689600) {           // clock set by SNTP
        if (!getenv("TZ")) {
            setenv("TZ", "MSK-3", 1);
            tzset();
        }
        struct tm lt;
        localtime_r(&t, &lt);
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", lt.tm_hour % 24, lt.tm_min % 60);
    }
    ui_post_text(UI_DIVIDER, 0, hhmm);
    ai_client_history_clear();
}

// ---- recording

// ui.md 5.1: the pre-roll never holds the tail of the device's own speech; after a barge-in it holds what the
// user said once the speech was flushed (the press itself is only decided at 400 ms)
static uint32_t preroll_cap(uint32_t want)
{
    if (!s_a.spk_quiet_t) {
        return want;
    }
    uint32_t since = app_now_ms() - s_a.spk_quiet_t;
    uint32_t room = since > APP_SPK_TAIL_MS ? since - APP_SPK_TAIL_MS : 0;
    return want < room ? want : room;
}

// preroll_ms: microphone audio from before the press that is kept (ui.md 5.1)
static void start_recording(int8_t side, uint32_t preroll_ms)
{
    ready_t r = ready();
    if (r != RDY_OK) {
        show_not_ready(r);
        return;
    }
    preroll_ms = preroll_cap(preroll_ms);
    maybe_divider();
    begin_turn(TURN_VOICE);
    s_a.t_submit = 0;
    ai_client_prewarm();            // the TLS handshake (2-3 s on the first request) runs while the user speaks
    uint32_t max_ms = (uint32_t)app_cfg_int("max_rec_s", 20) * 1000u;
    s_a.clip_ok = false;            // this recording overwrites the kept clip
    if (audio_in_start_ex(max_ms, preroll_ms) != ESP_OK) {
        turn_reset();
        show_error("MIC", NULL);
        return;
    }
    s_a.rec_max_ms = max_ms;
    s_a.rec_t0 = app_now_ms();
    s_a.latched = false;
    s_a.rec_side = UI_SIDE_AUTO;
    if (s_a.mode == 1) {
        // a held chip forces its language; otherwise the persistent direction (M -> Направление)
        s_a.rec_side = side == UI_SIDE_A || side == UI_SIDE_B ? side
                       : s_a.tr_dir == 1                      ? UI_SIDE_A
                       : s_a.tr_dir == 2                      ? UI_SIDE_B
                                                              : UI_SIDE_AUTO;
    }
    s_a.rec_vad = app_cfg_int(s_a.mode ? "tr_vad" : "vad_stop", s_a.mode ? 1 : 0) != 0;
    bool mic = audio_in_get_source() == AUDIO_SRC_MIC;
    vad_cfg_t c;
    app_vad_cfg(&c, NULL);
    vad_live_init(&s_a.vad, &c, mic ? app_io_ambient() : VAD_NO_HINT);
    s_a.vad_pos = 0;
    devcon_event("rec", "\"phase\":\"start\",\"turn\":%u,\"src\":\"%s\",\"side\":%d,\"max_ms\":%u,\"preroll_ms\":%u",
                 (unsigned)s_a.turn, mic ? "mic" : "inject", s_a.rec_side, (unsigned)max_ms,
                 mic ? (unsigned)preroll_ms : 0u);
    ui_state(UI_ST_REC);
    set_state(APP_LISTENING);
}

static void discard_recording(bool notify)
{
    if (audio_in_recording()) {
        audio_in_stop(NULL);
    }
    devcon_event("rec", "\"phase\":\"discard\",\"turn\":%u", (unsigned)s_a.turn);
    turn_reset();
    if (notify) {
        ui_status_id(S_ST_CANCELLED);
    }
    ui_state(UI_ST_IDLE);
    set_state(APP_IDLE);
}

// Sends a clip of s_a.turn (new recording or a re-run of the kept one).
static bool submit_clip(const int16_t *pcm, size_t samples, uint32_t rate, int8_t side, int floor_x10)
{
    char a[8], b[8];
    tr_langs(a, b);
    const char *src = NULL;
    if (s_a.mode == 1) {
        src = side == UI_SIDE_A ? a : side == UI_SIDE_B ? b : NULL;
    }
    ai_request_t q = {
        .turn = s_a.turn, .mode = s_a.mode ? AI_MODE_TRANSLATOR : AI_MODE_ASSISTANT,
        .pcm = pcm, .samples = samples, .rate = rate, .src_lang = src,
    };
    esp_err_t e = ai_client_submit(&q);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "submit: %s", esp_err_to_name(e));
        show_local_error("M0");
        return false;
    }
    s_a.clip_ok = true;
    s_a.clip_mode = s_a.mode;
    s_a.clip_side = side;
    s_a.clip_pcm = pcm;
    s_a.clip_samples = samples;
    s_a.clip_rate = rate;
    s_a.last_kind = TURN_VOICE;
    s_a.t_submit = app_now_ms();
    // progress estimate for the orb arc: about 250 KB/s upstream (measured), plus request latency
    s_a.est_upload_ms = 600 + (uint32_t)(or_wav_b64_len(samples) / 256);
    s_a.upload_pct = 0;
    // A16: the waveform bubble appears at once; the transcript replaces it
    ui_post_text(UI_USER_MSG, UI_SIDE_AUTO, NULL);
    uint8_t *env = heap_caps_malloc(32, MALLOC_CAP_SPIRAM);
    if (env) {
        vad_envelope(pcm, samples, floor_x10, env);
        ui_post(&(ui_msg_t){ .type = UI_USER_WAVE, .s = (char *)env });
    }
    s_a.placeholder = true;
    s_a.chat_used[s_a.mode] = true;
    ui_i(UI_UPLOAD_PCT, 0);
    ui_state(UI_ST_UPLOAD);
    set_state(APP_UPLOADING);
    return true;
}

static void send_recording(const char *why)
{
    audio_clip_t clip;
    audio_in_stop(&clip);
    s_a.latched = false;
    s_a.t_rec_ms = clip.ms;
    bool mic = audio_in_get_source() == AUDIO_SRC_MIC;
    vad_result_t g;
    if (clip.rate == AUDIO_IN_RATE) {
        vad_cfg_t c;
        app_vad_cfg(&c, NULL);
        vad_analyse(clip.pcm, clip.samples, &c, mic ? app_io_ambient() : VAD_NO_HINT, &g);
    } else {
        memset(&g, 0, sizeof(g));       // dev capture rates: no gate
        g.verdict = VAD_OK;
        g.total_ms = clip.ms;
        g.floor_db_x10 = g.thr_db_x10 = g.peak_db_x10 = g.rms_db_x10 = -600;
    }
    s_a.gate = g;
    s_a.gate_valid = true;
    devcon_event("rec",
                 "\"phase\":\"end\",\"why\":\"%s\",\"turn\":%u,\"ms\":%u,\"speech_ms\":%u,\"longest_ms\":%u,"
                 "\"floor\":%.1f,\"thr\":%.1f,\"peak\":%.1f,\"rms\":%.1f,\"verdict\":\"%s\"",
                 why, (unsigned)s_a.turn, (unsigned)clip.ms, (unsigned)g.speech_ms, (unsigned)g.longest_ms,
                 g.floor_db_x10 / 10.0, g.thr_db_x10 / 10.0, g.peak_db_x10 / 10.0, g.rms_db_x10 / 10.0,
                 gate_name(g.verdict));
    if (g.verdict == VAD_SILENT_INPUT && mic) {
        turn_reset();
        show_error("MIC", NULL);        // flat zeros from the microphone: the mic is dead (ui.md 5.6)
        return;
    }
    if (g.verdict != VAD_OK) {
        // noise or silence only (or too short): never sent, the model would invent a transcript
        bool too_short = g.verdict == VAD_TOO_SHORT;
        ui_status_id(too_short ? S_ST_TOO_SHORT : S_ST_NOT_HEARD);
        if (!too_short) {
            ui_i(UI_SET_MOOD, UI_MOOD_SAD);
        }
        s_a.last_end_t = app_now_ms();
        turn_reset();
        ui_state(UI_ST_IDLE);
        set_state(APP_IDLE);
        return;
    }
    if (mic && s_a.fault == UI_FAULT_MIC) {
        clear_fault();
    }
    ready_t r = ready();
    if (r != RDY_OK) {
        turn_reset();
        show_not_ready(r);
        return;
    }
    if (!submit_clip(clip.pcm, clip.samples, clip.rate, s_a.rec_side, g.floor_db_x10)) {
        turn_reset();
    }
}

static void rec_tick(uint32_t now)
{
    const int16_t *pcm;
    size_t n = audio_in_peek(&pcm);
    if (n >= s_a.vad_pos + VAD_FRAME) {
        size_t whole = (n - s_a.vad_pos) / VAD_FRAME * VAD_FRAME;
        vad_live_feed(&s_a.vad, pcm + s_a.vad_pos, whole);
        s_a.vad_pos += whole;
    }
    if (!audio_in_recording()) {
        send_recording("limit");        // max_rec_s reached, or the end of an injected clip
        return;
    }
    if ((s_a.latched || s_a.rec_vad) && s_a.vad.speech_ms >= APP_VAD_SPEECH_MS &&
        s_a.vad.silence_ms >= APP_VAD_SILENCE_MS) {
        send_recording("vad");
        return;
    }
    if (s_a.latched && s_a.vad.speech_ms < s_a.vad.cfg.heard_ms && now - s_a.rec_t0 >= APP_LATCH_IDLE_MS) {
        discard_recording(false);
        ui_status_id(S_ST_NOT_HEARD);
        ui_i(UI_SET_MOOD, UI_MOOD_SAD);
        return;
    }
    if (now - s_a.rec_t0 > s_a.rec_max_ms + 2000) {
        send_recording("deadline");
    }
}

// ---- typed turns, re-runs, replay

static void text_turn(char *text)
{
    app_abort_all();
    ready_t r = ready();
    if (r != RDY_OK) {
        free(text);
        show_not_ready(r);
        return;
    }
    maybe_divider();
    begin_turn(TURN_TEXT);
    ai_request_t q = { .turn = s_a.turn, .mode = s_a.mode ? AI_MODE_TRANSLATOR : AI_MODE_ASSISTANT, .text = text };
    if (ai_client_submit(&q) != ESP_OK) {
        free(text);
        turn_reset();
        show_local_error("M0");
        return;
    }
    free(s_a.last_text);
    s_a.last_text = text;
    s_a.last_kind = TURN_TEXT;
    if (s_a.mode == 0) {
        ui_post_text(UI_USER_MSG, UI_SIDE_AUTO, text);
    } else {
        // translator: the bubble moves to the detected speaker's side when the result arrives
        ui_post_text(UI_USER_MSG, UI_SIDE_AUTO, NULL);
        s_a.placeholder = true;
    }
    s_a.chat_used[s_a.mode] = true;
    s_a.t_submit = app_now_ms();
    s_a.t_rec_ms = 0;
    ui_state(UI_ST_THINK);
    set_state(APP_THINKING);
}

static void rerun_clip(int8_t side)
{
    if (!s_a.clip_ok || s_a.clip_mode != s_a.mode) {
        ui_toast_id(S_NOT_YET);
        return;
    }
    app_abort_all();
    ready_t r = ready();
    if (r != RDY_OK) {
        show_not_ready(r);
        return;
    }
    begin_turn(TURN_VOICE);
    devcon_event("rec", "\"phase\":\"rerun\",\"turn\":%u,\"side\":%d", (unsigned)s_a.turn, side);
    if (!submit_clip(s_a.clip_pcm, s_a.clip_samples, s_a.clip_rate, side, s_a.gate.floor_db_x10)) {
        turn_reset();
    }
}

static void retry_last(void)
{
    if (!app_quiet()) {
        return;
    }
    if (s_a.last_kind == TURN_VOICE && s_a.clip_ok && s_a.clip_mode == s_a.mode) {
        rerun_clip(s_a.clip_side);
    } else if (s_a.last_kind == TURN_TEXT && s_a.last_text) {
        char *t = ps_strdup(s_a.last_text);
        if (t) {
            text_turn(t);
        }
    } else {
        ui_toast_id(S_NOT_YET);
    }
}

static void replay(void)
{
    if (!app_quiet() || !s_a.last_tr) {
        ui_toast_id(S_NOT_YET);
        return;
    }
    if (app_cfg_int("volume", 60) == 0) {
        ui_toast_id(S_E_SPK);
        return;
    }
    app_abort_all();
    begin_turn(TURN_REPLAY);
    if (tts_client_speak(s_a.turn, s_a.last_tr, s_a.last_tr_lang) != ESP_OK) {
        turn_reset();
        return;
    }
    uint32_t now = app_now_ms();
    s_a.want_tts = true;
    s_a.revealed = true;            // the text is already on screen
    s_a.t_submit = s_a.t_result = now;
    ui_state(UI_ST_THINK);
    set_state(APP_THINKING);
}

// ---- results

static void on_result(app_result_t *r)
{
    s_a.t_result = app_now_ms();
    s_a.t_ai_ms = r->t.ai_ms;
    s_a.t_upload_ms = r->t.upload_ms;
    s_a.last_err[0] = 0;
    devcon_event("ai",
                 "\"turn\":%u,\"phase\":\"result\",\"ms\":%u,\"upload_ms\":%u,\"clip_ms\":%u,\"lang\":\"%s\","
                 "\"src_lang\":\"%s\",\"tts_lang\":\"%s\",\"unheard\":%s,\"truncated\":%s,\"cost\":%.6f",
                 (unsigned)s_a.turn, (unsigned)r->t.ai_ms, (unsigned)r->t.upload_ms, (unsigned)r->t.clip_ms, r->lang,
                 r->src_lang, r->tts_lang, r->unheard ? "true" : "false", r->truncated ? "true" : "false", r->cost);
    if (r->unheard || (!r->transcript[0] && !r->text[0])) {
        close_placeholder();
        ui_status_id(S_ST_NOT_HEARD);
        ui_i(UI_SET_MOOD, UI_MOOD_SAD);
        finish_turn(UI_ST_IDLE);
        return;
    }
    if (s_a.fault != UI_FAULT_MIC) {
        clear_fault();              // the key, credit and model work again
    }
    int side = UI_SIDE_AUTO;
    if (s_a.mode == 1) {
        char a[8], b[8];
        tr_langs(a, b);
        side = !strcmp(r->src_lang, a) ? UI_SIDE_A : UI_SIDE_B;    // "other" is translated into A
    }
    if (s_a.placeholder) {
        ui_post_text(UI_USER_MSG, side, r->transcript[0] ? r->transcript : TXT_ELLIPSIS);
        s_a.placeholder = false;
    }
    if (s_a.hints > 0) {
        ui_i(UI_SET_HINTS, --s_a.hints);
    }
    bool ignore = false;
    if (s_a.mode == 1 && r->other_lang) {
        ui_post_text(UI_STATUS, 0, loc("Другой язык", "Other language"));
        ignore = app_cfg_int("tr_other", 0) == 1;     // 1 = show the transcript only
    } else if (s_a.mode == 1) {
        // ui.md 5.5: the direction ("EN → RU") is shown for 2 s
        char dir[24], s1[4] = "", s2[4] = "";
        for (int i = 0; i < 3 && r->src_lang[i]; i++) {
            s1[i] = (char)toupper((unsigned char)r->src_lang[i]);
            s1[i + 1] = 0;
        }
        for (int i = 0; i < 3 && r->lang[i]; i++) {
            s2[i] = (char)toupper((unsigned char)r->lang[i]);
            s2[i + 1] = 0;
        }
        snprintf(dir, sizeof(dir), "%s " TXT_ARROW " %s", s1, s2);
        ui_post_text(UI_STATUS, 0, dir);
    }
    if (!r->text[0] || ignore) {
        finish_turn(UI_ST_ANSWERED);
        return;
    }
    const char *note = r->truncated ? loc("(ответ обрезан)", "(reply cut off)") : "";
    size_t n = strlen(r->text) + strlen(note) + 2;
    char *show = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (show) {
        snprintf(show, n, "%s%s%s", r->text, note[0] ? "\n" : "", note);
    }
    free(s_a.reply);
    s_a.reply = show ? show : ps_strdup(r->text);
    s_a.revealed = false;
    if (s_a.mode == 1) {
        free(s_a.last_tr);
        s_a.last_tr = ps_strdup(r->text);
        strlcpy(s_a.last_tr_lang, r->tts_lang[0] ? r->tts_lang : r->lang, sizeof(s_a.last_tr_lang));
    }
    if (voice_on() && r->tts_lang[0] && tts_client_speak(s_a.turn, r->text, r->tts_lang) == ESP_OK) {
        s_a.want_tts = true;        // the text appears with its audio (ui.md 5.4)
        if (s_a.st != APP_THINKING) {
            ui_state(UI_ST_THINK);
        }
        set_state(APP_THINKING);
        return;
    }
    reveal(false);
    finish_turn(UI_ST_ANSWERED);
}

static void on_tts_error(const or_err_t *e)
{
    char esc[160];
    app_json_esc(esc, sizeof(esc), e->message);
    devcon_event("tts", "\"turn\":%u,\"phase\":\"error\",\"id\":\"%s\",\"detail\":\"%s\"", (unsigned)s_a.turn, e->id, esc);
    if (!s_a.revealed && s_a.reply) {
        reveal(true);               // the text with the "голос недоступен" note
    } else if (s_a.mode == 1 && s_a.reply) {
        ui_post_text(UI_BOT_MSG, 1, s_a.reply);    // the translation bubble gets its note
    } else {
        ui_post_text(UI_TOAST, 0, e->msg_ru);
    }
    audio_out_stop();
    finish_turn(UI_ST_IDLE);
}

// The first audio of the reply plays: the text appears with it (ui.md 5.4)
static void speaking_begin(void)
{
    s_a.tts_first = true;
    s_a.t_first = app_now_ms();
    reveal(false);
    ui_state(UI_ST_SPEAK);
    set_state(APP_SPEAKING);
}

static void stop_speaking(void)
{
    if (s_a.turn) {
        ai_client_cancel(s_a.turn);     // flushes the speaker (tts_client_cancel -> audio_out_stop)
    }
    audio_out_stop();
    reveal(false);
    finish_turn(UI_ST_IDLE);
}

static void turn_timeout(void)
{
    ESP_LOGW(TAG, "turn %u: no result in %u ms", (unsigned)s_a.turn, (unsigned)APP_TURN_MAX_MS);
    cancel_turn();
    show_local_error("N8");
}

// ---- cancel, push-to-talk

// Ends what runs: false when there was nothing (IDLE).
static bool cancel_any(bool notify)
{
    s_a.pending = false;
    switch (s_a.st) {
    case APP_LISTENING:
        discard_recording(notify);
        return true;
    case APP_THINKING:
        if (s_a.want_tts) {
            stop_speaking();        // the reply is in and waits for its audio: show it, speak nothing
            return true;
        }
        /* fall through */
    case APP_UPLOADING:
        cancel_turn();
        if (notify) {
            ui_status_id(S_ST_CANCELLED);
        }
        s_a.last_end_t = app_now_ms();
        ui_state(UI_ST_IDLE);
        set_state(APP_IDLE);
        return true;
    case APP_SPEAKING:
        stop_speaking();
        return true;
    case APP_ERROR:
        ui_state(UI_ST_IDLE);
        set_state(APP_IDLE);
        return true;
    default:
        return false;
    }
}

void app_abort_all(void)
{
    cancel_any(false);
}

void app_pwr_short(void)
{
    if (ui_context() != UI_CTX_HOME) {
        ui_pwr_short();             // back one level (ui.md 2.3)
        return;
    }
    if (!cancel_any(true)) {
        app_io_screen_off();        // H idle: screen off
    }
}

// The orb and the chips are one touch source: the UI reports their releases alike.
static app_src_t src_family(app_src_t s)
{
    return s == APP_SRC_CHIP ? APP_SRC_ORB : s;
}

void app_ptt_down(int8_t side, app_src_t src)
{
    bool woke = app_io_activity();
    if (woke && (src == APP_SRC_ORB || src == APP_SRC_CHIP)) {
        return;                     // the touch that wakes the screen is swallowed
    }
    if (src == APP_SRC_BOOT) {
        s_a.boot_ignored = !ui_boot_down();     // keyboard, dialog or onboarding: BOOT does nothing there
        if (s_a.boot_ignored) {
            return;
        }
    }
    if (s_a.held && s_a.held_src != src_family(src)) {
        return;                     // a second source while one is held
    }
    // (a second press of the same source means its release was lost: it counts as a new press)
    s_a.held = true;
    s_a.held_src = src_family(src);
    s_a.press_t = app_now_ms();
    s_a.press_side = side;
    s_a.ignore_up = false;
    s_a.pending = false;
    switch (s_a.st) {
    case APP_IDLE:
    case APP_ERROR:
        // 250 ms before the press; a chip reports its press only after the 400 ms hold: 500 ms
        start_recording(side, src == APP_SRC_CHIP ? 500 : 250);
        break;
    case APP_LISTENING:
        if (s_a.latched) {
            s_a.ignore_up = true;   // latched: the press sends (ui.md 5.1)
            send_recording("press");
        }
        break;
    case APP_UPLOADING:
    case APP_THINKING:
        if (src == APP_SRC_CHIP) {
            // the chip has already been held 400 ms: barge-in now
            devcon_event("barge", "\"turn\":%u", (unsigned)s_a.turn);
            cancel_turn();
            set_state(APP_IDLE);
            start_recording(side, 500);
            break;
        }
        s_a.pending = true;         // a tap is ignored (the orb pulses); a hold aborts and records
        s_a.pending_preroll = 500;  // from the press, 400 ms before the recording starts
        break;
    case APP_SPEAKING:
        stop_speaking();            // any press stops speech; still held at 400 ms = barge-in
        s_a.pending = true;
        // preroll_cap() keeps only what followed the flush: the user's first words, not the speech tail
        s_a.pending_preroll = 500;
        break;
    default:
        break;
    }
    publish();
}

void app_ptt_up(int32_t held_ms, app_src_t src)
{
    if (src == APP_SRC_BOOT && s_a.boot_ignored) {
        s_a.boot_ignored = false;
        return;
    }
    if (!s_a.held || s_a.held_src != src_family(src)) {
        return;                     // e.g. BOOT released while the orb is held
    }
    s_a.held = false;
    uint32_t held = held_ms >= 0 ? (uint32_t)held_ms : app_now_ms() - s_a.press_t;
    if (s_a.ignore_up || s_a.pending) {
        s_a.ignore_up = s_a.pending = false;
    } else if (s_a.st == APP_LISTENING && !s_a.latched) {
        if (held < APP_TAP_MS) {
            s_a.latched = true;     // tap: keep recording until the next press, speech + silence, or 30 s
            devcon_event("rec", "\"phase\":\"latched\",\"turn\":%u", (unsigned)s_a.turn);
            ui_state(UI_ST_REC_LATCHED);
        } else {
            send_recording("release");
        }
    }
    publish();
}

static void ptt_cancel(void)
{
    if (!s_a.held || s_a.held_src != APP_SRC_ORB) {
        return;
    }
    s_a.held = false;
    if (s_a.pending) {
        s_a.pending = false;
    } else if (s_a.st == APP_LISTENING) {
        discard_recording(true);
    }
    publish();
}

// ---- modes and switches

static void set_mode(int m, bool save_now)
{
    m = m ? 1 : 0;
    if (m != s_a.mode) {
        app_abort_all();
        s_a.mode = m;
        ai_client_history_clear();  // protocol 4.3: a mode switch starts a new conversation
        ui_i(UI_SET_MODE, m);
        devcon_event("mode", "\"mode\":\"%s\"", m ? "translator" : "assistant");
    }
    app_io_save("mode", m, save_now);
    publish();
}

static void set_text_only(bool v, bool save_now)
{
    if (v != s_a.text_only) {
        s_a.text_only = v;
        if (v && s_a.st == APP_SPEAKING) {
            stop_speaking();        // the voice button also interrupts speech (ui.md 5.2)
        } else if (v && s_a.want_tts) {
            tts_client_cancel(s_a.turn);
            audio_out_stop();
            reveal(false);
            finish_turn(UI_ST_ANSWERED);
        }
        devcon_event("text_only", "\"v\":%d", v);
    }
    ui_i(UI_SET_TEXT_ONLY, v);      // with an app the UI waits for this before switching its icon
    app_io_save("text_only", v, save_now);
    publish();
}

static void new_chat(void)
{
    app_abort_all();
    ai_client_history_clear();
    ui_i(UI_CLEAR, 0);
    ui_i(UI_GOTO, UI_SCR_HOME);
    s_a.chat_used[s_a.mode] = false;
}

void app_net_changed(bool up)
{
    // only a request in flight fails; a reply already received is shown (its speech fails or plays out)
    if (!up && (s_a.st == APP_UPLOADING || (s_a.st == APP_THINKING && !s_a.want_tts))) {
        cancel_turn();
        show_local_error("N0");
    }
}

void app_portal_changed(bool on)
{
    // the portal blocks AI turns (architecture 1.8): setup is a mode of its own
    if (on && s_a.st != APP_IDLE && s_a.st != APP_ERROR) {
        app_abort_all();
        show_not_ready(RDY_PORTAL);
    }
}

// ---- events

static void on_setting(const char *key)
{
    bool all = !strcmp(key, "*");
    if (all || !strcmp(key, "mode")) {
        int v = settings_get_int("mode") ? 1 : 0;
        if (v != s_a.mode) {
            set_mode(v, false);
        }
    }
    if (all || !strcmp(key, "text_only")) {
        bool v = settings_get_int("text_only") != 0;
        if (v != s_a.text_only) {
            set_text_only(v, false);
        }
    }
    if ((all || !strcmp(key, "ai_key")) && s_a.fault == UI_FAULT_BAD_KEY) {
        clear_fault();
    }
    if ((all || !strcmp(key, "ai_model")) && (s_a.fault == UI_FAULT_MODEL || s_a.fault == UI_FAULT_REGION)) {
        clear_fault();
    }
    app_io_setting(key);
}

static void on_ui(ui_input_t ev, int32_t arg)
{
    switch (ev) {
    case UI_IN_NEW_CHAT: new_chat(); break;
    case UI_IN_CARD_ACTION:
        if (arg == UI_CARD_SERVER || arg == UI_CARD_TLS) {
            retry_last();           // "Повторить" re-sends the kept clip or text
        } else {
            ui_toast_id(S_NOT_YET); // the fixing screens (W, Q, P, AI, D) come with the settings UI
        }
        break;
    case UI_IN_WRONG_LANG:
        ui_i(UI_GOTO, UI_SCR_HOME);
        if (s_a.mode == 1) {
            rerun_clip((int8_t)arg);
        }
        break;
    case UI_IN_REPLAY: replay(); break;
    case UI_IN_OPEN:
        if (arg == UI_OPEN_MODE_SHEET) {
            set_mode(!s_a.mode, false);     // no mode sheet (M) yet: the pill switches the mode
        } else {
            ui_toast_id(S_NOT_YET);
        }
        break;
    case UI_IN_DISMISS:
        if (s_a.st == APP_ERROR) {
            ui_state(UI_ST_IDLE);
            set_state(APP_IDLE);
        }
        break;
    case UI_IN_LANGS:
        // tr_lang_a / tr_lang_b were saved by the UI: the new pair applies to the next turn
        if (s_a.mode == 1) {
            app_abort_all();
        }
        publish();
        break;
    case UI_IN_SPEAKER:
        s_no_speaker = arg == 0;
        if (s_no_speaker && s_a.st == APP_SPEAKING) {
            stop_speaking();
        }
        publish();
        break;
    case UI_IN_SLEEP: app_io_ui_sleep((int)arg); break;
    default: break;
    }
}

static void handle(app_evt_t *e)
{
    bool mine = e->turn && e->turn == s_a.turn;
    switch (e->type) {
    case EV_PTT_DOWN: app_ptt_down((int8_t)e->i, (app_src_t)e->j); break;
    case EV_PTT_UP: app_ptt_up(e->i, (app_src_t)e->j); break;
    case EV_PTT_CANCEL: ptt_cancel(); break;
    case EV_CANCEL:
        app_io_activity();
        cancel_any(true);
        break;
    case EV_TEXT_SUBMIT:
        app_io_activity();
        if (e->p) {
            text_turn(e->p);        // takes the text
            e->p = NULL;
        }
        break;
    case EV_AI_UPLOAD_DONE:
        if (mine && s_a.st == APP_UPLOADING) {
            devcon_event("ai", "\"turn\":%u,\"phase\":\"upload_done\",\"ms\":%d", (unsigned)e->turn, (int)e->i);
            ui_i(UI_UPLOAD_PCT, 100);
            ui_state(UI_ST_THINK);
            set_state(APP_THINKING);
        }
        break;
    case EV_AI_RETRY:
        if (mine) {
            devcon_event("retry", "\"turn\":%u,\"n\":%d", (unsigned)e->turn, (int)e->i);
            ui_i(UI_RETRY, e->i);
        }
        break;
    case EV_AI_RESULT:
        if (mine && (s_a.st == APP_UPLOADING || s_a.st == APP_THINKING)) {
            on_result(e->p);
        }
        break;
    case EV_AI_ERROR:
        if (mine && (s_a.st == APP_UPLOADING || s_a.st == APP_THINKING)) {
            const or_err_t *er = e->p;
            devcon_event("ai", "\"turn\":%u,\"phase\":\"error\",\"id\":\"%s\"", (unsigned)e->turn, er->id);
            cancel_turn();
            show_error(er->id, er->msg_ru);
        }
        break;
    case EV_AI_RELEASED:
        // the clip's audio is no longer read (one recording buffer: nothing to hand back)
        devcon_event("ai", "\"turn\":%u,\"phase\":\"released\"", (unsigned)e->turn);
        break;
    case EV_TTS_FIRST_AUDIO:
        if (mine && s_a.want_tts && !s_a.tts_first) {
            s_a.t_ttfb_ms = (uint32_t)e->i;
            devcon_event("tts", "\"turn\":%u,\"phase\":\"first_audio\",\"ttfb\":%d", (unsigned)e->turn, (int)e->i);
            speaking_begin();
        }
        break;
    case EV_TTS_DONE:
        if (mine && s_a.want_tts) {
            s_a.tts_done = true;
            devcon_event("tts", "\"turn\":%u,\"phase\":\"done\"", (unsigned)e->turn);
            if (!s_a.tts_first && audio_out_busy()) {
                speaking_begin();   // its first-audio event was lost (queue full): the speaker plays
            } else if (!s_a.tts_first) {
                reveal(false);      // nothing was played
                finish_turn(UI_ST_ANSWERED);
            }
        }
        break;
    case EV_TTS_ERROR:
        if (mine && s_a.want_tts) {
            on_tts_error(e->p);
        }
        break;
    case EV_SET_MODE:
        app_io_activity();
        set_mode(e->i, e->j != 0);
        break;
    case EV_SET_TEXT_ONLY:
        app_io_activity();
        set_text_only(e->i != 0, e->j != 0);
        break;
    case EV_SET_DIR:
        s_a.tr_dir = e->i < 0 || e->i > 2 ? 0 : (int)e->i;
        ui_i(UI_SET_TR_DIR, s_a.tr_dir);
        publish();
        break;
    case EV_SETTING:
        if (e->p) {
            on_setting(e->p);
        }
        break;
    case EV_BUTTON: app_io_button((board_btn_t)e->i, (board_btn_ev_t)e->j, (uint32_t)e->k); break;
    case EV_UI:
        if (e->i != UI_IN_SLEEP) {
            app_io_activity();      // the UI's sleep report is a status, not input: it must never wake it
        }
        on_ui((ui_input_t)e->i, e->j);
        break;
    case EV_WAKE: app_io_activity(); break;
    case EV_SYNC: break;
    default: break;
    }
}

static void fsm_tick(uint32_t now)
{
    if (s_a.held && s_a.held_src == APP_SRC_BOOT && !board_button_down(BOARD_BTN_BOOT) &&
        now - s_a.press_t > 1000) {
        app_ptt_up(-1, APP_SRC_BOOT);   // the release event was lost (queue full): the key is up
    } else if (s_a.held && s_a.st != APP_LISTENING && now - s_a.press_t > 120000) {
        s_a.held = s_a.pending = s_a.ignore_up = false;    // a lost touch release must not block talking
    }
    if (s_a.pending && s_a.held && now - s_a.press_t >= APP_TAP_MS) {
        // held while busy: abort and record (barge-in); after speech this records with no pre-roll
        s_a.pending = false;
        if (s_a.st == APP_UPLOADING || s_a.st == APP_THINKING) {
            devcon_event("barge", "\"turn\":%u", (unsigned)s_a.turn);
            cancel_turn();
            set_state(APP_IDLE);
        } else if (s_a.st == APP_SPEAKING) {
            stop_speaking();        // the reply began to play while the press was pending: still a barge-in
        }
        if (s_a.st == APP_IDLE || s_a.st == APP_ERROR) {
            start_recording(s_a.press_side, s_a.pending_preroll);
        }
    }
    switch (s_a.st) {
    case APP_LISTENING:
        rec_tick(now);
        break;
    case APP_UPLOADING: {
        uint32_t el = now - s_a.t_submit;
        int pct = s_a.est_upload_ms ? (int)(el * 100 / s_a.est_upload_ms) : 0;
        pct = pct > 90 ? 90 : pct;      // 100 % comes with the upload-done event
        if (pct >= s_a.upload_pct + 5) {
            s_a.upload_pct = pct;
            ui_i(UI_UPLOAD_PCT, pct);
        }
        if (el > APP_TURN_MAX_MS) {
            turn_timeout();
        }
        break;
    }
    case APP_THINKING:
        if (!s_a.want_tts) {
            if (now - s_a.t_submit > APP_TURN_MAX_MS) {
                turn_timeout();
            }
        } else if (now - s_a.t_result >= APP_TTS_FIRST_MS) {
            or_err_t e;
            or_err_local(&e, "T1");
            snprintf(e.message, sizeof(e.message), "no audio in %u ms", (unsigned)APP_TTS_FIRST_MS);
            tts_client_cancel(s_a.turn);
            on_tts_error(&e);
        } else if (!s_a.revealed && now - s_a.t_result >= APP_REVEAL_MS) {
            reveal(false);
        }
        break;
    case APP_SPEAKING:
        if (s_a.tts_done && !audio_out_busy()) {
            devcon_event("audio_drained", "\"turn\":%u", (unsigned)s_a.turn);
            finish_turn(UI_ST_IDLE);
        } else if (now - s_a.t_first > APP_SPEAK_MAX_MS) {
            stop_speaking();
        }
        break;
    case APP_ERROR:
        if (now - s_a.st_t0 >= APP_ERROR_MS) {
            set_state(APP_IDLE);        // the UI left its ERROR look after 2.4 s by itself
        }
        break;
    default:
        break;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    uint32_t last_tick = 0;
    for (;;) {
        uint32_t tick = s_a.st == APP_LISTENING || s_a.pending ? TICK_FAST_MS : TICK_MS;
        app_evt_t e;
        if (xQueueReceive(s_q, &e, pdMS_TO_TICKS(tick)) == pdTRUE) {
            handle(&e);
            app_evt_free(&e);
            atomic_fetch_add(&s_handled, 1);
        }
        uint32_t now = app_now_ms();
        if (now - last_tick >= tick) {
            last_tick = now;
            fsm_tick(now);
            app_io_tick(now);
            publish();
        }
    }
}

// ---- posting (any task)

bool app_post(const app_evt_t *e, TickType_t wait)
{
    if (!e) {
        return false;
    }
    if (!s_q || xQueueSend(s_q, e, wait) != pdTRUE) {
        app_evt_t c = *e;
        app_evt_free(&c);
        return false;
    }
    atomic_fetch_add(&s_posted, 1);
    return true;
}

bool app_wait_handled(uint32_t timeout_ms)
{
    unsigned target = atomic_load(&s_posted);
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 10) {
        if ((int)(atomic_load(&s_handled) - target) >= 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

// UI input (taskLVGL): post only.
static void ui_input_cb(ui_input_t ev, int32_t i, void *ctx)
{
    (void)ctx;
    app_evt_t e = { 0 };
    if (ev == UI_IN_SLEEP) {
        // the UI owns sleep (ui_sleep.c): its level report only updates the app's mirror, never wakes
        e.type = EV_UI;
        e.i = ev;
        e.j = i;
        app_post(&e, 0);
        return;
    }
    if (app_io_screen() != APP_SCREEN_ON) {
        // ui.md 2.3: the first touch on a dimmed or dark screen only wakes it
        e.type = EV_WAKE;
        app_post(&e, 0);
        if (ev == UI_IN_PTT_DOWN) {
            lv_indev_t *in = lv_indev_active();
            if (in) {
                lv_indev_wait_release(in);
            }
            return;
        }
    }
    switch (ev) {
    case UI_IN_PTT_DOWN:
        e.type = EV_PTT_DOWN;
        e.i = i;
        e.j = i == UI_SIDE_AUTO ? APP_SRC_ORB : APP_SRC_CHIP;
        break;
    case UI_IN_PTT_UP:
        e.type = EV_PTT_UP;
        e.i = i;
        e.j = APP_SRC_ORB;
        break;
    case UI_IN_PTT_CANCEL: e.type = EV_PTT_CANCEL; break;
    case UI_IN_TEXT_ONLY:
        e.type = EV_SET_TEXT_ONLY;
        e.i = i;
        break;
    case UI_IN_SET_MODE:            // the mode sheet already switched the UI and saves "mode"
        e.type = EV_SET_MODE;
        e.i = i;
        break;
    case UI_IN_TR_DIR:
        e.type = EV_SET_DIR;
        e.i = i;
        break;
    case UI_IN_TEXT: {              // typed question: the app owns the copy (freed by app_post on overflow)
        size_t n = (size_t)(i > 0 ? i : 0) + 1;
        char *t = heap_caps_malloc(n < 2048 ? 2048 : n + 1, MALLOC_CAP_SPIRAM);
        if (!t) {
            return;
        }
        ui_typed_text(t, n < 2048 ? 2048 : n + 1);
        e.type = EV_TEXT_SUBMIT;
        e.p = t;
        break;
    }
    default:
        e.type = EV_UI;
        e.i = ev;
        e.j = i;
        break;
    }
    app_post(&e, 0);
}

static app_result_t *result_copy(const ai_result_t *r)
{
    app_result_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM);
    if (!c) {
        return NULL;
    }
    c->transcript = ps_strdup(r->transcript ? r->transcript : "");
    c->text = ps_strdup(r->text ? r->text : "");
    if (!c->transcript || !c->text) {
        free(c->transcript);
        free(c->text);
        free(c);
        return NULL;
    }
    strlcpy(c->lang, r->lang, sizeof(c->lang));
    strlcpy(c->src_lang, r->src_lang, sizeof(c->src_lang));
    strlcpy(c->tts_lang, r->tts_lang, sizeof(c->tts_lang));
    strlcpy(c->model, r->model, sizeof(c->model));
    c->unheard = r->unheard;
    c->truncated = r->truncated;
    c->other_lang = r->other_lang;
    c->stt_used = r->stt_used;
    c->t = r->t;
    c->cost = r->cost;
    return c;
}

static or_err_t *err_copy(const or_err_t *e)
{
    or_err_t *c = heap_caps_malloc(sizeof(*c), MALLOC_CAP_SPIRAM);
    if (c && e) {
        *c = *e;
    } else if (c) {
        or_err_local(c, "M0");
    }
    return c;
}

// AI / TTS events (ai and tts tasks): copy what the app needs and post, never block for long.
static void ai_listener(const ai_event_t *ev, void *ctx)
{
    (void)ctx;
    // console turns ("ai ask") and turns the app already left are dropped; "released" still reaches the
    // console log for a cancelled turn (architecture test 4.6)
    if (!ev->turn || (ev->turn != atomic_load(&s_turn_pub) && ev->type != AI_EV_RELEASED)) {
        return;
    }
    app_evt_t e = { .turn = ev->turn, .i = ev->i };
    switch (ev->type) {
    case AI_EV_UPLOAD_DONE: e.type = EV_AI_UPLOAD_DONE; break;
    case AI_EV_RETRY: e.type = EV_AI_RETRY; break;
    case AI_EV_RESULT:
        e.type = EV_AI_RESULT;
        e.p = result_copy(ev->result);
        if (!e.p) {
            e.type = EV_AI_ERROR;
            e.p = err_copy(NULL);
        }
        break;
    case AI_EV_ERROR:
        e.type = EV_AI_ERROR;
        e.p = err_copy(ev->err);
        break;
    case AI_EV_RELEASED: e.type = EV_AI_RELEASED; break;
    case AI_EV_TTS_FIRST_AUDIO: e.type = EV_TTS_FIRST_AUDIO; break;
    case AI_EV_TTS_DONE: e.type = EV_TTS_DONE; break;
    case AI_EV_TTS_ERROR:
        e.type = EV_TTS_ERROR;
        e.p = err_copy(ev->err);
        break;
    default: return;
    }
    if ((e.type == EV_AI_RESULT || e.type == EV_AI_ERROR || e.type == EV_TTS_ERROR) && !e.p) {
        return;                     // out of memory: the turn deadline ends it
    }
    app_post(&e, pdMS_TO_TICKS(100));
}

esp_err_t app_start(void)
{
    ESP_RETURN_ON_FALSE(!s_task, ESP_ERR_INVALID_STATE, TAG, "already started");
    memset(&s_a, 0, sizeof(s_a));
    s_a.mode = settings_get_int("mode") ? 1 : 0;
    s_a.text_only = settings_get_int("text_only") != 0;
    s_a.hints = 3;
    s_a.last_card = -1;
    s_a.fault = UI_FAULT_NONE;
    s_a.press_side = UI_SIDE_AUTO;
    vad_cfg_t c;
    vad_default_cfg(&c);
    app_vad_cfg(NULL, &c);
    s_q = xQueueCreate(APP_Q_DEPTH, sizeof(app_evt_t));
    ESP_RETURN_ON_FALSE(s_q, ESP_ERR_NO_MEM, TAG, "queue");
    publish();
    app_io_init();                  // buttons and the settings callback post into the queue
    ESP_RETURN_ON_ERROR(ai_add_listener(ai_listener, NULL), TAG, "ai listener");
    ui_set_input_cb(ui_input_cb, NULL);
    BaseType_t ok = xTaskCreatePinnedToCore(app_task, "app", APP_STACK, NULL, APP_PRIO, &s_task, APP_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "started (mode %s, text_only %d)", s_a.mode ? "translator" : "assistant", s_a.text_only);
    return ESP_OK;
}
