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

// UI_IN_CARD_ACTION argument of a card stamped with turn tag
static int32_t card_arg(int card, uint32_t tag)
{
    return (int32_t)((tag << 8) | (uint32_t)card);
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

static void t_double(void)
{
    printf("t_double\n");
    // a BOOT tap records from the press and latches once the 300 ms double-tap window has passed
    boot();
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(s_a.st == APP_LISTENING && g_rec_starts == 1);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 90);
    CHECK(s_a.st == APP_LISTENING && !s_a.latched && s_a.latch_wait);
    step(APP_DOUBLE_MS + 40);
    CHECK(s_a.latched && !s_a.latch_wait && last_state_is(UI_ST_REC_LATCHED));
    post_ev(EV_CANCEL, 0, 0);
    step(400);
    // double tap: the first tap's recording goes without a word, the mode switches with a toast
    int st0 = ui_count(UI_STATUS);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 80);
    step(120);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(s_a.mode == 1 && s_a.st == APP_IDLE && !g_recording && ui_last(UI_SET_MODE)->i == 1 && g_nsub == 0);
    CHECK(ui_last(UI_TOAST) && ui_last(UI_TOAST)->i == S_MODE_TRANSLATOR && ui_count(UI_STATUS) == st0);
    CHECK(strstr(ev_last("boot_double"), "translator") != NULL);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 80);
    step(400);
    CHECK(s_a.st == APP_IDLE && g_rec_starts == 2 && !s_a.latched);
    // and back
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 80);
    step(60);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 80);
    step(400);
    CHECK(s_a.mode == 0 && ui_last(UI_TOAST)->i == S_MODE_ASSISTANT);
    // a second press after the window is the latched recording's "send"
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 80);
    audio(300, 0);
    audio(900, 1);
    CHECK(s_a.latched && s_a.st == APP_LISTENING);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(s_a.st == APP_UPLOADING && s_a.mode == 0 && g_nsub == 1);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 80);
    // the orb inside the window: the BOOT tap was a single one, so the orb press sends it
    boot();
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 80);
    audio(100, 1);
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_AUTO);
    CHECK(s_a.st != APP_LISTENING && s_a.mode == 0 && strstr(ev_last("rec"), "\"why\":\"press\""));
    ui_in(UI_IN_PTT_UP, 80);
    // a hold is no tap: release sends, and a quick press after it is a new recording, not a double
    boot();
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    audio(300, 0);
    audio(900, 1);
    audio(200, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 1400);
    CHECK(s_a.st == APP_UPLOADING && s_a.boot_tap_t == 0);
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

    // PWR (lock) while the reply waits for its audio: the text is shown, nothing is spoken, no "Отменено"
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
    CHECK(g_locked && s_a.st == APP_IDLE && g_tts_cancel_upto >= turn && ui_count(UI_BOT_MSG) == 1);
    CHECK(!strcmp(ui_last(UI_BOT_MSG)->s, "Ответ.") && ui_count(UI_STATUS) == st0);
    g_locked = false;

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
    // PWR while uploading: it locks, and the lock ends the turn without a word
    say(1000);
    uint32_t turn = s_a.turn;
    int st0 = ui_count(UI_STATUS);
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_UP, 120);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 120);
    CHECK(g_locked && g_pwr_short == 1 && s_a.st == APP_IDLE && g_cancel_upto >= turn && ui_count(UI_STATUS) == st0);
    CHECK(g_pwr_down == 1 && g_pwr_up == 1 && strstr(ev_last("lock"), "\"on\":1"));
    // the cancelled turn still reports that its audio is released (architecture test 4.6), nothing else
    int nui = g_nui;
    emit_simple(AI_EV_RELEASED, turn, 0);
    emit_error(AI_EV_ERROR, turn, "N2", "late");
    pump();
    CHECK(strstr(ev_last("ai"), "\"phase\":\"released\"") && g_nui == nui && s_a.st == APP_IDLE);
    // locked: BOOT talks to no one (its release neither), a second PWR click unlocks, then BOOT talks again
    int starts = g_rec_starts;
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 900);
    CHECK(g_boot_refused == 1 && g_rec_starts == starts && s_a.st == APP_IDLE);
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 100);
    CHECK(!g_locked && g_pwr_short == 2);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(s_a.st == APP_LISTENING);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 900);
    post_ev(EV_CANCEL, 0, 0);
    // a recording is discarded by the lock too; nothing of the press carries over
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    audio(400, 1);
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 100);
    CHECK(g_locked && s_a.st == APP_IDLE && !g_recording && !s_a.held && g_nsub == 1);
    post_ev(EV_PTT_UP, 900, APP_SRC_ORB);
    CHECK(s_a.st == APP_IDLE && g_nsub == 1);
    // a chip held from before the lock reports its 400 ms hold while locked: no recording
    starts = g_rec_starts;
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_A);
    CHECK(g_rec_starts == starts && s_a.st == APP_IDLE && !s_a.held);
    ui_in(UI_IN_PTT_UP, 600);
    ui_keylock(false);
    pump();
    // the UI's own screen off (power off on USB, console): its SLEEP(3) report does not light it again (flow-1);
    // BOOT wakes and talks at once
    app_io_screen_off();
    int off_at = ui_index(UI_SCREEN_OFF, 0);
    step(1000);
    CHECK(app_io_screen() == APP_SCREEN_OFF && g_ui_level == 3 && off_at >= 0 && ui_index(UI_WAKE, off_at) < 0);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(app_io_screen() == APP_SCREEN_ON && g_ui_level == 0 && s_a.st == APP_LISTENING);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 900);
    post_ev(EV_CANCEL, 0, 0);
    // a touch on the dark screen only wakes it
    app_io_screen_off();
    ui_in(UI_IN_PTT_DOWN, UI_SIDE_AUTO);
    CHECK(app_io_screen() == APP_SCREEN_ON && s_a.st == APP_IDLE && g_wait_release == 1);
    // PWR on a dark screen: the click only wakes (no lock)
    app_io_screen_off();
    int pwr = g_pwr_short;
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 100);
    CHECK(app_io_screen() == APP_SCREEN_ON && g_pwr_short == pwr && !g_locked);
    // ... but locked, the press lights the lock screen and its click unlocks: one press
    ui_keylock(true);
    pump();
    app_io_screen_off();
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    CHECK(app_io_screen() == APP_SCREEN_ON);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 100);
    CHECK(!g_locked && g_pwr_short == pwr + 1);
    // a dimmed screen is still seen: PWR locks it (on any screen; PWR is no longer "back")
    ui_sleep_to(1);
    pump();
    g_ui_ctx = UI_CTX_READ;
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_CLICK, 100);
    CHECK(g_locked && g_pwr_short == pwr + 2 && app_io_screen() == APP_SCREEN_ON);
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
    ui_in(UI_IN_CARD_ACTION, card_arg(UI_CARD_SERVER, turn));
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
    // a BOOT tap may be the first of a double tap: "not ready" comes once the window has passed
    CHECK(ui_count(UI_ERROR) == cards);
    step(APP_DOUBLE_MS + 40);
    CHECK(ui_count(UI_ERROR) == cards + 1 && ui_last(UI_ERROR)->i == UI_CARD_NO_WIFI && g_rec_starts == starts);
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    post_ev(EV_PTT_UP, 100, APP_SRC_BOOT);
    step(APP_DOUBLE_MS + 40);
    CHECK(ui_count(UI_ERROR) == cards + 1 && last_state_is(UI_ST_ERROR));
    // a double tap while not ready only switches the mode: no error at all
    step(4100);
    int errs = ui_count(UI_STATUS) + ui_count(UI_ERROR);
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    post_ev(EV_PTT_UP, 90, APP_SRC_BOOT);
    step(100);
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    post_ev(EV_PTT_UP, 90, APP_SRC_BOOT);
    step(400);
    CHECK(s_a.mode == 1 && ui_count(UI_STATUS) + ui_count(UI_ERROR) == errs && g_rec_starts == starts);
    post_ev(EV_SET_MODE, 0, 1);
    // no key (ux-10): its own card (with "Ввести ключ") and "Нет ключа" in the pill, not "Ключ не принят"
    g_net_state = NET_UP;
    step(600);
    set_str("ai_key", "");
    step(25000);
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_BOOT);
    step(APP_TAP_MS + 20);              // held: no double tap any more, the error while it is still down
    CHECK(ui_last(UI_ERROR)->i == UI_CARD_NO_KEY && !ui_last(UI_ERROR)->has_s);
    CHECK(ui_last(UI_SET_FAULT)->i == UI_FAULT_NO_KEY);
    post_ev(EV_PTT_UP, 100, APP_SRC_BOOT);
    set_str("ai_key", "sk-or-v1-test");
    on_setting_cb("ai_key", NULL);
    pump();
    CHECK(ui_last(UI_SET_FAULT)->i == UI_FAULT_NONE);
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

static void t_cards(void)
{
    printf("t_cards\n");
    // flow-7: two different errors of one card type within 20 s each get their own card and reason
    boot();
    say(1000);
    emit_error(AI_EV_ERROR, s_a.turn, "413", "Запись слишком длинная. Говорите короче.");
    pump();
    say(1000);
    emit_error(AI_EV_ERROR, s_a.turn, "P5", "ИИ отказался отвечать на этот запрос.");
    pump();
    CHECK(ui_count(UI_ERROR) == 2 && !strcmp(ui_last(UI_ERROR)->s, "ИИ отказался отвечать на этот запрос."));
    // the same error again soon: the orb and status only ...
    say(1000);
    emit_error(AI_EV_ERROR, s_a.turn, "P5", "ИИ отказался отвечать на этот запрос.");
    pump();
    CHECK(ui_count(UI_ERROR) == 2 && s_a.st == APP_ERROR && last_state_is(UI_ST_ERROR));
    // ... but in an emptied chat ("Новый разговор" goes to H) it gets its card again
    ui_in(UI_IN_NEW_CHAT, 0);
    CHECK(ui_last(UI_GOTO)->i == UI_SCR_HOME && ui_count(UI_CLEAR) == 1);
    say(1000);
    emit_error(AI_EV_ERROR, s_a.turn, "P5", "ИИ отказался отвечать на этот запрос.");
    pump();
    CHECK(ui_count(UI_ERROR) == 3);
    // ux-12: history deleted in S -> История: the app does not take the user to H
    int gotos = ui_count(UI_GOTO), hc = g_hist_clear;
    ui_in(UI_IN_NEW_CHAT, 1);
    CHECK(ui_count(UI_GOTO) == gotos && ui_count(UI_CLEAR) == 1 && g_hist_clear == hc + 1 && !s_a.chat_used[0]);
    // a retry card re-sends its own turn; an older one whose clip is gone is refused, nothing else is sent
    say(1000);
    uint32_t failed = s_a.turn;
    emit_error(AI_EV_ERROR, failed, "N6", "Нет ответа от ИИ. Попробуйте ещё раз.");
    pump();
    int n = g_nsub;
    ui_in(UI_IN_CARD_ACTION, card_arg(UI_CARD_SERVER, failed));
    CHECK(g_nsub == n + 1 && g_sub[n].pcm == g_rec && strstr(ev_last("entry"), "\"result\":\"rerun\""));
    post_ev(EV_CANCEL, 0, 0);
    ui_in(UI_IN_CARD_ACTION, card_arg(UI_CARD_SERVER, 99999));
    CHECK(g_nsub == n + 1 && ui_last(UI_TOAST)->i == S_NOT_YET);
    // ux-09: with the English UI the card body is English
    g_ui_lang = 1;
    step(4100);
    say(1000);
    emit_error(AI_EV_ERROR, s_a.turn, "N2", "Сервер недоступен. Попробуйте позже.");
    pump();
    CHECK(!strcmp(ui_last(UI_ERROR)->s, "Server unavailable. Try again later."));
    g_ui_lang = 0;
    // flow-8: D -> "Показать подсказки" resets the app's counter, which then counts down again
    boot();
    for (int i = 0; i < 3; i++) {
        say(1000);
        emit_result(s_a.turn, AI_MODE_ASSISTANT, "a", "b", "ru", "", "ru", false);
        pump();
    }
    CHECK(s_a.hints == 0 && ui_last(UI_SET_HINTS)->i == 0);
    ui_in(UI_IN_HINTS, 3);
    CHECK(s_a.hints == 3 && ui_last(UI_SET_HINTS)->i == 3);
    say(1000);
    emit_result(s_a.turn, AI_MODE_ASSISTANT, "a", "b", "ru", "", "ru", false);
    pump();
    CHECK(ui_last(UI_SET_HINTS)->i == 2);
    // ux-14: voice switched on with volume 0 says so
    ui_in(UI_IN_TEXT_ONLY, 0);
    set_int("volume", 0);
    ui_in(UI_IN_TEXT_ONLY, 1);
    ui_in(UI_IN_TEXT_ONLY, 0);
    CHECK(ui_last(UI_TOAST)->has_s && !strcmp(ui_last(UI_TOAST)->s, "Громкость 0"));
    set_int("volume", 60);
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
    // its chat entries carry the turn as their tag (the placeholder bubble is posted after the tag)
    uint32_t t1 = s_a.turn;
    size_t ns1 = g_sub[0].samples;
    CHECK(ui_last(UI_SET_TURN)->i == (int32_t)t1 && ui_index(UI_SET_TURN, 0) < ui_index(UI_USER_MSG, 0));
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
    uint32_t t2 = s_a.turn;
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Where is the station?", "Где вокзал?", "ru", "en", "ru", false);
    pump();
    CHECK(ui_last(UI_USER_MSG)->i == UI_SIDE_B);
    // "Не тот язык" on turn 2: its clip (still in the recording buffer) again with the other source
    const int16_t *pcm = g_sub[g_nsub - 1].pcm;
    size_t ns = g_sub[g_nsub - 1].samples;
    ui_in(UI_IN_WRONG_LANG, (int32_t)t2);
    CHECK(ui_last(UI_GOTO)->i == UI_SCR_HOME && g_sub[g_nsub - 1].pcm == pcm && g_sub[g_nsub - 1].samples == ns);
    CHECK(!strcmp(g_sub[g_nsub - 1].src, "ru") && s_a.st == APP_UPLOADING);
    CHECK(strstr(ev_last("entry"), "\"action\":\"wrong_lang\"") && strstr(ev_last("entry"), "\"result\":\"rerun\""));
    uint32_t t3 = s_a.turn;
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Where", "Где", "en", "ru", "en", false);
    pump();
    // replay of that translation (voice off is no obstacle: it is an explicit tap)
    ui_in(UI_IN_REPLAY, (int32_t)t3);
    CHECK(g_nspeak == 1 && !strcmp(g_speak[0].text, "Где") && !strcmp(g_speak[0].lang, "en"));
    emit_simple(AI_EV_TTS_FIRST_AUDIO, s_a.turn, 500);
    emit_simple(AI_EV_TTS_DONE, s_a.turn, 1);
    pump();
    step(40);
    CHECK(s_a.st == APP_IDLE);
    // flow-6: replay in the reader on turn 1 speaks turn 1's translation, not the newest one
    ui_in(UI_IN_REPLAY, (int32_t)t1);
    CHECK(g_nspeak == 2 && !strcmp(g_speak[1].text, "Good morning") && !strcmp(g_speak[1].lang, "en"));
    emit_simple(AI_EV_TTS_FIRST_AUDIO, s_a.turn, 500);
    emit_simple(AI_EV_TTS_DONE, s_a.turn, 1);
    pump();
    step(40);
    // flow-5: "Не тот язык" on turn 1 re-sends turn 1's clip (copied out when turn 2 recorded), forced to B
    int n = g_nsub;
    ui_in(UI_IN_WRONG_LANG, (int32_t)t1);
    CHECK(g_nsub == n + 1 && g_sub[n].pcm != g_rec && g_sub[n].samples == ns1 && !strcmp(g_sub[n].src, "en"));
    const kept_t *k5 = kept_find(s_a.turn);
    CHECK(s_a.st == APP_UPLOADING && k5 && k5->pcm == g_sub[n].pcm && k5->lent == s_a.turn && !kept_find(t1)->pcm);
    emit_simple(AI_EV_RELEASED, s_a.turn, 0);
    pump();
    CHECK(k5->lent == 0);
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Good morning", "Доброе утро", "ru", "en", "ru", false);
    pump();
    // other language with tr_other unset (0): translated into A and shown
    say(1000);
    uint32_t t6 = s_a.turn;
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Guten Morgen", "Доброе утро", "ru", "other", "ru", false);
    pump();
    CHECK(!strcmp(ui_last(UI_BOT_MSG)->s, "Доброе утро") && ui_last(UI_USER_MSG)->i == UI_SIDE_B);
    // ux-06: a recording that is cancelled (or not heard) no longer loses that clip
    post_ev(EV_PTT_DOWN, UI_SIDE_AUTO, APP_SRC_ORB);
    post_ev(EV_CANCEL, 0, 0);
    post_ev(EV_PTT_UP, 100, APP_SRC_ORB);
    n = g_nsub;
    ui_in(UI_IN_WRONG_LANG, (int32_t)t6);
    CHECK(g_nsub == n + 1 && g_sub[n].pcm != g_rec && !strcmp(g_sub[n].src, "ru"));
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Guten Morgen", "Доброе утро", "ru", "ru", "ru", false);
    pump();
    // a typed turn has no clip: "Не тот язык" on it re-sends nothing (no older clip instead)
    char *t = malloc(32);
    strcpy(t, "Привет");
    app_evt_t te = { .type = EV_TEXT_SUBMIT, .p = t };
    app_post(&te, 0);
    pump();
    uint32_t tt = s_a.turn;
    emit_result(s_a.turn, AI_MODE_TRANSLATOR, "Привет", "Hello", "en", "ru", "en", false);
    pump();
    n = g_nsub;
    ui_in(UI_IN_WRONG_LANG, (int32_t)tt);
    CHECK(g_nsub == n && ui_last(UI_TOAST)->i == S_NOT_YET && strstr(ev_last("entry"), "typed"));
    // an entry the app no longer keeps: nothing
    ui_in(UI_IN_WRONG_LANG, 0);
    ui_in(UI_IN_REPLAY, 0);
    CHECK(g_nsub == n && g_nspeak == 2);
    // ux-14: replay with volume 0 says so, not "Нет динамика"
    set_int("volume", 0);
    ui_in(UI_IN_REPLAY, (int32_t)tt);
    CHECK(g_nspeak == 2 && ui_last(UI_TOAST)->has_s && !strcmp(ui_last(UI_TOAST)->s, "Громкость 0"));
    set_int("volume", 60);
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
    // flow-2: the UI is told (screen off, level 3); it owns the backlight again
    CHECK(g_power_off == 1 && ev_count("power_off") == 1 && app_io_screen() == APP_SCREEN_OFF && g_ui_level == 3);
    g_inactive_ms = 50;                 // concurrency-3: LVGL activity the UI made itself is no input
    step(600);                          // still running: USB power
    // the note is visible (backlight up) while the UI stays at screen off: the latch stays released
    CHECK(app_io_screen() == APP_SCREEN_OFF && ui_last(UI_TOAST)->i == S_USB_POWER && g_backlight == 80);
    CHECK(g_latch_on == 0 && g_ui_level == 3);
    step(2100);
    CHECK(app_io_screen() == APP_SCREEN_OFF && g_backlight == 0 && g_latch_on == 0);
    g_inactive_ms = 100000;
    btn(BOARD_BTN_PWR, BTN_EV_UP, 3200);
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(g_latch_on == 1 && app_io_screen() == APP_SCREEN_ON && g_ui_level == 0 && s_a.st == APP_LISTENING);
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 3000);
    // a touch on the dark screen after a power off on USB (the UI's wake catcher) uses the device again
    boot();
    btn(BOARD_BTN_PWR, BTN_EV_DOWN, 0);
    btn(BOARD_BTN_PWR, BTN_EV_LONG, 3000);
    btn(BOARD_BTN_PWR, BTN_EV_UP, 3100);
    step(3000);
    CHECK(g_latch_on == 0 && app_io_screen() == APP_SCREEN_OFF);
    ui_sleep_to(0);
    pump();
    CHECK(g_latch_on == 1 && app_io_screen() == APP_SCREEN_ON && strstr(ev_last("power_latch"), "\"on\":1"));
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
    CHECK(app_io_screen() == APP_SCREEN_OFF && g_backlight == 0);
    step(200);
    CHECK(ui_last(UI_TOAST)->i == S_USB_POWER && g_backlight == 80 && g_ui_level == 3);
    step(2100);
    CHECK(app_io_screen() == APP_SCREEN_OFF && g_backlight == 0 && g_latch_on == 0);
    // flow-1 / concurrency-2: the UI dims by itself; its SLEEP report is no activity and never wakes it
    boot();
    int wakes = ui_count(UI_WAKE);
    ui_sleep_to(1);
    step(3000);
    CHECK(app_io_screen() == APP_SCREEN_DIM && g_ui_level == 1 && ui_count(UI_WAKE) == wakes);
    ui_sleep_to(2);
    ui_sleep_to(3);
    step(3000);
    CHECK(app_io_screen() == APP_SCREEN_OFF && g_ui_level == 3 && ui_count(UI_WAKE) == wakes);
    // a finger on the panel is input (the UI's catcher has woken it already)
    g_touch_down = true;
    step(220);
    g_touch_down = false;
    CHECK(app_io_screen() == APP_SCREEN_ON && g_ui_level == 0);
    // "set dim_s 0" on a dimmed screen lights it
    ui_sleep_to(1);
    pump();
    CHECK(app_io_screen() == APP_SCREEN_DIM);
    settings_set_int("dim_s", 0);
    pump();
    CHECK(app_io_screen() == APP_SCREEN_ON && g_ui_level == 0);
    settings_set_int("dim_s", 60);
    pump();
    // battery (3.16): the state goes to the UI when it changes; toasts at 15 % and 5 %, once each per discharge
    boot();
    g_bat_mv = 3480;                    // the mock curve: 20 %
    step(BAT_POLL_MS + 100);
    CHECK(ui_last(UI_SET_BATTERY) && UI_BAT_PCT(ui_last(UI_SET_BATTERY)->i) == 20 && ui_count(UI_TOAST) == 0);
    g_bat_mv = 3430;                    // 14 %
    step(BAT_POLL_MS + 100);
    CHECK(ui_count(UI_TOAST) == 1 && ui_last(UI_TOAST)->has_s && strstr(ui_last(UI_TOAST)->s, "Батарея 14 %"));
    g_bat_mv = 3420;                    // 13 %: no second toast
    step(BAT_POLL_MS + 100);
    CHECK(ui_count(UI_TOAST) == 1 && UI_BAT_PCT(ui_last(UI_SET_BATTERY)->i) == 13);
    g_bat_mv = 3340;                    // 4 %
    step(BAT_POLL_MS + 100);
    CHECK(ui_count(UI_TOAST) == 2 && strstr(ui_last(UI_TOAST)->s, "Батарея 4 %"));
    int nbat = ui_count(UI_SET_BATTERY);
    step(BAT_POLL_MS + 100);            // nothing changed: nothing posted
    CHECK(ui_count(UI_SET_BATTERY) == nbat && ui_count(UI_TOAST) == 2);
    g_bat_chg = true;                   // charging starts a new cycle
    step(BAT_POLL_MS + 100);
    CHECK((ui_last(UI_SET_BATTERY)->i & UI_BAT_CHARGING) && ui_count(UI_TOAST) == 2);
    g_bat_chg = false;
    step(BAT_POLL_MS + 100);
    CHECK(ui_count(UI_TOAST) == 3 && !(ui_last(UI_SET_BATTERY)->i & UI_BAT_CHARGING));
    // the console's "bat sim" hands the state over at once
    g_bat_mv = 4200;
    post_ev(EV_BATTERY, 0, 0);
    CHECK(UI_BAT_PCT(ui_last(UI_SET_BATTERY)->i) == 100);
    g_bat_mv = -1;
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
    // flow-9: a press that was cancelled (or not heard) is no exchange: the idle time still counts from the
    // answer, as the AI client's history does; the divider comes with the request, before it is sent
    step(60000 + 100);
    say(1000);
    CHECK(ui_count(UI_DIVIDER) == 1 && g_hist_clear == hc + 1 && s_a.st == APP_UPLOADING);
    CHECK(ui_index(UI_DIVIDER, 0) < ui_index(UI_USER_MSG, ui_index(UI_DIVIDER, 0)));
    emit_result(s_a.turn, AI_MODE_ASSISTANT, "c", "d", "ru", "", "ru", false);
    pump();
    // right after an answer: no divider
    say(1000);
    CHECK(ui_count(UI_DIVIDER) == 1);
    post_ev(EV_CANCEL, 0, 0);

    // flow-4: BOOT on the phone-setup screen goes home and records once the portal has stopped
    boot();
    g_portal = true;
    step(600);
    g_ui_ctx = UI_CTX_QR;
    g_boot_down = true;
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    CHECK(s_a.st == APP_IDLE && s_a.portal_wait && ui_count(UI_ERROR) == 0 && g_rec_starts == 0);
    g_ui_ctx = UI_CTX_HOME;             // the UI went home; Q's delete stops the portal on its worker
    step(300);
    CHECK(s_a.st == APP_IDLE && ui_count(UI_ERROR) == 0);
    g_portal = false;
    step(40);
    CHECK(s_a.st == APP_LISTENING && !s_a.latched && g_preroll == 500 && ui_count(UI_ERROR) == 0);
    audio(1000, 1);
    audio(200, 0);
    g_boot_down = false;
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 1600);
    CHECK(s_a.st == APP_UPLOADING && strstr(ev_last("rec"), "\"why\":\"release\""));
    post_ev(EV_CANCEL, 0, 0);
    // a tap there: released before the portal is down, so a latched recording
    g_portal = true;
    step(600);
    g_ui_ctx = UI_CTX_QR;
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    g_ui_ctx = UI_CTX_HOME;
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 90);
    step(100);
    g_portal = false;
    step(40);
    CHECK(s_a.st == APP_LISTENING && s_a.latched && last_state_is(UI_ST_REC_LATCHED));
    post_ev(EV_CANCEL, 0, 0);
    // the portal never stops: the error after 5 s
    g_portal = true;
    step(600);
    g_ui_ctx = UI_CTX_QR;
    g_boot_down = true;
    btn(BOARD_BTN_BOOT, BTN_EV_DOWN, 0);
    g_ui_ctx = UI_CTX_HOME;
    step(5100);
    CHECK(s_a.st == APP_ERROR && !s_a.portal_wait && strstr(ui_last(UI_ERROR)->s, "Закройте"));
    g_boot_down = false;
    btn(BOARD_BTN_BOOT, BTN_EV_UP, 5100);
    g_portal = false;
    step(4600);

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
    post_ev(EV_PTT_UP, 100, APP_SRC_BOOT);
    step(APP_DOUBLE_MS + 40);
    CHECK(s_a.st == APP_ERROR && g_rec_starts == 0 && !strcmp(s_a.last_err, "M0"));
    s_low_mem = false;
}

int main(void)
{
    t_hold_text_only();
    t_tap_latched();
    t_double();
    t_gate();
    t_inject_end();
    t_voice();
    t_barge();
    t_cancel();
    t_errors();
    t_cards();
    t_translator();
    t_text();
    t_power();
    t_misc();
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
