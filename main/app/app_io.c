// Device side of the app (docs/ui.md 2.3 and 5.8, architecture 2.3 input mapping): BOOT / PWR, screen
// dim / off / wake, power off, battery, Wi-Fi and portal state, the low-memory guard, the idle
// microphone floor for the speech gate, and applying / saving settings.
#include "app_priv.h"
#include "audio_in.h"
#include "audio_out.h"
#include "devconsole.h"
#include "net.h"
#include "portal.h"
#include "settings.h"
#include "ui.h"
#include "ui_str.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "app_io";

#define DIM_PCT          25              // ui.md 5.8: dimmed backlight
#define OFF_AFTER_DIM_S  300             // dimmed this long: screen off (the ambient clock comes later)
#define NET_POLL_MS      500
#define BAT_POLL_MS      30000
#define BAT_WARN_PCT     10              // ui.md 5.6: toast "Батарея 10 %"
#define HEAP_POLL_MS     1000
#define TOUCH_POLL_MS    200
#define AMBIENT_POLL_MS  100
#define LOW_INT_FREE     (20 * 1024)     // below this no new turn starts (architecture 3.1 gates)
#define LOW_INT_BLOCK    (8 * 1024)
#define LOW_PSRAM_FREE   (512 * 1024)
#define USB_CHECK_MS     500             // still running this long after the latch was released: USB power
#define USB_NOTE_MS      2000            // "Питание от USB" stays this long before the screen goes off

typedef struct {
    const char *key;
    int32_t v;
    uint32_t due;
    bool on;
} save_t;

static atomic_int s_screen = APP_SCREEN_ON;
static atomic_int s_bat_mv = -1;
static uint32_t s_last_act;
static int s_bright = 80, s_dim_s = 60;
static bool s_pwr_woke;                  // this PWR press woke the screen: its click does nothing else
static bool s_latch_off;                 // power latch released (power off while on USB)
static bool s_off_wait_up;               // ... and PWR still held: on battery the key itself keeps the board on
static uint32_t s_off_check, s_note_until;
static int s_net = -1;
static bool s_portal;
static uint32_t s_t_net, s_t_bat, s_t_heap, s_t_touch, s_t_amb;
static bool s_bat_polled, s_bat_warned;
static bool s_low_mem;
static int s_ambient = VAD_NO_HINT;
static save_t s_save[] = { { .key = "mode" }, { .key = "text_only" } };

app_screen_t app_io_screen(void)
{
    return (app_screen_t)atomic_load(&s_screen);
}

int app_io_bat_mv(void)
{
    return atomic_load(&s_bat_mv);
}

int app_io_ambient(void)
{
    return s_ambient;
}

bool app_io_low_mem(void)
{
    return s_low_mem;
}

bool app_io_portal(void)
{
    return portal_active();
}

bool app_io_net_up(void)
{
    if (s_net < 0) {
        net_status_t ns;
        net_status(&ns);
        return ns.state == NET_UP;
    }
    return s_net == NET_UP;
}

// ---- screen

static void backlight_apply(void)
{
    int sc = atomic_load(&s_screen);
    int pct = sc == APP_SCREEN_ON ? s_bright : sc == APP_SCREEN_DIM ? (s_bright < DIM_PCT ? s_bright : DIM_PCT) : 0;
    board_backlight_set((uint8_t)pct);
}

static void screen_set(app_screen_t sc)
{
    if (atomic_exchange(&s_screen, (int)sc) == (int)sc) {
        return;
    }
    backlight_apply();
    devcon_event("screen", "\"state\":\"%s\"", sc == APP_SCREEN_ON ? "on" : sc == APP_SCREEN_DIM ? "dim" : "off");
}

bool app_io_activity(void)
{
    s_last_act = app_now_ms();
    if (s_latch_off) {
        board_power_latch_on();     // used again after a power off on USB: stay on when unplugged
        s_latch_off = s_off_wait_up = false;
        s_off_check = s_note_until = 0;
    }
    if (app_io_screen() == APP_SCREEN_ON) {
        return false;
    }
    screen_set(APP_SCREEN_ON);
    return true;
}

void app_io_screen_off(void)
{
    screen_set(APP_SCREEN_OFF);
}

void app_io_power_off(void)
{
    devcon_event("power_off", "\"by\":\"pwr_long\"");
    ESP_LOGW(TAG, "power off");
    app_abort_all();
    board_power_off();              // backlight 0, latch released: on battery the board switches off here
    s_latch_off = true;
    atomic_store(&s_screen, APP_SCREEN_OFF);
    // on battery the held key powers the board until it is released: "still running" means USB only after that
    s_off_wait_up = true;
    s_off_check = s_note_until = 0;
}

// ---- buttons

// esp_timer task: post only
static void btn_cb(board_btn_t b, board_btn_ev_t ev, uint32_t held_ms, void *ctx)
{
    (void)ctx;
    app_evt_t e = { .type = EV_BUTTON, .i = b, .j = ev, .k = (int32_t)held_ms };
    app_post(&e, 0);
}

void app_io_button(board_btn_t b, board_btn_ev_t ev, uint32_t held_ms)
{
    devcon_event("btn", "\"b\":\"%s\",\"ev\":\"%s\",\"held\":%u", board_btn_name(b), board_btn_ev_name(ev),
                 (unsigned)held_ms);
    if (b == BOARD_BTN_BOOT) {
        // BOOT = talk: it wakes the screen and acts at once (ui.md 2.3 Dimmed / Z / X rows)
        if (ev == BTN_EV_DOWN) {
            app_ptt_down(UI_SIDE_AUTO, APP_SRC_BOOT);
        } else if (ev == BTN_EV_UP) {
            app_ptt_up((int32_t)held_ms, APP_SRC_BOOT);
        }
        return;
    }
    switch (ev) {
    case BTN_EV_DOWN:
        s_pwr_woke = app_io_activity();
        ui_pwr_down();              // the rim POWER ring appears after 600 ms
        break;
    case BTN_EV_UP:
        ui_pwr_up();
        break;
    case BTN_EV_CLICK:
        if (s_pwr_woke) {
            s_pwr_woke = false;     // PWR on a dark screen only wakes it
        } else {
            app_pwr_short();
        }
        break;
    case BTN_EV_LONG:
        app_io_power_off();
        break;
    default:
        break;
    }
}

// ---- settings

static void on_setting_cb(const char *key, void *ctx)
{
    // runs in the setter's task (console, portal, app): post a copy
    (void)ctx;
    size_t n = strlen(key) + 1;
    char *k = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!k) {
        return;
    }
    memcpy(k, key, n);
    app_evt_t e = { .type = EV_SETTING, .p = k };
    app_post(&e, 0);
}

void app_io_setting(const char *key)
{
    bool all = !strcmp(key, "*");
    if (all || !strcmp(key, "volume")) {
        audio_out_set_volume((uint8_t)settings_get_int("volume"));
    }
    if (all || !strcmp(key, "bright")) {
        s_bright = settings_get_int("bright");
        backlight_apply();
    }
    if (all || !strcmp(key, "dim_s")) {
        s_dim_s = settings_get_int("dim_s");
    }
    if (all || !strcmp(key, "ui_lang")) {
        char l[8];
        settings_get_str("ui_lang", l, sizeof(l));
        ui_post(&(ui_msg_t){ .type = UI_SET_LANG, .i = !strcmp(l, "en") });
    }
    if (all || !strcmp(key, "tr_lang_a") || !strcmp(key, "tr_lang_b")) {
        char a[8], b[8], ab[20];
        settings_get_str("tr_lang_a", a, sizeof(a));
        settings_get_str("tr_lang_b", b, sizeof(b));
        snprintf(ab, sizeof(ab), "%s|%s", a, b);
        ui_post_text(UI_SET_LANGS, 0, ab);
    }
    if (all || !strcmp(key, "mic_slot") || !strcmp(key, "mic_shift")) {
        audio_in_set_format(settings_get_int("mic_slot") == 0, settings_get_int("mic_shift"));
    }
}

void app_io_save(const char *key, int32_t v, bool now)
{
    save_t *sv = NULL;
    for (size_t i = 0; i < sizeof(s_save) / sizeof(s_save[0]); i++) {
        if (!strcmp(s_save[i].key, key)) {
            sv = &s_save[i];
        }
    }
    if (settings_get_int(key) == v) {
        if (sv) {
            sv->on = false;
        }
        return;
    }
    if (now || !sv) {
        esp_err_t e = settings_set_int(key, v);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "save %s: %s", key, esp_err_to_name(e));
        }
        if (sv) {
            sv->on = false;
        }
        return;
    }
    // UI changes: flash writes stall the caches (and LVGL), so they wait 2 s and for IDLE (architecture 1.7)
    sv->v = v;
    sv->due = app_now_ms() + APP_SAVE_DELAY_MS;
    sv->on = true;
}

// ---- periodic work

static void poll_net(void)
{
    net_status_t ns;
    net_status(&ns);
    if ((int)ns.state != s_net) {
        bool was_up = s_net == NET_UP;
        s_net = (int)ns.state;
        static const ui_net_t map[] = { UI_NET_OFF, UI_NET_CONNECTING, UI_NET_UP, UI_NET_FAILED };
        ui_post(&(ui_msg_t){ .type = UI_SET_NET, .i = (unsigned)ns.state < 4 ? map[ns.state] : UI_NET_OFF });
        char ssid[72];
        app_json_esc(ssid, sizeof(ssid), ns.ssid);
        devcon_event("net", "\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\"", net_state_name(ns.state), ssid, ns.ip);
        if (was_up != (ns.state == NET_UP)) {
            app_net_changed(ns.state == NET_UP);
        }
    }
    bool portal = portal_active();
    if (portal != s_portal) {
        s_portal = portal;
        ui_post(&(ui_msg_t){ .type = UI_PORTAL, .i = portal });
        devcon_event("portal", "\"on\":%d", portal);
        app_portal_changed(portal);
    }
}

static void poll_battery(void)
{
    int mv = board_battery_mv();
    atomic_store(&s_bat_mv, mv);
    int pct = board_battery_pct(mv);
    if (pct >= 0 && pct <= BAT_WARN_PCT && !s_bat_warned) {
        s_bat_warned = true;
        char t[40];
        snprintf(t, sizeof(t), ui_str_lang() ? "Battery %d %%" : "Батарея %d %%", pct);
        ui_post_text(UI_TOAST, 0, t);
        devcon_event("battery_low", "\"mv\":%d,\"pct\":%d", mv, pct);
    } else if (pct > BAT_WARN_PCT + 5) {
        s_bat_warned = false;
    }
}

static void poll_heap(void)
{
    size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t l = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bool low = f < LOW_INT_FREE || l < LOW_INT_BLOCK || p < LOW_PSRAM_FREE;
    if (low != s_low_mem) {
        s_low_mem = low;
        if (low) {
            ESP_LOGW(TAG, "low memory: internal %u (largest %u), psram %u", (unsigned)f, (unsigned)l, (unsigned)p);
            devcon_event("heap_low", "\"int\":%u,\"largest\":%u,\"psram\":%u", (unsigned)f, (unsigned)l, (unsigned)p);
        }
    }
}

static void poll_ambient(void)
{
    // idle mic floor for the speech gate: falls to any quieter 20 ms reading, rises 2 dB/s
    if (app_state() != APP_IDLE || audio_in_recording() || audio_in_get_source() != AUDIO_SRC_MIC) {
        return;
    }
    int db = audio_in_level() * 5 - 600;    // level 0..100 = -60..-10 dBFS
    if (s_ambient == VAD_NO_HINT || db < s_ambient) {
        s_ambient = db;
    } else if (s_ambient < 0) {
        s_ambient += 2;
    }
}

static void poll_touch(void)
{
    // LVGL's inactivity timer sees the real and the injected pointer
    uint32_t idle = UINT32_MAX;
    if (ui_lock(20)) {
        idle = lv_display_get_inactive_time(NULL);
        ui_unlock();
    }
    if (idle < TOUCH_POLL_MS + 50) {
        app_io_activity();
    }
}

static void dim_tick(uint32_t now)
{
    if (!app_quiet()) {
        s_last_act = now;           // a turn keeps the screen awake
    }
    if (s_off_wait_up && !board_button_down(BOARD_BTN_PWR)) {
        s_off_wait_up = false;
        s_off_check = now + USB_CHECK_MS;
    }
    if (s_off_check && (int32_t)(now - s_off_check) >= 0) {
        // the latch was released and the board still runs: it is powered by USB (ui.md 5.8)
        s_off_check = 0;
        s_note_until = now + USB_NOTE_MS;
        screen_set(APP_SCREEN_ON);
        ui_post(&(ui_msg_t){ .type = UI_TOAST, .i = S_USB_POWER });
    }
    if (s_note_until && (int32_t)(now - s_note_until) >= 0) {
        s_note_until = 0;
        screen_set(APP_SCREEN_OFF);
    }
    if (s_dim_s <= 0 || s_note_until || s_off_check) {
        return;
    }
    uint32_t idle = now - s_last_act;
    app_screen_t sc = app_io_screen();
    if (sc == APP_SCREEN_ON && idle >= (uint32_t)s_dim_s * 1000u) {
        screen_set(APP_SCREEN_DIM);
    } else if (sc == APP_SCREEN_DIM && idle >= (uint32_t)(s_dim_s + OFF_AFTER_DIM_S) * 1000u) {
        screen_set(APP_SCREEN_OFF);
    }
}

void app_io_tick(uint32_t now)
{
    if (now - s_t_net >= NET_POLL_MS) {
        s_t_net = now;
        poll_net();
    }
    if (!s_bat_polled || now - s_t_bat >= BAT_POLL_MS) {
        s_bat_polled = true;
        s_t_bat = now;
        poll_battery();
    }
    if (now - s_t_heap >= HEAP_POLL_MS) {
        s_t_heap = now;
        poll_heap();
    }
    if (now - s_t_amb >= AMBIENT_POLL_MS) {
        s_t_amb = now;
        poll_ambient();
    }
    if (now - s_t_touch >= TOUCH_POLL_MS) {
        s_t_touch = now;
        poll_touch();
    }
    dim_tick(now);
    for (size_t i = 0; i < sizeof(s_save) / sizeof(s_save[0]); i++) {
        save_t *sv = &s_save[i];
        if (sv->on && (int32_t)(now - sv->due) >= 0 && app_quiet()) {
            sv->on = false;
            if (settings_get_int(sv->key) != sv->v) {
                esp_err_t e = settings_set_int(sv->key, sv->v);
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "save %s: %s", sv->key, esp_err_to_name(e));
                }
            }
        }
    }
}

void app_io_init(void)
{
    s_bright = settings_get_int("bright");
    s_dim_s = settings_get_int("dim_s");
    s_last_act = app_now_ms();
    settings_on_change(on_setting_cb, NULL);
    if (board_buttons_start(btn_cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "buttons unavailable");
    }
}
