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
#define NET_POLL_MS      500
#define BAT_POLL_MS      5000            // ui.md 3.16: one reading into the battery model every 5 s (every 2 s
                                         // while it measures a step: board_bat_t.poll_ms)
#define BAT_LOW_PCT      15              // toasts "Батарея 15 %" and "Батарея 5 %", once each per discharge
#define BAT_CRIT_PCT     5
#define BAT_REARM_PCT    20              // ... until charging or back above this
#define BAT_SETTLE_MS    60000           // a reading counts as settled (charging trend) this long after a load step
#define BAT_SKIP_AUDIO_MS 15000          // no reading while the speaker plays and this long after (the cell sags
#define BAT_SKIP_TURN_MS 20000           // under it), nor during a turn (its Wi-Fi / TLS bursts) and this long after,
#define BAT_SKIP_SCREEN_MS 3000          // nor during a backlight fade (150-800 ms) and its first recovery
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
static uint32_t s_last_act, s_last_wake_post;
static int s_bright = 80, s_dim_s = 60;
static bool s_pwr_woke;                  // this PWR press woke the screen: its click does nothing else
static bool s_latch_off;                 // power latch released (power off while on USB)
static bool s_off_wait_up;               // ... and PWR still held: on battery the key itself keeps the board on
static uint32_t s_off_check, s_note_until;
static int s_net = -1;
static bool s_portal;
static uint32_t s_t_net, s_t_bat, s_t_heap, s_t_touch, s_t_amb;
static bool s_bat_polled, s_bat_warned, s_bat_crit;
static uint32_t s_bat_poll_ms = BAT_POLL_MS;    // when the model wants its next reading
static int s_bat_vfull_seen = -1;       // the model's last learned v_full, saved as bat_vfull
static int32_t s_bat_posted = -1;       // UI_BAT_MAKE() value last sent to the UI
static uint32_t s_load_t;               // last load step: a turn, sound, the mic, a backlight change, the Wi-Fi state
                                        // or the portal (settle clock)
static uint32_t s_audio_t, s_turn_t, s_screen_t;   // the speaker last played, a turn last ran, the backlight changed
static int s_bl_seen = -1;              // the backlight level at the last battery reading
static bool s_low_mem;
static int s_ambient = VAD_NO_HINT;
static save_t s_save[] = { { .key = "mode" }, { .key = "text_only" }, { .key = "bat_vfull" } };

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

static const char *screen_name(app_screen_t sc)
{
    return sc == APP_SCREEN_ON ? "on" : sc == APP_SCREEN_DIM ? "dim" : "off";
}

// The UI owns dimming, ambient and the backlight fades (ui_sleep.c, ui.md 5.8); the app only asks it
// to wake or to switch off, and mirrors the level the UI reports through UI_IN_SLEEP.
static void screen_set(app_screen_t sc)
{
    if (sc == APP_SCREEN_ON) {
        ui_post(&(ui_msg_t){ .type = UI_WAKE });
    } else if (sc == APP_SCREEN_OFF) {
        ui_post(&(ui_msg_t){ .type = UI_SCREEN_OFF });
    }
    if (atomic_exchange(&s_screen, (int)sc) != (int)sc) {
        s_load_t = s_screen_t = app_now_ms();   // the backlight current changes: the battery voltage steps
        devcon_event("screen", "\"state\":\"%s\"", screen_name(sc));
    }
}

// Used again after a power off on USB (a real button or touch): stay on when unplugged
static void relatch(void)
{
    if (s_latch_off) {
        board_power_latch_on();
        s_latch_off = s_off_wait_up = false;
        s_off_check = s_note_until = 0;
        devcon_event("power_latch", "\"on\":1");
    }
}

void app_io_ui_sleep(int level)
{
    app_screen_t sc = level <= 0 ? APP_SCREEN_ON : level >= 3 ? APP_SCREEN_OFF : APP_SCREEN_DIM;
    if (level <= 0) {
        // after a power off the UI sleeps at level 3 until a touch wakes it (the USB note does not)
        relatch();
    }
    if (atomic_exchange(&s_screen, (int)sc) != (int)sc) {
        s_load_t = s_screen_t = app_now_ms();
        devcon_event("screen", "\"state\":\"%s\",\"level\":%d", screen_name(sc), level);
    }
}

bool app_io_activity(void)
{
    uint32_t now = app_now_ms();
    s_last_act = now;
    relatch();
    if (app_io_screen() == APP_SCREEN_ON) {
        if (now - s_last_wake_post >= 1000) {   // restarts the UI's dim timer, at most once a second
            s_last_wake_post = now;
            ui_post(&(ui_msg_t){ .type = UI_WAKE });
        }
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
    // the UI goes to screen off (level 3, wake catcher) and owns the backlight again; only its wake re-latches
    screen_set(APP_SCREEN_OFF);
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
        // ui.md 2.3: PWR on a dark screen only wakes it; locked, the press lights the lock screen and its click
        // unlocks (one press). A dimmed or ambient screen is still seen: its click locks.
        s_pwr_woke = app_io_screen() == APP_SCREEN_OFF && !ui_keylocked();
        app_io_activity();
        ui_pwr_down();              // the rim POWER ring appears after 600 ms
        break;
    case BTN_EV_UP:
        ui_pwr_up();
        break;
    case BTN_EV_CLICK:
        if (s_pwr_woke) {
            s_pwr_woke = false;
        } else {
            app_pwr_short();        // lock / unlock
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
    if (all || !strcmp(key, "spk_boost")) {
        audio_out_set_boost_db((uint8_t)settings_get_int("spk_boost"));
    }
    if (all || !strcmp(key, "bright")) {
        s_bright = settings_get_int("bright");
        backlight_apply();
        s_load_t = s_screen_t = app_now_ms();
    }
    if (all || !strcmp(key, "dim_s")) {
        s_dim_s = settings_get_int("dim_s");
        app_io_activity();          // a new timeout counts from now ("never" also lights a dimmed screen)
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
    if (all || !strcmp(key, "bat_vfull")) {
        // the battery's 100 % (ui.md 3.16); set by hand, a later learning that comes back to the old value is
        // stored again
        s_bat_vfull_seen = -1;
        board_battery_set_vfull(settings_get_int("bat_vfull"));
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
        if (s_net >= 0 && was_up != (ns.state == NET_UP)) {
            // joined (scanning and joining end, the radio idles in modem sleep) or lost: the radio's draw steps, and
            // so does the battery voltage (ui.md 3.16). Not the retries' connecting / failed (every minute without a
            // network: the readings would never settle; their scans are short, a spike to the step detector)
            s_load_t = app_now_ms();
        }
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
        s_load_t = app_now_ms();        // the access point's radio on or off: a load step
        ui_post(&(ui_msg_t){ .type = UI_PORTAL, .i = portal });
        devcon_event("portal", "\"on\":%d", portal);
        app_portal_changed(portal);
    }
}

static bool within(uint32_t now, uint32_t t, uint32_t ms)
{
    return t && now - t < ms;       // 0 = never
}

// How far the next reading can be trusted (ui.md 3.16): under the speaker or a turn's Wi-Fi the cell sags (a PC
// port gives less than they take), and it recovers for a while after any load, which must not read as a charger
static board_bat_q_t bat_quality(uint32_t now)
{
    if (audio_out_busy() || within(now, s_audio_t, BAT_SKIP_AUDIO_MS) || !app_quiet() ||
        within(now, s_turn_t, BAT_SKIP_TURN_MS) || within(now, s_screen_t, BAT_SKIP_SCREEN_MS)) {
        // near empty on battery the warnings below must not wait for the end of a long conversation: the reading
        // is taken under the load (it reads low, which errs on the safe side)
        board_bat_t b;
        board_battery_state(&b);
        bool low = !b.sim && b.power == BOARD_PWR_BATTERY && b.pct >= 0 && b.pct <= BAT_REARM_PCT;
        return low ? BOARD_BAT_LOADED : BOARD_BAT_SKIP;
    }
    return now - s_load_t >= BAT_SETTLE_MS ? BOARD_BAT_SETTLED : BOARD_BAT_LOADED;
}

void app_io_battery(bool sample)
{
    if (sample) {
        uint32_t now = app_now_ms();
        // a backlight change is a load step whoever made it (the UI's fades, its brightness adjuster, a QR shown at
        // full brightness): the cell's voltage steps with the backlight current, and a step up must not read as a
        // charger plugged in (ui.md 3.16)
        int bl = board_backlight_get();
        if (bl != s_bl_seen) {
            if (s_bl_seen >= 0) {
                s_load_t = s_screen_t = now;
            }
            s_bl_seen = bl;
        }
        board_battery_sample(bat_quality(now));
    }
    board_bat_t b;
    board_battery_state(&b);
    atomic_store(&s_bat_mv, b.mv);
    s_bat_poll_ms = b.poll_ms > 0 ? (uint32_t)b.poll_ms : BAT_POLL_MS;
    if (b.vfull_learn > 0 && b.vfull_learn != s_bat_vfull_seen) {
        // a plateau on the charger taught the model this divider's full level: kept for the next boot (written
        // once the app is idle, a flash write stalls the caches)
        s_bat_vfull_seen = b.vfull_learn;
        app_io_save("bat_vfull", b.vfull_learn, false);
    }
    int32_t v = UI_BAT_MAKE(b.pct, (b.power == BOARD_PWR_CHARGING ? UI_BAT_CHARGING : 0) |
                                       (b.power == BOARD_PWR_FULL ? UI_BAT_FULL : 0) |
                                       (b.power == BOARD_PWR_USB_DRAIN ? UI_BAT_DRAIN : 0) | (b.usb ? UI_BAT_USB : 0));
    if (v != s_bat_posted) {
        s_bat_posted = v;
        ui_post(&(ui_msg_t){ .type = UI_SET_BATTERY, .i = v });
        devcon_event("battery",
                     "\"mv\":%d,\"pct\":%d,\"power\":\"%s\",\"charging\":%d,\"full\":%d,\"usb\":%d,\"ext\":\"%s\","
                     "\"why\":\"%s\",\"sim\":%d",
                     b.mv, b.pct, board_power_name(b.power), b.charging, b.full, b.usb, board_ext_name(b.ext),
                     board_bat_ev_name(b.why), b.sim);
    }
    // ui.md 5.6 / 3.16: "Батарея 15 %" and "Батарея 5 %" once each while it discharges
    if (b.pct < 0 || b.charging || b.full || b.pct > BAT_REARM_PCT) {
        s_bat_warned = s_bat_crit = false;
        return;
    }
    bool crit = b.pct <= BAT_CRIT_PCT && !s_bat_crit;
    if (crit || (b.pct <= BAT_LOW_PCT && !s_bat_warned)) {
        s_bat_warned = true;
        s_bat_crit |= crit;
        char t[40];
        snprintf(t, sizeof(t), ui_s(S_BAT_LOW), b.pct);
        ui_post_text(UI_TOAST, 0, t);
        devcon_event("battery_low", "\"mv\":%d,\"pct\":%d", b.mv, b.pct);
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
    // a pointer (the panel or the console's injected one) is down: real input. LVGL's inactivity time is no
    // source: the UI resets it itself (wake, busy turns, screens that never sleep), and that would re-latch
    // the power after a power off
    bool down = false;
    if (ui_lock(20)) {
        for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
            down |= lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED;
        }
        ui_unlock();
    }
    if (down) {
        app_io_activity();
    }
}

static void dim_tick(uint32_t now)
{
    if (!app_quiet()) {
        s_last_act = now;           // a turn keeps the screen awake
    }
    if (!app_quiet() || audio_out_busy() || audio_in_recording()) {
        s_load_t = now;             // also a speaker or mic test outside a turn (D)
    }
    if (audio_out_busy()) {
        s_audio_t = now;
    }
    if (!app_quiet()) {
        s_turn_t = now;
    }
    if (s_off_wait_up && !board_button_down(BOARD_BTN_PWR)) {
        s_off_wait_up = false;
        s_off_check = now + USB_CHECK_MS;
    }
    if (s_off_check && (int32_t)(now - s_off_check) >= 0) {
        // the latch was released and the board still runs: it is powered by USB (ui.md 5.8). The UI stays at
        // screen off (a touch on the note is then a real wake); only the backlight comes up to show it
        s_off_check = 0;
        s_note_until = now + USB_NOTE_MS;
        ui_post(&(ui_msg_t){ .type = UI_TOAST, .i = S_USB_POWER });
        board_backlight_set((uint8_t)s_bright);
        s_load_t = s_screen_t = now;
    }
    if (s_note_until && (int32_t)(now - s_note_until) >= 0) {
        s_note_until = 0;
        board_backlight_set(0);
        s_load_t = s_screen_t = now;
    }
    // dim / ambient / off after dim_s are run by the UI's own idle timer (ui_sleep.c)
}

void app_io_tick(uint32_t now)
{
    if (now - s_t_net >= NET_POLL_MS) {
        s_t_net = now;
        poll_net();
    }
    if (!s_bat_polled || now - s_t_bat >= s_bat_poll_ms) {
        s_bat_polled = true;
        s_t_bat = now;
        app_io_battery(true);
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
    s_last_act = s_load_t = app_now_ms();     // boot (Wi-Fi joining, TLS) is a load too
    board_battery_set_vfull(settings_get_int("bat_vfull"));    // before the first reading
    settings_on_change(on_setting_cb, NULL);
    if (board_buttons_start(btn_cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "buttons unavailable");
    }
}
