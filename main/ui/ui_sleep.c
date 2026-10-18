// Sleep, ambient and screen off (docs/ui.md 5.8): idle for dim_s -> backlight 25 % and a sleepy face; 60 s more
// -> Z ambient at 15 %; 5 min more -> backlight off. Any touch only wakes (a top-layer catcher swallows it),
// BOOT and PWR wake through the app (UI_WAKE). Text entry, phone setup, the adjuster, the touch test, the
// onboarding (with every screen opened from it) and the Wi-Fi connecting step never sleep, nor does a busy turn.
// The lock screen (3.16) has its own short timers: dimmed after 10 s, off after 20 s, never ambient.
#include "ui_internal.h"
#include "ui_orb.h"
#include "ui_scr.h"
#include "ui_work.h"
#include "board.h"

#include <stdatomic.h>

#define TICK_MS         1000
#define AMBIENT_AFTER   60000       // after the dim
#define OFF_AFTER       300000      // after the ambient
#define DIM_PCT         25
#define AMBIENT_PCT     15
#define LOCK_DIM_MS     10000       // lock screen: dimmed after this
#define LOCK_OFF_MS     20000       // ... and off

static atomic_int s_level;
static lv_timer_t *s_tick;
static lv_obj_t *s_catcher;
static int32_t s_bl = -1;           // animated backlight value

int ui_sleep_level(void)
{
    return atomic_load(&s_level);
}

static void bl_exec(void *var, int32_t v)
{
    (void)var;
    s_bl = v;
    board_backlight_set((uint8_t)v);
}

// A32: backlight fades (800 ms down, 150 ms up) through lv_anim steps
static void bl_fade(int32_t to, uint32_t ms)
{
    lv_anim_delete(&s_bl, bl_exec);
    int32_t from = board_backlight_get();       // the app may have changed it meanwhile
    if (from == to) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_bl);
    lv_anim_set_exec_cb(&a, bl_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static int32_t awake_pct(void)
{
    return LV_CLAMP(5, ui_cfg_int("bright"), 100);
}

static void catcher_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in) {
        lv_indev_wait_release(in);      // the waking touch is swallowed (2.3)
    }
    (void)e;
    ui_sleep_wake();
    if (ui_keylocked()) {
        ui_lockscr_request(UI_LOCK_NUDGE);      // a touch on the dark lock screen lights it and shows the hint
    }
}

static void catcher_on(void)
{
    if (s_catcher) {
        return;
    }
    s_catcher = lv_obj_create(lv_layer_top());
    ui_theme_plain(s_catcher);
    lv_obj_set_name_static(s_catcher, "wake");
    lv_obj_set_pos(s_catcher, 0, 0);
    lv_obj_set_size(s_catcher, 360, 360);
    lv_obj_add_flag(s_catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_catcher, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_catcher, catcher_cb, LV_EVENT_PRESSED, NULL);
}

static void catcher_off(void)
{
    if (s_catcher) {
        lv_obj_delete_async(s_catcher);     // may run inside its own PRESSED event
        s_catcher = NULL;
    }
}

static void enter(int level)
{
    int cur = atomic_load(&s_level);
    if (level <= cur) {
        return;
    }
    atomic_store(&s_level, level);
    catcher_on();
    if (level == 1) {
        bl_fade(LV_MIN(awake_pct(), DIM_PCT), 800);
        if (ui_nav_current() == UI_SCR_HOME) {
            ui_orb_set_expr(scr_home_orb(), EXPR_SLEEPY, 0);
        }
    } else if (level == 2) {
        ui_nav_home();
        ui_nav_push(UI_SCR_AMBIENT, 0);
        lv_obj_move_foreground(s_catcher);
        bl_fade(LV_MIN(awake_pct(), AMBIENT_PCT), 800);
    } else {
        bl_fade(0, 300);
    }
    ui_emit(UI_IN_SLEEP, level);
}

void ui_sleep_wake(void)
{
    int from = atomic_load(&s_level);
    if (from == 0) {
        lv_display_trigger_activity(NULL);  // BOOT / app activity also restarts the dim timer
        return;
    }
    atomic_store(&s_level, 0);
    catcher_off();
    ui_lockscr_woke(from);
    if (ui_nav_current() == UI_SCR_AMBIENT) {
        ui_nav_back();
    }
    if (ui_nav_current() == UI_SCR_HOME) {
        ui_orb_set_expr(scr_home_orb(), EXPR_NEUTRAL, 1);      // ends the sleepy override
    }
    bl_fade(awake_pct(), 150);
    lv_display_trigger_activity(NULL);
    ui_emit(UI_IN_SLEEP, 0);
}

void ui_sleep_off(void)
{
    enter(3);
}

static bool never_sleeps(void)
{
    ui_ctx_t c = ui_context();
    ui_screen_t s = ui_nav_current();
    // the whole first-boot flow stays awake (W / K / WC on top of a step too): the ambient step goes home and
    // would drop it
    return c == UI_CTX_TEXT || c == UI_CTX_QR || c == UI_CTX_ADJUST || c == UI_CTX_TOUCH || c == UI_CTX_TEST ||
           s == UI_SCR_ONBOARD || s == UI_SCR_WIFI_CONN || s == UI_SCR_DIALOG || ui_onboarding();
}

static bool busy(void)
{
    int32_t st = ui_get(g_s_state);
    return st == UI_ST_REC || st == UI_ST_REC_LATCHED || st == UI_ST_REC_CANCEL || st == UI_ST_UPLOAD ||
           st == UI_ST_THINK || st == UI_ST_SPEAK;
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    int level = atomic_load(&s_level);
    if (ui_keylocked()) {
        // whatever the screen below and dim_s: the lock screen dims after 10 s and goes dark after 20 s
        uint32_t idle = lv_display_get_inactive_time(NULL);
        int target = idle >= LOCK_OFF_MS ? 3 : idle >= LOCK_DIM_MS ? 1 : 0;
        if (target > level) {
            enter(target);
        }
        return;
    }
    if (busy()) {
        // a turn (BOOT, an answer arriving) keeps the screen awake and wakes it
        lv_display_trigger_activity(NULL);
        if (level > 0 && level < 3) {
            ui_sleep_wake();
        }
        return;
    }
    if (level == 0 && never_sleeps()) {
        lv_display_trigger_activity(NULL);
        return;
    }
    int32_t dim_s = ui_cfg_int("dim_s");
    if (dim_s <= 0) {
        return;     // "Никогда"
    }
    uint32_t idle = lv_display_get_inactive_time(NULL);
    uint32_t dim = (uint32_t)dim_s * 1000u;
    int target = idle >= dim + AMBIENT_AFTER + OFF_AFTER ? 3 : idle >= dim + AMBIENT_AFTER ? 2 : idle >= dim ? 1 : 0;
    if (target > level) {
        enter(target);
    }
}

void ui_sleep_init(void)
{
    if (!s_tick) {
        s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    }
}
