// App state machine tests: app.c and app_io.c compiled into this TU (to reach their statics) on top of the
// recording mocks. Time, audio, AI and TTS events are driven by the test, as the tasks would.
#include "app.c"
#define TAG TAG_io
#include "app_io.c"
#undef TAG
#include "mocks.h"

#include <math.h>

static int s_checks, s_fail;
#define CHECK(c)                                                              \
    do {                                                                      \
        s_checks++;                                                           \
        if (!(c)) {                                                           \
            s_fail++;                                                         \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)

// ---- synthetic audio (16 kHz): PC fan at -45 dBFS, speech bursts at -22 dBFS

static uint32_t s_rng = 777;
static size_t s_t;                  // running sample clock for continuous phase

static double urand(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((s_rng >> 8) & 0xFFFFFF) / (double)0x800000 - 1.0;
}

static double noise(double db)
{
    return (urand() + urand() + urand() + urand()) / 1.1547 * 32768.0 * pow(10.0, db / 20.0);
}

static int16_t sample(int kind)
{
    size_t ms = s_t * 1000 / 16000;
    double v = noise(-45);
    if (kind == 1) {
        bool on = ms % 220 < 160;
        double env = on ? sin(3.14159265 * (double)(ms % 220) / 160.0) : 0.0;
        v += on ? env * 1.4 * noise(-22) : noise(-42);
    }
    s_t++;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// ---- driving the app

static void pump(void)
{
    app_evt_t e;
    while (xQueueReceive(s_q, &e, 0) == pdTRUE) {
        handle(&e);
        app_evt_free(&e);
    }
}

static void tick20(void)
{
    g_now_us += 20000;
    pump();
    uint32_t now = app_now_ms();
    fsm_tick(now);
    app_io_tick(now);
    publish();
    pump();
}

static void step(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 20) {
        tick20();
    }
}

// kind 0 = fan only, 1 = speech; samples go into the recording while it runs, at real time
static void audio(uint32_t ms, int kind)
{
    for (uint32_t t = 0; t < ms; t += 20) {
        for (int i = 0; i < 320; i++) {
            int16_t v = sample(kind);
            if (g_recording && g_rec_len < REC_MAX) {
                g_rec[g_rec_len++] = v;
            }
        }
        tick20();
    }
}

static void post_ev(app_evt_type_t t, int32_t i, int32_t j)
{
    app_evt_t e = { .type = t, .i = i, .j = j };
    app_post(&e, 0);
    pump();
}

static void ui_in(ui_input_t ev, int32_t i)
{
    ui_input_cb(ev, i, NULL);       // the real UI callback
    pump();
}

static void btn(board_btn_t b, board_btn_ev_t ev, uint32_t held)
{
    btn_cb(b, ev, held, NULL);      // the real button callback
    pump();
}

static void boot(void)
{
    mocks_reset();
    s_task = NULL;
    atomic_store(&s_screen, APP_SCREEN_ON);
    atomic_store(&s_bat_mv, -1);
    s_last_act = 0;
    s_pwr_woke = s_latch_off = s_off_wait_up = false;
    s_off_check = s_note_until = 0;
    s_net = -1;
    s_portal = false;
    s_t_net = s_t_bat = s_t_heap = s_t_touch = s_t_amb = 0;
    s_bat_polled = s_bat_warned = s_low_mem = false;
    s_ambient = VAD_NO_HINT;
    for (size_t i = 0; i < sizeof(s_save) / sizeof(s_save[0]); i++) {
        s_save[i].on = false;
    }
    CHECK(app_start() == ESP_OK);
    step(200);                      // first net poll, ambient level
    g_nui = 0;
    g_nev = 0;
}

static bool last_state_is(ui_state_t st)
{
    const ui_rec_t *r = ui_last(UI_SET_STATE);
    return r && r->i == (int32_t)st;
}

// hold the orb, speak ms, release
static void say(uint32_t ms)
{
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    audio(200, 0);
    audio(ms, 1);
    audio(200, 0);
    post_ev(EV_PTT_UP, (int32_t)(ms + 400), APP_SRC_ORB);
}

// ---- tests

static void t_hold_text_only(void)
{
    printf("t_hold_text_only\n");
    boot();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    CHECK(s_a.st == APP_LISTENING && g_recording && g_preroll == 250 && g_prewarm == 1);
    CHECK(g_max_ms == 30000 && last_state_is(UI_ST_REC));
    audio(200, 0);
    audio(1200, 1);
    audio(200, 0);
    post_ev(EV_PTT_UP, 1600, APP_SRC_ORB);
    CHECK(s_a.st == APP_UPLOADING && g_nsub == 1 && !g_recording);
    CHECK(g_sub[0].pcm == g_rec && g_sub[0].samples == g_rec_len && g_sub[0].mode == AI_MODE_ASSISTANT);
    CHECK(strstr(ev_last("rec"), "\"verdict\":\"ok\"") && strstr(ev_last("rec"), "\"why\":\"release\""));
    int um = ui_index(UI_USER_MSG, 0);
    CHECK(um >= 0 && !g_ui[um].has_s && ui_index(UI_USER_WAVE, um) > um);
    CHECK(last_state_is(UI_ST_UPLOAD));
    uint32_t turn = s_a.turn;
    emit_simple(AI_EV_UPLOAD_DONE, turn, 300);
    pump();
    CHECK(s_a.st == APP_THINKING && last_state_is(UI_ST_THINK) && ui_last(UI_UPLOAD_PCT)->i == 100);
    emit_result(turn, AI_MODE_ASSISTANT, "Привет", "Здравствуйте!", "ru", "", "ru", false);
    pump();
    const ui_rec_t *u = ui_last(UI_USER_MSG), *b = ui_last(UI_BOT_MSG);
    CHECK(u && !strcmp(u->s, "Привет") && b && !strcmp(b->s, "Здравствуйте!") && b->i == 0);
    CHECK(s_a.st == APP_IDLE && last_state_is(UI_ST_ANSWERED) && g_nspeak == 0);    // text_only = 1 by default
    CHECK(ui_last(UI_SET_HINTS) && ui_last(UI_SET_HINTS)->i == 2);
    // late events of the finished turn are dropped
    int n = g_nui;
    emit_simple(AI_EV_TTS_FIRST_AUDIO, turn, 500);
    pump();
    CHECK(g_nui == n);
}

static void t_tap_latched(void)
{
    printf("t_tap_latched\n");
    boot();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    audio(100, 0);
    post_ev(EV_PTT_UP, 100, APP_SRC_ORB);
    CHECK(s_a.st == APP_LISTENING && s_a.latched && last_state_is(UI_ST_REC_LATCHED));
    audio(1000, 1);
    CHECK(s_a.st == APP_LISTENING);
    // the next press sends on press-down; its release does nothing
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    CHECK(s_a.st == APP_UPLOADING && g_nsub == 1);
    post_ev(EV_PTT_UP, 80, APP_SRC_ORB);
    CHECK(s_a.st == APP_UPLOADING && !s_a.held);

    // latched: speech then 1.2 s of silence sends by itself
    boot();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    post_ev(EV_PTT_UP, 90, APP_SRC_BOOT);
    audio(300, 0);
    audio(1100, 1);
    audio(1000, 0);
    CHECK(s_a.st == APP_LISTENING);
    audio(400, 0);
    CHECK(s_a.st == APP_UPLOADING && g_nsub == 1 && strstr(ev_last("rec"), "\"why\":\"vad\""));

    // latched with fan noise only: "Не расслышал" after 5 s, no request
    boot();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    post_ev(EV_PTT_UP, 90, APP_SRC_ORB);
    audio(4800, 0);
    CHECK(s_a.st == APP_LISTENING);
    audio(400, 0);
    CHECK(s_a.st == APP_IDLE && g_nsub == 0 && ui_last(UI_STATUS)->i == S_ST_NOT_HEARD);
    CHECK(ui_last(UI_SET_MOOD) && ui_last(UI_SET_MOOD)->i == UI_MOOD_SAD);
}

static void t_gate(void)
{
    printf("t_gate\n");
    // the bench finding: a fan-noise clip must never reach the AI
    boot();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    audio(2500, 0);
    post_ev(EV_PTT_UP, 2500, APP_SRC_ORB);
    CHECK(g_nsub == 0 && s_a.st == APP_IDLE && last_state_is(UI_ST_IDLE));
    CHECK(ui_last(UI_STATUS) && ui_last(UI_STATUS)->i == S_ST_NOT_HEARD && !ui_last(UI_STATUS)->has_s);
    CHECK(ui_count(UI_USER_MSG) == 0 && strstr(ev_last("rec"), "no_speech"));
    // a single short syllable: too short
    boot();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    audio(300, 0);
    s_t = 0;
    audio(160, 1);
    audio(300, 0);
    post_ev(EV_PTT_UP, 800, APP_SRC_ORB);
    CHECK(g_nsub == 0 && s_a.st == APP_IDLE);
    CHECK(ui_last(UI_STATUS) && (ui_last(UI_STATUS)->i == S_ST_TOO_SHORT || ui_last(UI_STATUS)->i == S_ST_NOT_HEARD));
    // dead microphone: zeros from the mic source -> mic card and fault
    boot();
    g_level = 0;
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    step(1000);
    g_rec_len = 16000;
    memset(g_rec, 0, 16000 * sizeof(int16_t));
    post_ev(EV_PTT_UP, 1000, APP_SRC_ORB);
    CHECK(g_nsub == 0 && s_a.st == APP_ERROR && ui_last(UI_ERROR) && ui_last(UI_ERROR)->i == UI_CARD_MIC);
    CHECK(ui_last(UI_SET_FAULT) && ui_last(UI_SET_FAULT)->i == UI_FAULT_MIC);
    // the same zeros from injected audio are just "not heard"
    boot();
    g_src = 1;
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_CONSOLE);
    CHECK(g_preroll == 250);        // audio_in itself skips the pre-roll for injected audio
    g_rec_len = 16000;
    memset(g_rec, 0, 16000 * sizeof(int16_t));
    post_ev(EV_PTT_UP, 1000, APP_SRC_CONSOLE);
    CHECK(g_nsub == 0 && s_a.st == APP_IDLE && ui_count(UI_ERROR) == 0);
}

static void t_inject_end(void)
{
    printf("t_inject_end\n");
    // "mic src inject fast" + "ptt tap": the clip is in at once, its end sends the latched recording
    boot();
    g_src = 1;
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_CONSOLE);
    for (int i = 0; i < 16000 * 2; i++) {
        g_rec[g_rec_len++] = sample(i > 4000 && i < 28000 ? 1 : 0);
    }
    g_recording = false;            // audio_in stops at the end of the injected clip
    step(20);
    CHECK(s_a.st == APP_UPLOADING && g_nsub == 1 && strstr(ev_last("rec"), "\"why\":\"limit\""));
    post_ev(EV_PTT_UP, 80, APP_SRC_CONSOLE);
    CHECK(s_a.st == APP_UPLOADING);
}

static void t_voice(void)
{
    printf("t_voice\n");
    boot();
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1200);
    uint32_t turn = s_a.turn;
    emit_simple(AI_EV_UPLOAD_DONE, turn, 300);
    emit_result(turn, AI_MODE_ASSISTANT, "Как дела?", "Хорошо.", "ru", "", "ru", false);
    pump();
    CHECK(g_nspeak == 1 && g_speak[0].turn == turn && !strcmp(g_speak[0].text, "Хорошо.") &&
          !strcmp(g_speak[0].lang, "ru"));
    CHECK(s_a.st == APP_THINKING && ui_count(UI_BOT_MSG) == 0);    // the reply waits for its audio
    emit_simple(AI_EV_TTS_FIRST_AUDIO, turn, 700);
    pump();
    CHECK(s_a.st == APP_SPEAKING && last_state_is(UI_ST_SPEAK) && ui_count(UI_BOT_MSG) == 1);
    g_out_busy = true;
    emit_simple(AI_EV_TTS_DONE, turn, 1);
    step(200);
    CHECK(s_a.st == APP_SPEAKING);
    g_out_busy = false;
    step(40);
    CHECK(s_a.st == APP_IDLE && last_state_is(UI_ST_IDLE) && ev_count("audio_drained") == 1);

    // no first audio: the text appears after 2.5 s, "voice unavailable" after 15 s
    boot();
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1000);
    turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест", "Ответ.", "ru", "", "ru", false);
    pump();
    step(2400);
    CHECK(ui_count(UI_BOT_MSG) == 0);
    step(200);
    CHECK(ui_count(UI_BOT_MSG) == 1 && s_a.st == APP_THINKING);
    step(12600);
    CHECK(s_a.st == APP_IDLE && ui_last(UI_TOAST) && g_tts_cancel_upto >= turn);

    // TTS error before any audio: the text with the note
    boot();
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1000);
    turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест", "Ответ.", "ru", "", "ru", false);
    emit_error(AI_EV_TTS_ERROR, turn, "T1", "Озвучка недоступна");
    pump();
    CHECK(s_a.st == APP_IDLE && ui_last(UI_BOT_MSG) && ui_last(UI_BOT_MSG)->i == 1);

    // PWR while the reply waits for its audio: the text is shown, nothing is spoken, no "Отменено"
    boot();
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1000);
    turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест", "Ответ.", "ru", "", "ru", false);
    pump();
    int st0 = ui_count(UI_STATUS);
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_UP, 120);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 120);
    CHECK(s_a.st == APP_IDLE && g_tts_cancel_upto >= turn && ui_count(UI_BOT_MSG) == 1);
    CHECK(!strcmp(ui_last(UI_BOT_MSG)->s, "Ответ.") && ui_count(UI_STATUS) == st0);

    // a hold barge-in in that phase keeps the reply on screen too
    say(1000);
    turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест 2", "Ответ 2.", "ru", "", "ru", false);
    pump();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    step(420);
    CHECK(s_a.st == APP_LISTENING && s_a.turn != turn && !strcmp(ui_last(UI_BOT_MSG)->s, "Ответ 2."));
    CHECK(g_preroll == 500);            // nothing was played: the full pre-roll
    post_ev(EV_CANCEL, 0, 0);
    post_ev(EV_PTT_UP, 600, APP_SRC_ORB);

    // Wi-Fi drops after the reply arrived: no "Нет Wi-Fi" error over an answer in hand
    say(1000);
    turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест 3", "Ответ 3.", "ru", "", "ru", false);
    pump();
    int cards = ui_count(UI_ERROR);
    g_net_state = NET_CONNECTING;
    step(600);
    CHECK(s_a.st == APP_THINKING && ui_count(UI_ERROR) == cards);
    emit_error(AI_EV_TTS_ERROR, turn, "T1", "Озвучка недоступна");
    pump();
    CHECK(s_a.st == APP_IDLE && !strcmp(ui_last(UI_BOT_MSG)->s, "Ответ 3.") && ui_last(UI_BOT_MSG)->i == 1);
    g_net_state = NET_UP;
    step(600);

    // the first-audio event was lost (queue full): "done" with the speaker playing still means SPEAKING
    say(1000);
    turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест 4", "Ответ 4.", "ru", "", "ru", false);
    pump();
    g_out_busy = true;
    emit_simple(AI_EV_TTS_DONE, turn, 1);
    pump();
    CHECK(s_a.st == APP_SPEAKING && !strcmp(ui_last(UI_BOT_MSG)->s, "Ответ 4.") && last_state_is(UI_ST_SPEAK));
    g_out_busy = false;
    step(40);
    CHECK(s_a.st == APP_IDLE);

    // volume 0 behaves like text_only
    boot();
    set_int("text_only", 0);
    set_int("volume", 0);
    s_a.text_only = false;
    say(1000);
    emit_result(s_a.turn, AI_MODE_ASSISTANT, "Тест", "Ответ.", "ru", "", "ru", false);
    pump();
    CHECK(g_nspeak == 0 && s_a.st == APP_IDLE && ui_count(UI_BOT_MSG) == 1);
}

static void t_barge(void)
{
    printf("t_barge\n");
    boot();
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1000);
    uint32_t turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест", "Длинный ответ.", "ru", "", "ru", false);
    emit_simple(AI_EV_TTS_FIRST_AUDIO, turn, 600);
    pump();
    g_out_busy = true;
    CHECK(s_a.st == APP_SPEAKING);
    int starts = g_rec_starts;
    // a tap stops the speech only
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    CHECK(s_a.st == APP_IDLE && g_tts_cancel_upto >= turn && g_out_stops > 0 && last_state_is(UI_ST_IDLE));
    step(200);
    post_ev(EV_PTT_UP, 200, APP_SRC_ORB);
    step(400);
    CHECK(s_a.st == APP_IDLE && g_rec_starts == starts);

    // a hold stops it and records after 400 ms; the pre-roll holds what followed the flush (the user's
    // first words), never the tail of the speech
    boot();
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1000);
    turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест", "Ответ.", "ru", "", "ru", false);
    emit_simple(AI_EV_TTS_FIRST_AUDIO, turn, 600);
    pump();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    step(300);
    CHECK(s_a.st == APP_IDLE);
    step(140);
    CHECK(s_a.st == APP_LISTENING && s_a.turn != turn);
    CHECK(g_preroll >= 400 - APP_SPK_TAIL_MS && g_preroll <= 440 - APP_SPK_TAIL_MS);
    audio(1000, 1);
    audio(200, 0);
    post_ev(EV_PTT_UP, 1700, APP_SRC_BOOT);
    CHECK(s_a.st == APP_UPLOADING);

    // right after the speech drained, a new press keeps no speech tail in its pre-roll
    boot();
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1000);
    turn = s_a.turn;
    emit_result(turn, AI_MODE_ASSISTANT, "Тест", "Ответ.", "ru", "", "ru", false);
    emit_simple(AI_EV_TTS_FIRST_AUDIO, turn, 600);
    emit_simple(AI_EV_TTS_DONE, turn, 1);
    pump();
    step(100);
    CHECK(s_a.st == APP_IDLE);
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    CHECK(s_a.st == APP_LISTENING && g_preroll == 0);
    post_ev(EV_CANCEL, 0, 0);
    post_ev(EV_PTT_UP, 100, APP_SRC_ORB);
    step(400);                          // 250 ms + the tail after the speech: the normal pre-roll again
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    CHECK(s_a.st == APP_LISTENING && g_preroll == 250);
    post_ev(EV_CANCEL, 0, 0);
    post_ev(EV_PTT_UP, 100, APP_SRC_ORB);

    // pressed while uploading, the answer starts to play before 400 ms: the hold still barges in
    boot();
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1000);
    turn = s_a.turn;
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    step(100);
    emit_result(turn, AI_MODE_ASSISTANT, "Тест", "Ответ.", "ru", "", "ru", false);
    emit_simple(AI_EV_TTS_FIRST_AUDIO, turn, 600);
    pump();
    g_out_busy = true;
    CHECK(s_a.st == APP_SPEAKING && ui_count(UI_BOT_MSG) == 1);
    step(320);
    CHECK(s_a.st == APP_LISTENING && s_a.turn != turn && g_tts_cancel_upto >= turn && g_preroll <= 20);
    g_out_busy = false;
    post_ev(EV_CANCEL, 0, 0);
    post_ev(EV_PTT_UP, 800, APP_SRC_ORB);

    // pressed while thinking: a tap is ignored, a hold aborts and records (500 ms pre-roll)
    boot();
    say(1000);
    turn = s_a.turn;
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    step(100);
    post_ev(EV_PTT_UP, 100, APP_SRC_ORB);
    step(500);
    CHECK(s_a.st == APP_UPLOADING && s_a.turn == turn && g_cancel_upto < turn);
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    step(420);
    CHECK(s_a.st == APP_LISTENING && g_cancel_upto >= turn && g_preroll == 500);
    // the waveform bubble of the aborted turn was closed
    const ui_rec_t *u = ui_last(UI_USER_MSG);
    CHECK(u && u->has_s && !strcmp(u->s, TXT_ELLIPSIS));
    // results of the old turn are dropped
    emit_result(turn, AI_MODE_ASSISTANT, "old", "old", "ru", "", "ru", false);
    pump();
    CHECK(s_a.st == APP_LISTENING && strcmp(ui_last(UI_USER_MSG)->s, "old"));
}

static void t_cancel(void)
{
    printf("t_cancel\n");
    boot();
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    audio(500, 1);
    post_ev(EV_CANCEL, 0, 0);
    CHECK(s_a.st == APP_IDLE && !g_recording && ui_last(UI_STATUS)->i == S_ST_CANCELLED);
    post_ev(EV_PTT_UP, 900, APP_SRC_ORB);
    CHECK(s_a.st == APP_IDLE && g_nsub == 0);
    // slide off the orb and release: discard
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    audio(500, 1);
    ui_in(UI_IN_PTT_CANCEL, 0);
    CHECK(s_a.st == APP_IDLE && g_nsub == 0);
    // PWR while uploading
    say(1000);
    uint32_t turn = s_a.turn;
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_UP, 120);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 120);
    CHECK(s_a.st == APP_IDLE && g_cancel_upto >= turn && ui_last(UI_STATUS)->i == S_ST_CANCELLED);
    CHECK(g_pwr_down == 1 && g_pwr_up == 1);
    // the cancelled turn still reports that its audio is released (architecture test 4.6), nothing else
    int nui = g_nui;
    emit_simple(AI_EV_RELEASED, turn, 0);
    emit_error(AI_EV_ERROR, turn, "N2", "late");
    pump();
    CHECK(strstr(ev_last("ai"), "\"phase\":\"released\"") && g_nui == nui && s_a.st == APP_IDLE);
    // PWR at H idle: screen off; BOOT wakes and talks at once
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 100);
    CHECK(app_io_screen() == APP_SCREEN_OFF && g_backlight == 0);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(app_io_screen() == APP_SCREEN_ON && g_backlight == 80 && s_a.st == APP_LISTENING);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 60);
    post_ev(EV_CANCEL, 0, 0);
    // a touch on the dark screen only wakes it
    app_io_screen_off();
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_AUTO);
    CHECK(app_io_screen() == APP_SCREEN_ON && s_a.st == APP_IDLE && g_wait_release == 1);
    // PWR on a dark screen: the click only wakes
    app_io_screen_off();
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 100);
    CHECK(app_io_screen() == APP_SCREEN_ON);
    // PWR off the Home screen goes back
    g_ui_ctx = UI_CTX_READ;
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 100);
    CHECK(g_pwr_short == 1 && app_io_screen() == APP_SCREEN_ON);
}

static void t_errors(void)
{
    printf("t_errors\n");
    boot();
    say(1000);
    uint32_t turn = s_a.turn;
    emit_error(AI_EV_ERROR, turn, "401", "Неверный API-ключ.");
    pump();
    const ui_rec_t *e = ui_last(UI_ERROR);
    CHECK(s_a.st == APP_ERROR && e && e->i == UI_CARD_BAD_KEY && !strcmp(e->s, "Неверный API-ключ."));
    CHECK(ui_last(UI_SET_FAULT)->i == UI_FAULT_BAD_KEY && !strcmp(s_a.last_err, "401"));
    CHECK(ui_last(UI_USER_MSG)->has_s);         // the waveform bubble is closed
    step(4100);
    CHECK(s_a.st == APP_IDLE);
    // the next good turn clears the fault
    say(1000);
    emit_result(s_a.turn, AI_MODE_ASSISTANT, "a", "b", "ru", "", "ru", false);
    pump();
    CHECK(ui_last(UI_SET_FAULT)->i == UI_FAULT_NONE);
    // server errors: card with a retry button; "Повторить" re-sends the kept clip
    say(1000);
    turn = s_a.turn;
    emit_error(AI_EV_ERROR, turn, "N6", "Нет ответа от ИИ.");
    pump();
    CHECK(ui_last(UI_ERROR)->i == UI_CARD_SERVER);
    int n = g_nsub;
    ui_in(UI_IN_CARD_ACTION, UI_CARD_SERVER);
    CHECK(g_nsub == n + 1 && g_sub[n].pcm == g_rec && s_a.st == APP_UPLOADING && s_a.turn != turn);
    // unheard result (P4): no bot bubble, "Не расслышал"
    emit_result(s_a.turn, AI_MODE_ASSISTANT, "", "", "ru", "", "", true);
    pump();
    CHECK(s_a.st == APP_IDLE && ui_last(UI_STATUS)->i == S_ST_NOT_HEARD && !strcmp(ui_last(UI_USER_MSG)->s, TXT_ELLIPSIS));
    // not ready: no Wi-Fi card, a second press within 20 s only re-shows the state
    g_net_state = NET_FAILED;
    step(600);
    int cards = ui_count(UI_ERROR), starts = g_rec_starts;
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    post_ev(EV_PTT_UP, 100, APP_SRC_BOOT);
    CHECK(ui_count(UI_ERROR) == cards + 1 && ui_last(UI_ERROR)->i == UI_CARD_NO_WIFI && g_rec_starts == starts);
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    post_ev(EV_PTT_UP, 100, APP_SRC_BOOT);
    CHECK(ui_count(UI_ERROR) == cards + 1 && last_state_is(UI_ST_ERROR));
    // no key
    g_net_state = NET_UP;
    step(600);
    set_str("ai_key", "");
    step(25000);
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    CHECK(ui_last(UI_ERROR)->i == UI_CARD_GENERIC && ui_last(UI_SET_FAULT)->i == UI_FAULT_BAD_KEY);
    post_ev(EV_PTT_UP, 100, APP_SRC_BOOT);
    // Wi-Fi drops while thinking
    boot();
    say(1000);
    emit_simple(AI_EV_UPLOAD_DONE, s_a.turn, 300);
    pump();
    g_net_state = NET_CONNECTING;
    step(600);
    CHECK(s_a.st == APP_ERROR && ui_last(UI_ERROR)->i == UI_CARD_NO_WIFI);
    // no result at all: N8 after 75 s
    g_net_state = NET_UP;
    step(4600);
    say(1000);
    step(76000);
    CHECK(s_a.st == APP_ERROR && !strcmp(s_a.last_err, "N8"));
}

static void t_translator(void)
{
    printf("t_translator\n");
    boot();
    ui_in(UI_IN_OPEN, UI_OPEN_MODE_SHEET);      // the pill switches the mode (no mode sheet yet)
    CHECK(s_a.mode == 1 && ui_last(UI_SET_MODE)->i == 1 && g_hist_clear == 1);
    CHECK(settings_get_int("mode") == 0);       // written 2 s later, once idle
    step(2100);
    CHECK(settings_get_int("mode") == 1);
    // chip A held: the UI reports it after 400 ms, so 500 ms of pre-roll, and the source is forced
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_A);
    CHECK(s_a.st == APP_LISTENING && g_preroll == 500 && s_a.rec_side == UI_SIDE_A);
    audio(1000, 1);
    audio(200, 0);
    ui_in(UI_IN_PTT_UP, 1600);
    CHECK(s_a.st == APP_UPLOADING && g_nsub == 1 && g_sub[0].mode == AI_MODE_TRANSLATOR && !strcmp(g_sub[0].src, "ru"));
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Доброе утро", "Good morning", "en", "ru", "en", false);
    pump();
    const ui_rec_t *u = ui_last(UI_USER_MSG);
    CHECK(u && u->i == UI_SIDE_A && !strcmp(u->s, "Доброе утро") && !strcmp(ui_last(UI_BOT_MSG)->s, "Good morning"));
    CHECK(ui_last(UI_STATUS)->has_s && !strcmp(ui_last(UI_STATUS)->s, "RU " TXT_ARROW " EN"));
    // the orb detects the language; with tr_vad = 1 a pause while holding sends it
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_AUTO);
    CHECK(s_a.rec_side == UI_SIDE_AUTO && g_preroll == 250);
    audio(300, 0);
    audio(1000, 1);
    audio(1400, 0);
    CHECK(s_a.st == APP_UPLOADING && g_sub[g_nsub - 1].src[0] == 0);
    ui_in(UI_IN_PTT_UP, 2700);
    CHECK(s_a.st == APP_UPLOADING);
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Where is the station?", "Где вокзал?", "ru", "en", "ru", false);
    pump();
    CHECK(ui_last(UI_USER_MSG)->i == UI_SIDE_B);
    // "Не тот язык": the kept clip again with the other source
    const int16_t *pcm = g_sub[g_nsub - 1].pcm;
    size_t ns = g_sub[g_nsub - 1].samples;
    ui_in(UI_IN_WRONG_LANG, UI_SIDE_A);
    CHECK(ui_last(UI_GOTO)->i == UI_SCR_HOME && g_sub[g_nsub - 1].pcm == pcm && g_sub[g_nsub - 1].samples == ns);
    CHECK(!strcmp(g_sub[g_nsub - 1].src, "ru") && s_a.st == APP_UPLOADING);
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Where", "Где", "en", "ru", "en", false);
    pump();
    // replay of the last translation (voice off is no obstacle: it is an explicit tap)
    ui_in(UI_IN_REPLAY, 0);
    CHECK(g_nspeak == 1 && !strcmp(g_speak[0].text, "Где") && !strcmp(g_speak[0].lang, "en"));
    emit_simple(AI_EV_TTS_FIRST_AUDIO, s_a.turn, 500);
    emit_simple(AI_EV_TTS_DONE, s_a.turn, 1);
    pump();
    step(40);
    CHECK(s_a.st == APP_IDLE);
    // other language with tr_other unset (0): translated into A and shown
    say(1000);
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Guten Morgen", "Доброе утро", "ru", "other", "ru", false);
    pump();
    CHECK(!strcmp(ui_last(UI_BOT_MSG)->s, "Доброе утро") && ui_last(UI_USER_MSG)->i == UI_SIDE_B);
    // a recording overwrites the kept clip: no re-run after it
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    post_ev(EV_CANCEL, 0, 0);
    post_ev(EV_PTT_UP, 100, APP_SRC_ORB);
    int n = g_nsub;
    ui_in(UI_IN_WRONG_LANG, UI_SIDE_A);
    CHECK(g_nsub == n && ui_last(UI_TOAST)->i == S_NOT_YET);
    // back to the assistant from the console: saved at once
    post_ev(EV_SET_MODE, 0, 1);
    CHECK(s_a.mode == 0 && settings_get_int("mode") == 0);
}

static void t_text(void)
{
    printf("t_text\n");
    boot();
    char *t = malloc(32);
    strcpy(t, "Сколько времени?");
    app_evt_t e = { .type = EV_TEXT_SUBMIT, .p = t };
    app_post(&e, 0);
    pump();
    CHECK(s_a.st == APP_THINKING && g_nsub == 1 && g_sub[0].has_text && !strcmp(g_sub[0].text, "Сколько времени?"));
    CHECK(!strcmp(ui_last(UI_USER_MSG)->s, "Сколько времени?") && last_state_is(UI_ST_THINK));
    emit_result(s_a.turn, AI_MODE_ASSISTANT, "Сколько времени?", "Полдень.", "ru", "", "ru", false);
    pump();
    CHECK(ui_count(UI_USER_MSG) == 1 && !strcmp(ui_last(UI_BOT_MSG)->s, "Полдень.") && s_a.st == APP_IDLE);
    // text-only switched on while speaking stops the speech
    set_int("text_only", 0);
    s_a.text_only = false;
    say(1000);
    emit_result(s_a.turn, AI_MODE_ASSISTANT, "a", "b", "ru", "", "ru", false);
    emit_simple(AI_EV_TTS_FIRST_AUDIO, s_a.turn, 500);
    pump();
    CHECK(s_a.st == APP_SPEAKING);
    ui_in(UI_IN_TEXT_ONLY, 1);
    CHECK(s_a.st == APP_IDLE && s_a.text_only && ui_last(UI_SET_TEXT_ONLY)->i == 1);
    step(2100);
    CHECK(settings_get_int("text_only") == 1);
    // a setting written elsewhere (console "set text_only 0") reaches the app
    settings_set_int("text_only", 0);
    pump();
    CHECK(!s_a.text_only && ui_last(UI_SET_TEXT_ONLY)->i == 0);
    settings_set_int("volume", 35);
    pump();
    CHECK(g_volume_set == 35);
}

static void t_power(void)
{
    printf("t_power\n");
    boot();
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_LONG, 3000);
    CHECK(g_power_off == 1 && ev_count("power_off") == 1 && app_io_screen() == APP_SCREEN_OFF);
    step(600);                          // still running: USB power
    CHECK(app_io_screen() == APP_SCREEN_ON && ui_last(UI_TOAST)->i == S_USB_POWER);
    step(2100);
    CHECK(app_io_screen() == APP_SCREEN_OFF);
    btn(BOARD_BTN_PWR, BTN_EV_UP, 3200);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(g_latch_on == 1 && app_io_screen() == APP_SCREEN_ON && s_a.st == APP_LISTENING);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 3000);
    // on battery the held key powers the board until it is released: no "USB" note while it is held
    boot();
    g_pwr_held = true;
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_LONG, 3000);
    step(1500);
    CHECK(g_power_off == 1 && app_io_screen() == APP_SCREEN_OFF && ui_count(UI_TOAST) == 0);
    g_pwr_held = false;                 // released and still alive: USB
    btn(BOARD_BTN_PWR, BTN_EV_UP, 4500);
    step(400);
    CHECK(app_io_screen() == APP_SCREEN_OFF);
    step(200);
    CHECK(app_io_screen() == APP_SCREEN_ON && ui_last(UI_TOAST)->i == S_USB_POWER);
    step(2100);
    CHECK(app_io_screen() == APP_SCREEN_OFF);
    // dim after dim_s, off 300 s later; any input wakes
    boot();
    step(59000);
    CHECK(app_io_screen() == APP_SCREEN_ON);
    step(1100);
    CHECK(app_io_screen() == APP_SCREEN_DIM && g_backlight == 25);
    step(300000);
    CHECK(app_io_screen() == APP_SCREEN_OFF && g_backlight == 0);
    g_inactive_ms = 50;                 // a touch
    step(220);
    CHECK(app_io_screen() == APP_SCREEN_ON && g_backlight == 80);
    g_inactive_ms = 100000;
    // a turn keeps the screen awake
    say(1000);
    step(70000);
    CHECK(app_io_screen() == APP_SCREEN_ON);
    // battery: toast at 10 %
    boot();
    g_bat_mv = 3360;
    step(31000);
    CHECK(ui_last(UI_TOAST) && ui_last(UI_TOAST)->has_s && strstr(ui_last(UI_TOAST)->s, "Батарея"));
    // BOOT ignored where the UI says so (keyboard): its release too
    boot();
    g_boot_ok = false;
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 900);
    CHECK(s_a.st == APP_IDLE && g_rec_starts == 0);
    // a lost BOOT release: the key reads up, the recording is sent
    g_boot_ok = true;
    g_boot_down = true;
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    audio(800, 1);
    g_boot_down = false;
    audio(400, 0);
    CHECK(s_a.st == APP_UPLOADING);
}

static void t_misc(void)
{
    printf("t_misc\n");
    // after hist_idle_min (10) minutes the next turn starts a new conversation with a divider
    boot();
    say(1000);
    emit_result(s_a.turn, AI_MODE_ASSISTANT, "a", "b", "ru", "", "ru", false);
    pump();
    int hc = g_hist_clear;
    step(9 * 60000);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);       // wakes the dark screen and talks
    CHECK(ui_count(UI_DIVIDER) == 0 && s_a.st == APP_LISTENING);
    post_ev(EV_CANCEL, 0, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 100);
    step(10 * 60000 + 100);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(ui_count(UI_DIVIDER) == 1 && g_hist_clear == hc + 1);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 100);
    post_ev(EV_CANCEL, 0, 0);

    // the portal blocks turns and ends a running one
    boot();
    say(1000);
    g_portal = true;
    step(600);
    CHECK(s_a.st == APP_ERROR && ui_last(UI_PORTAL)->i == 1 && g_cancel_upto > 0);
    CHECK(ui_last(UI_ERROR)->has_s && strstr(ui_last(UI_ERROR)->s, "Закройте"));
    g_portal = false;
    step(4600);

    // a chip held while thinking: already 400 ms, barge-in at once
    post_ev(EV_SET_MODE, 1, 1);
    say(1000);
    uint32_t turn = s_a.turn;
    emit_simple(AI_EV_UPLOAD_DONE, turn, 200);
    pump();
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_B);
    CHECK(s_a.st == APP_LISTENING && g_cancel_upto >= turn && s_a.rec_side == UI_SIDE_B && g_preroll == 500);
    ui_in(UI_IN_PTT_UP, 500);
    // persistent direction B -> A forces B for the orb
    post_ev(EV_CANCEL, 0, 0);
    post_ev(EV_SET_DIR, 2, 0);
    CHECK(ui_last(UI_SET_TR_DIR)->i == 2);
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_AUTO);
    CHECK(s_a.rec_side == UI_SIDE_B);
    ui_in(UI_IN_PTT_UP, 100);
    post_ev(EV_CANCEL, 0, 0);
    post_ev(EV_SET_DIR, 0, 0);

    // a lost touch release: the next press of the orb counts as a new press
    boot();
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_AUTO);
    audio(600, 1);
    post_ev(EV_CANCEL, 0, 0);       // the release never arrives
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_AUTO);
    CHECK(s_a.st == APP_LISTENING);
    // and BOOT cannot release the orb's press
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 2000);
    CHECK(s_a.st == APP_LISTENING && s_a.held);
    ui_in(UI_IN_PTT_UP, 100);
    CHECK(s_a.latched);
    post_ev(EV_CANCEL, 0, 0);

    // settings that the device side applies
    set_str("ui_lang", "en");
    settings_set_int("bright", 50);
    pump();
    CHECK(g_backlight == 50);
    set_str("tr_lang_b", "de");
    on_setting_cb("tr_lang_b", NULL);
    pump();
    CHECK(ui_last(UI_SET_LANGS) && !strcmp(ui_last(UI_SET_LANGS)->s, "ru|de"));
    on_setting_cb("ui_lang", NULL);
    pump();
    CHECK(ui_last(UI_SET_LANG)->i == 1);
    // settings reset ("*"): mode and text-only follow the stored values
    s_a.mode = 1;
    set_int("mode", 0);
    on_setting_cb("*", NULL);
    pump();
    CHECK(s_a.mode == 0 && ui_last(UI_SET_MODE)->i == 0);
    set_str("ui_lang", "ru");

    // no memory: no new turn
    boot();
    s_low_mem = true;
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    CHECK(s_a.st == APP_ERROR && g_rec_starts == 0 && !strcmp(s_a.last_err, "M0"));
    post_ev(EV_PTT_UP, 100, APP_SRC_BOOT);
    s_low_mem = false;
}

int main(void)
{
    t_hold_text_only();
    t_tap_latched();
    t_gate();
    t_inject_end();
    t_voice();
    t_barge();
    t_cancel();
    t_errors();
    t_translator();
    t_text();
    t_power();
    t_misc();
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
