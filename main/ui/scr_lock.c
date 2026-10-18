// Lock screen (docs/ui.md 3.16, A35-A38). PWR locks and unlocks; nothing about it is stored, the device always
// boots unlocked. One overlay object on the top layer covers whatever screen is open (the stack below stays as it
// is), catches every touch and draws in LV_EVENT_DRAW_MAIN: the iris (a black annulus whose inner radius moves),
// the battery ring on the rim, the padlock and its pulse ring. Clock, date, hint and battery line are child labels.
//   Lock (700 ms): the iris closes from the rim to the centre, the padlock appears there, its shackle drops with an
//   overshoot and clicks, a soft ring pulses out, then the padlock glides up above the clock as the content fades in.
//   Unlock (560 ms): the shackle springs up, the padlock grows and fades with the content, the iris opens on the
//   screen that was locked.
//   PWR again while one of them runs plays the running timeline back from where it stands (no jump).
// Only what changes is invalidated: the band the iris or the pulse moved over, in 30 deg pieces (never the 360 px box
// of a ring), the padlock's old and new box, and the labels (6.1).
#include "ui_internal.h"
#include "ui_geom.h"
#include "ui_rim.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// ---- Geometry (ui.md 3.16, each checked against r 168)
#define IRIS_R          182     // just past the glass (r 180): an open iris draws nothing visible
#define IRIS_MIN        3       // below this inner radius the backdrop is one rectangle
#define SEG_DEG         30      // ring bands are invalidated in 12 pieces
#define RING_R          168     // battery ring r 160-168, from 12 o'clock clockwise
#define RING_W          8
#define HL_DEG          36      // charging highlight length
#define HL_STEP         4       // ... and its step per HL_TICK_MS (100 deg/s)
#define HL_TICK_MS      40
#define LK_CX           180
#define LK_CY_MID       180     // padlock while it locks: the centre
#define LK_CY_REST      84      // at rest above the clock: k 0.55 -> box x 162-198, y 59-109 (|(18,121)| = 122)
#define LK_K_REST       0.55f
#define LK_LIFT         16.0f   // shackle raised (base px); its short leg then clears the body by 10 px
#define CLOCK_CY        160     // ui_48, x 100-260, y 132-188 (|(80,48)| = 93)
#define CLOCK_W         160
#define DATE_CY         208     // ui_16, x 70-290, y 198-218 (|(110,38)| = 116); the hint takes its place
#define DATE_W          220
#define BAT_CY          262     // ui_20, x 100-260, y 249-275 (|(80,95)| = 124)
#define BAT_W           160
#define TIME_OK         1700000000

// ---- Timelines (ms)
#define LOCK_MS         700
#define LK_IRIS_MS      300     // 0-300 iris closes
#define LK_APPEAR_T     180     // 180-340 padlock appears in the centre (scale 0.6 -> 1)
#define LK_DROP_T       320     // 320-450 shackle drops, overshoot and click
#define LK_DROP_MS      130
#define LK_PULSE_T      380     // 380-560 ring pulse from the click (over before the ring band fades in: at most
#define LK_PULSE_MS     180     // 12 + 12 + 6 invalidated areas of LV_INV_BUF_SIZE 32 would be too close)
#define LK_GLIDE_T      450     // 450-640 padlock glides up (scale 1 -> 0.55)
#define LK_GLIDE_MS     190
#define LK_FADE_T       560     // 560-700 clock, date, battery fade in, once the padlock has left the clock
#define LK_FADE_MS      140
#define UNLOCK_MS       560
#define UL_SPRING_MS    180     // 0-180 shackle springs up
#define UL_GROW_T       140     // 140-340 padlock grows (0.55 -> 0.95) and fades
#define UL_FADE_T       60      // 60-240 content fades out
#define UL_IRIS_T       300     // 300-560 iris opens
#define WAKE_SHOW_MS    400     // PWR on a dark lock screen: it is lit this long before the unlock runs
#define SHAKE_MS        360

typedef enum { PH_NONE, PH_LOCKING, PH_LOCKED, PH_UNLOCKING } phase_t;

typedef struct {
    float cx, cy, k;            // centre of the shut padlock and scale (1 = 60 x 84 px)
    float lift;                 // shackle raised by this many base px (0 = shut)
    float dx;                   // shake offset, px
    uint8_t a;                  // 0..255 pre-mixed against the black backdrop (no alpha blending of large areas)
} padlock_t;

static atomic_bool s_locked;
static phase_t s_phase;
static lv_obj_t *s_ov, *s_clock, *s_date, *s_hint, *s_bat;
static int32_t s_tvar, s_t;     // timeline variable / position on the shown timeline
static bool s_tl_unlock;        // the frames come from the unlock timeline (A36), else the lock one (A35); a reversal
                                // plays the other phase's timeline backwards
static bool s_frozen;           // console: the frame stays until the next lock request
static int32_t s_iris = IRIS_R;
static padlock_t s_pl;
static bool s_pl_on;
static lv_area_t s_pl_box;
static int32_t s_pulse_r = -1;
static uint8_t s_pulse_a;
static uint8_t s_content;       // 0..255: clock, date, battery ring and line
static int32_t s_hint_mix;      // 0 date, 255 hint
static int32_t s_shake_v;       // 0..SHAKE_MS, SHAKE_MS = still
static int32_t s_hl = -1;       // charging highlight head, degrees after 12 o'clock (-1 = none)
static int32_t s_hl_a0, s_hl_a1;    // its drawn span
static lv_timer_t *s_hl_timer, *s_clock_timer;
static int s_last_min = -1;
static uint32_t s_wake_t;       // the lock screen came back from "off" at this tick, 0 = not lately

bool ui_keylocked(void)
{
    return atomic_load(&s_locked);
}

bool ui_lockscr_active(void)
{
    return s_ov != NULL;
}

lv_obj_t *ui_lockscr_obj(void)
{
    return s_ov;
}

const char *ui_bat_glyph(int pct)
{
    return pct >= 88 ? SYM_BAT_FULL : pct >= 63 ? SYM_BAT_3 : pct >= 38 ? SYM_BAT_2 : pct >= 13 ? SYM_BAT_1
                                                                                                 : SYM_BAT_EMPTY;
}

// ---- Easing (u in 0..1)

static float clampf(float u)
{
    return u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
}

static float ease_out(float u)
{
    float v = 1.0f - u;
    return 1.0f - v * v * v;
}

static float ease_in(float u)
{
    return u * u * u;
}

static float ease_io(float u)
{
    if (u < 0.5f) {
        return 4.0f * u * u * u;
    }
    float v = -2.0f * u + 2.0f;
    return 1.0f - v * v * v / 2.0f;
}

// ease_out_back: about 10 % past the end, then back (the shackle's click, ui.md 1.6 allows this one overshoot)
static float back_out(float u)
{
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float v = u - 1.0f;
    return 1.0f + c3 * v * v * v + c1 * v * v;
}

static lv_color_t premix(lv_color_t c, uint8_t a)
{
    return lv_color_mix(c, lv_color_black(), a);
}

// ---- Invalidation

// The ring band r_in..r_out around (cx, cy), between angles a0 < a1 (may pass 360), in SEG_DEG pieces
static void inval_arc(int32_t cx, int32_t cy, int32_t r_in, int32_t r_out, int32_t a0, int32_t a1)
{
    if (!s_ov || r_out <= 0 || a1 <= a0) {
        return;
    }
    r_out += 1;                     // anti-aliased edges
    r_in = r_in > 1 ? r_in - 1 : 0;
    if (r_out <= 48) {
        lv_area_t a = { cx - r_out, cy - r_out, cx + r_out, cy + r_out };
        lv_obj_invalidate_area(s_ov, &a);
        return;
    }
    int32_t w = r_out - r_in;
    if (a1 - a0 > 360) {
        a1 = a0 + 360;
    }
    while (a0 < 0) {
        a0 += 360;
        a1 += 360;
    }
    while (a0 < a1) {
        int32_t s = a0 % 360;
        int32_t step = SEG_DEG - (s % SEG_DEG);
        int32_t e = s + (a1 - a0 < step ? a1 - a0 : step);
        lv_area_t ar;
        lv_draw_arc_get_area(cx, cy, (uint16_t)r_out, s, e, w, false, &ar);
        lv_obj_invalidate_area(s_ov, &ar);
        a0 += e - s;
    }
}

static void inval_band(int32_t r_in, int32_t r_out)
{
    inval_arc(UI_CX, UI_CY, r_in, r_out, 0, 360);
}

static void set_iris(int32_t r)
{
    r = LV_CLAMP(0, r, IRIS_R);
    if (r == s_iris) {
        return;
    }
    inval_band(LV_MIN(r, s_iris), LV_MAX(r, s_iris));
    s_iris = r;
}

static lv_area_t padlock_box(const padlock_t *p)
{
    float lift = p->lift > 0 ? p->lift : 0;
    lv_area_t a = {
        (int32_t)(p->cx + p->dx - 30.0f * p->k) - 2, (int32_t)(p->cy - (42.0f + lift) * p->k) - 2,
        (int32_t)(p->cx + p->dx + 30.0f * p->k) + 2, (int32_t)(p->cy + 42.0f * p->k) + 2,
    };
    return a;
}

static void set_padlock(const padlock_t *p)
{
    if (!s_ov) {
        return;
    }
    if (!p) {
        if (s_pl_on) {
            lv_obj_invalidate_area(s_ov, &s_pl_box);
            s_pl_on = false;
        }
        return;
    }
    if (s_pl_on && p->cx == s_pl.cx && p->cy == s_pl.cy && p->k == s_pl.k && p->lift == s_pl.lift &&
        p->dx == s_pl.dx && p->a == s_pl.a) {
        return;
    }
    if (s_pl_on) {
        lv_obj_invalidate_area(s_ov, &s_pl_box);
    }
    s_pl = *p;
    s_pl_on = true;
    s_pl_box = padlock_box(p);
    lv_obj_invalidate_area(s_ov, &s_pl_box);
}

static void set_pulse(int32_t r, uint8_t a)
{
    if (r == s_pulse_r && a == s_pulse_a) {
        return;
    }
    int32_t lo = LV_MIN(r, s_pulse_r), hi = LV_MAX(r, s_pulse_r);
    if (lo < 0) {
        lo = hi;
    }
    if (hi > 0) {
        inval_arc(LK_CX, LK_CY_MID, lo - 3, hi, 0, 360);
    }
    s_pulse_r = r;
    s_pulse_a = a;
}

static void label_opa(void)
{
    if (!s_ov) {
        return;
    }
    lv_obj_set_style_opa(s_clock, s_content, 0);
    lv_obj_set_style_opa(s_bat, s_content, 0);
    lv_obj_set_style_opa(s_date, (lv_opa_t)(s_content * (255 - s_hint_mix) / 255), 0);
    lv_obj_set_style_opa(s_hint, (lv_opa_t)(s_content * s_hint_mix / 255), 0);
}

static void set_content(uint8_t a)
{
    if (a == s_content) {
        return;
    }
    s_content = a;
    label_opa();
    inval_band(RING_R - RING_W, RING_R);
}

// ---- Drawing

static void arc_abs(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t r, int32_t w, int32_t a0, int32_t a1,
                    lv_color_t c, bool rounded)
{
    if (a1 <= a0 || r <= 0 || w <= 0) {
        return;
    }
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)r;
    d.width = w;
    d.color = c;
    d.opa = LV_OPA_COVER;
    d.rounded = rounded ? 1 : 0;
    d.start_angle = a0 % 360;
    d.end_angle = (a1 - a0 >= 360) ? d.start_angle + 360 : a1 % 360;
    lv_draw_arc(layer, &d);
}

static void rect_abs(lv_layer_t *layer, float x1, float y1, float x2, float y2, float radius, lv_color_t c,
                     lv_color_t c_bot)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = c;
    d.radius = (int32_t)(radius + 0.5f);
    if (!lv_color_eq(c, c_bot)) {
        d.bg_grad.dir = LV_GRAD_DIR_VER;
        d.bg_grad.stops_count = 2;
        d.bg_grad.stops[0].color = c;
        d.bg_grad.stops[0].opa = LV_OPA_COVER;
        d.bg_grad.stops[0].frac = 0;
        d.bg_grad.stops[1].color = c_bot;
        d.bg_grad.stops[1].opa = LV_OPA_COVER;
        d.bg_grad.stops[1].frac = 255;
    }
    lv_area_t a = { (int32_t)(x1 + 0.5f), (int32_t)(y1 + 0.5f), (int32_t)(x2 + 0.5f) - 1, (int32_t)(y2 + 0.5f) - 1 };
    if (a.x2 >= a.x1 && a.y2 >= a.y1) {
        lv_draw_rect(layer, &d, &a);
    }
}

// Padlock in base units (k = 1): body 60 x 46 r 12 from cy - 4 to cy + 42 in the mode accent (vertical gradient,
// like the orb core); shackle an arc of outer r 21, width 7 centred 21 above the body's top edge, a long left leg
// that always reaches into the body and a short right leg that leaves it when the shackle rises; keyhole in ink.
static void draw_padlock(lv_layer_t *layer, const padlock_t *p)
{
    float k = p->k, cx = p->cx + p->dx, cy = p->cy;
    const ui_accent_t *acc = ui_accent(ui_mode_accent(ui_get(g_s_mode)));
    lv_color_t shackle = premix(ui_col(UI_COL_TEXT2), p->a);
    lv_color_t top = premix(ui_col(acc->top), p->a), bot = premix(ui_col(acc->bot), p->a);
    lv_color_t ink = premix(ui_col(UI_COL_INK), p->a);
    float ay = cy - (21.0f + p->lift) * k;          // shackle arc centre
    float ro = 21.0f * k, w = 7.0f * k;
    if (w < 2.0f) {
        w = 2.0f;
    }
    arc_abs(layer, (int32_t)(cx + 0.5f), (int32_t)(ay + 0.5f), (int32_t)(ro + 0.5f), (int32_t)(w + 0.5f), 180, 360,
            shackle, false);
    rect_abs(layer, cx - ro, ay - 1.0f, cx - ro + w, cy + 6.0f * k, 0, shackle, shackle);          // long leg
    rect_abs(layer, cx + ro - w, ay - 1.0f, cx + ro, ay + 23.0f * k, 0, shackle, shackle);         // short leg
    rect_abs(layer, cx - 30.0f * k, cy - 4.0f * k, cx + 30.0f * k, cy + 42.0f * k, 12.0f * k, top, bot);
    float kr = 5.5f * k;
    rect_abs(layer, cx - kr, cy + 14.0f * k - kr, cx + kr, cy + 14.0f * k + kr, kr, ink, ink);       // keyhole
    rect_abs(layer, cx - 2.0f * k, cy + 15.0f * k, cx + 2.0f * k, cy + 28.0f * k, 2.0f * k, ink, ink);
}

static lv_color_t level_colour(int pct, bool full)
{
    ui_acc_t a = full || pct >= 30 ? UI_ACC_SUCCESS : pct >= 15 ? UI_ACC_WARNING : UI_ACC_ERROR;
    return ui_col(ui_accent(a)->top);
}

static void draw_ring(lv_layer_t *layer)
{
    int32_t v = ui_get(g_s_battery);
    int pct = UI_BAT_PCT(v);
    if (pct == UI_BAT_NONE) {
        return;
    }
    bool full = (v & UI_BAT_FULL) != 0;
    arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 0, 360, premix(ui_col(UI_COL_SURFACE2), s_content), false);
    int32_t deg = full ? 360 : pct * 360 / 100;
    lv_color_t c = level_colour(pct, full);
    if (deg >= 360) {
        arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 0, 360, premix(c, s_content), false);
    } else if (deg > 0) {
        arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 270, 270 + deg, premix(c, s_content), true);
    }
    if (s_hl >= 0 && s_hl_a1 > s_hl_a0) {
        lv_color_t hl = lv_color_mix(lv_color_white(), c, 110);
        arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 270 + s_hl_a0, 270 + s_hl_a1, premix(hl, s_content), true);
    }
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_iris < IRIS_MIN) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_black();
        d.bg_opa = LV_OPA_COVER;
        lv_area_t a = { 0, 0, UI_W - 1, UI_H - 1 };
        lv_draw_rect(layer, &d, &a);
    } else if (s_iris < IRIS_R) {
        arc_abs(layer, UI_CX, UI_CY, IRIS_R, IRIS_R - s_iris, 0, 360, lv_color_black(), false);
    }
    if (s_content > 0) {
        draw_ring(layer);
    }
    if (s_pulse_r > 0 && s_pulse_a > 0) {
        lv_color_t c = ui_col(ui_accent(ui_mode_accent(ui_get(g_s_mode)))->top);
        arc_abs(layer, LK_CX, LK_CY_MID, s_pulse_r, 3, 0, 360, premix(c, s_pulse_a), false);
    }
    if (s_pl_on && s_pl.a > 0) {
        draw_padlock(layer, &s_pl);
    }
}

// ---- Content

static void clock_refresh(bool force)
{
    static const char *const wd_ru[] = { "вс", "пн", "вт", "ср", "чт", "пт", "сб" };
    static const char *const wd_en[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mon_ru[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля",
                                          "августа", "сентября", "октября", "ноября", "декабря" };
    static const char *const mon_en[] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };
    if (!s_ov) {
        return;
    }
    time_t now = time(NULL);
    if (now < TIME_OK) {
        if (force || s_last_min != -2) {
            s_last_min = -2;
            lv_label_set_text_static(s_clock, "--:--");
            lv_label_set_text_static(s_date, "");
        }
        return;
    }
    struct tm t;
    localtime_r(&now, &t);
    if (!force && t.tm_min == s_last_min) {
        return;
    }
    s_last_min = t.tm_min;
    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(s_clock, buf);
    bool en = ui_str_lang() != 0;
    snprintf(buf, sizeof(buf), "%s, %d %s", en ? wd_en[t.tm_wday % 7] : wd_ru[t.tm_wday % 7], t.tm_mday,
             en ? mon_en[t.tm_mon % 12] : mon_ru[t.tm_mon % 12]);
    lv_label_set_text(s_date, buf);
}

static void clock_cb(lv_timer_t *t)
{
    (void)t;
    clock_refresh(false);
}

static bool hl_wanted(void)
{
    int32_t v = ui_get(g_s_battery);
    return s_phase == PH_LOCKED && (v & UI_BAT_CHARGING) && !(v & UI_BAT_FULL) && UI_BAT_PCT(v) != UI_BAT_NONE;
}

static void hl_span(int32_t head, int32_t span, int32_t *a0, int32_t *a1)
{
    *a0 = LV_MAX(0, head - HL_DEG);
    *a1 = LV_MIN(head, span);
}

static void hl_cb(lv_timer_t *t)
{
    (void)t;
    if (!hl_wanted() || ui_sleep_level() >= 3) {
        if (s_hl >= 0) {
            inval_arc(UI_CX, UI_CY, RING_R - RING_W, RING_R, 270 + s_hl_a0 - 6, 270 + s_hl_a1 + 6);
            s_hl = -1;
        }
        if (s_hl_timer) {
            lv_timer_pause(s_hl_timer);     // hl_update() resumes it: the wake, a battery change, the lock's end
        }
        return;
    }
    // A38: a brighter piece travels along the filled part of the ring, about 100 deg/s, then starts over
    int32_t span = UI_BAT_PCT(ui_get(g_s_battery)) * 360 / 100;
    if (span < HL_DEG / 2) {
        span = HL_DEG / 2;
    }
    int32_t head = s_hl < 0 ? 0 : s_hl + HL_STEP;
    if (head > span + HL_DEG) {
        head = 0;
    }
    int32_t a0, a1;
    hl_span(head, span, &a0, &a1);
    int32_t lo = s_hl < 0 ? a0 : LV_MIN(a0, s_hl_a0), hi = s_hl < 0 ? a1 : LV_MAX(a1, s_hl_a1);
    s_hl = head;
    s_hl_a0 = a0;
    s_hl_a1 = a1;
    // rounded caps reach about 4 px (2 deg) past the span
    inval_arc(UI_CX, UI_CY, RING_R - RING_W, RING_R, 270 + lo - 4, 270 + hi + 4);
}

static void hl_update(void)
{
    if (!hl_wanted()) {
        return;     // the timer stops itself on its next tick
    }
    if (!s_hl_timer) {
        s_hl_timer = lv_timer_create(hl_cb, HL_TICK_MS, NULL);
    }
    lv_timer_resume(s_hl_timer);
}

static void bat_refresh(void)
{
    if (!s_ov) {
        return;
    }
    int32_t v = ui_get(g_s_battery);
    int pct = UI_BAT_PCT(v);
    char buf[64];
    unsigned long ok = (unsigned long)ui_accent(UI_ACC_SUCCESS)->top;
    if (pct == UI_BAT_NONE) {
        buf[0] = 0;
    } else if (v & UI_BAT_FULL) {
        snprintf(buf, sizeof(buf), "#%06lX %s#", ok, ui_s(S_LK_FULL));
    } else {
        char num[16];
        snprintf(num, sizeof(num), ui_s(S_LK_PCT), pct);
        if (v & UI_BAT_CHARGING) {
            snprintf(buf, sizeof(buf), "#%06lX " SYM_BOLT "# %s", ok, num);
        } else if (pct < 15) {
            snprintf(buf, sizeof(buf), "#%06lX %s#", (unsigned long)ui_accent(UI_ACC_ERROR)->top, num);
        } else {
            snprintf(buf, sizeof(buf), "%s", num);
        }
    }
    lv_label_set_text(s_bat, buf);
    if (s_content > 0) {
        inval_band(RING_R - RING_W, RING_R);
    }
    hl_update();
}

static void bat_obs(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    bat_refresh();
}

void ui_lockscr_lang(void)
{
    if (s_ov) {
        clock_refresh(true);
        lv_label_set_text_static(s_hint, ui_s(S_LK_HINT));
        bat_refresh();
    }
}

// ---- Rejected input (touch, BOOT): the padlock shakes and the hint replaces the date for 1.5 s

static void shake_exec(void *var, int32_t v)
{
    (void)var;
    s_shake_v = v;
    if (s_phase != PH_LOCKED) {
        return;
    }
    float u = (float)v / (float)SHAKE_MS;
    padlock_t p = s_pl;
    p.dx = u >= 1.0f ? 0.0f : 6.0f * (float)lv_trigo_sin((int16_t)(1080.0f * u)) / 32768.0f * (1.0f - u);
    set_padlock(&p);
}

static void hint_exec(void *var, int32_t v)
{
    (void)var;
    s_hint_mix = v;
    label_opa();
}

static void hint_fade(int32_t to, uint32_t delay, uint32_t ms, lv_anim_completed_cb_t done)
{
    lv_anim_delete(&s_hint_mix, hint_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_hint_mix);
    lv_anim_set_exec_cb(&a, hint_exec);
    lv_anim_set_values(&a, s_hint_mix, to);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, to ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_set_early_apply(&a, false);     // the fade back starts from where the hint is after the delay
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void hint_shown(lv_anim_t *a)
{
    (void)a;
    hint_fade(0, 1400, 200, NULL);      // back to the date after 1.4 s
}

static void nudge(void)
{
    lv_display_trigger_activity(NULL);      // the lock screen's 10 s / 20 s count from here
    if (ui_asleep()) {
        ui_sleep_wake();
    }
    if (s_phase != PH_LOCKED) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_shake_v);
    lv_anim_set_exec_cb(&a, shake_exec);
    lv_anim_set_values(&a, 0, SHAKE_MS);
    lv_anim_set_duration(&a, SHAKE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_delete(&s_shake_v, shake_exec);
    lv_anim_start(&a);
    hint_fade(255, 0, UI_T_FAST, hint_shown);   // "Нажмите PWR" in the date's place (A22-like swap)
}

static void press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in) {
        lv_indev_wait_release(in);      // nothing below gets this touch, not even its release
    }
    nudge();
}

// ---- Overlay

static lv_obj_t *label(const char *name, const lv_style_t *st, int32_t cy, int32_t w)
{
    lv_obj_t *l = lv_label_create(s_ov);
    lv_obj_set_name_static(l, name);
    lv_obj_add_style(l, (lv_style_t *)st, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    int32_t h = lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN));
    lv_obj_set_size(l, w, h);
    lv_obj_set_pos(l, UI_CX - w / 2, cy - h / 2);
    lv_obj_set_style_opa(l, 0, 0);
    lv_label_set_text_static(l, "");
    return l;
}

static void overlay_create(void)
{
    if (!getenv("TZ")) {
        setenv("TZ", "MSK-3", 1);       // as the ambient clock: the prompt builder may not have run yet
        tzset();
    }
    s_ov = lv_obj_create(lv_layer_top());
    ui_theme_plain(s_ov);
    lv_obj_set_name_static(s_ov, "lock");
    lv_obj_set_pos(s_ov, 0, 0);
    lv_obj_set_size(s_ov, UI_W, UI_H);
    lv_obj_add_flag(s_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ov, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_ov, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_ov, press_cb, LV_EVENT_PRESSED, NULL);
    s_iris = IRIS_R;
    s_pl_on = false;
    s_pulse_r = -1;
    s_pulse_a = 0;
    s_content = 0;
    s_hint_mix = 0;
    s_shake_v = SHAKE_MS;
    s_hl = -1;
    s_last_min = -1;
    s_clock = label("lock_clock", &st_text48, CLOCK_CY, CLOCK_W);
    s_date = label("lock_date", &st_text16, DATE_CY, DATE_W);
    s_hint = label("lock_hint", &st_text16_1, DATE_CY, DATE_W);
    lv_label_set_text_static(s_hint, ui_s(S_LK_HINT));
    s_bat = label("lock_bat", &st_text20, BAT_CY, BAT_W);
    lv_label_set_recolor(s_bat, true);
    clock_refresh(true);
    ui_watch(g_s_battery, bat_obs, s_ov, NULL);     // runs once now; removed with the overlay
    s_clock_timer = lv_timer_create(clock_cb, 1000, NULL);
}

static void overlay_delete(void)
{
    lv_anim_delete(&s_shake_v, shake_exec);
    lv_anim_delete(&s_hint_mix, hint_exec);
    if (s_clock_timer) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }
    if (s_hl_timer) {
        lv_timer_delete(s_hl_timer);
        s_hl_timer = NULL;
    }
    if (s_ov) {
        lv_obj_delete(s_ov);
        s_ov = NULL;
    }
    s_hl = -1;
}

// ---- Timelines

static void frame_lock(int32_t t)
{
    set_iris(t < LK_IRIS_MS ? (int32_t)((float)IRIS_R * (1.0f - ease_io((float)t / LK_IRIS_MS)) + 0.5f) : 0);
    if (t >= LK_APPEAR_T) {
        float e = ease_out(clampf((float)(t - LK_APPEAR_T) / 160.0f));
        padlock_t p = { .cx = LK_CX, .cy = LK_CY_MID, .k = 0.6f + 0.4f * e, .lift = LK_LIFT, .dx = 0,
                        .a = (uint8_t)(255.0f * e) };
        if (t >= LK_DROP_T) {
            p.lift = LK_LIFT * (1.0f - back_out(clampf((float)(t - LK_DROP_T) / LK_DROP_MS)));
        }
        if (t >= LK_GLIDE_T) {
            float g = ease_io(clampf((float)(t - LK_GLIDE_T) / LK_GLIDE_MS));
            p.cy = LK_CY_MID + (LK_CY_REST - LK_CY_MID) * g;
            p.k = 1.0f + (LK_K_REST - 1.0f) * g;
        }
        set_padlock(&p);
    } else {
        set_padlock(NULL);
    }
    // the click's pulse only on the way in: played back it would shrink and vanish at r 40
    if (s_phase == PH_LOCKING && t >= LK_PULSE_T && t < LK_PULSE_T + LK_PULSE_MS) {
        float u = (float)(t - LK_PULSE_T) / LK_PULSE_MS;
        set_pulse((int32_t)(40.0f + 52.0f * ease_out(u)), (uint8_t)(170.0f * (1.0f - u) * (1.0f - u)));
    } else {
        set_pulse(-1, 0);
    }
    set_content(t >= LK_FADE_T ? (uint8_t)(255.0f * ease_out(clampf((float)(t - LK_FADE_T) / LK_FADE_MS))) : 0);
}

static void frame_unlock(int32_t t)
{
    padlock_t p = { .cx = LK_CX, .cy = LK_CY_REST, .k = LK_K_REST, .lift = LK_LIFT, .dx = 0, .a = 255 };
    if (t < UL_SPRING_MS) {
        p.lift = LK_LIFT * back_out((float)t / UL_SPRING_MS);
    }
    if (t >= UL_GROW_T) {
        float u = ease_in(clampf((float)(t - UL_GROW_T) / 200.0f));
        p.k = LK_K_REST + (0.95f - LK_K_REST) * u;
        p.a = (uint8_t)(255.0f * (1.0f - u));
    }
    set_padlock(p.a > 0 ? &p : NULL);
    set_pulse(-1, 0);
    set_content(t < UL_FADE_T ? 255
                : t < UL_FADE_T + 180 ? (uint8_t)(255.0f * (1.0f - ease_in((float)(t - UL_FADE_T) / 180.0f)))
                                      : 0);
    set_iris(t < UL_IRIS_T ? 0 : (int32_t)((float)IRIS_R * ease_io(clampf((float)(t - UL_IRIS_T) / 260.0f)) + 0.5f));
}

static void frame_exec(void *var, int32_t v)
{
    (void)var;
    s_t = v;
    if (s_tl_unlock) {
        frame_unlock(v);
    } else {
        frame_lock(v);
    }
}

// Where the shown timeline ends for the running phase: forwards on its own one, or back to the start of the other
static int32_t phase_end(void)
{
    if (s_phase == PH_LOCKING) {
        return s_tl_unlock ? 0 : LOCK_MS;
    }
    return s_tl_unlock ? UNLOCK_MS : 0;
}

static void phase_done(void)
{
    if (s_phase == PH_LOCKING) {
        frame_exec(&s_tvar, phase_end());  // A35 at its end, or A36 back at its start: the same locked frame
        s_tl_unlock = false;
        s_phase = PH_LOCKED;
        hl_update();
    } else if (s_phase == PH_UNLOCKING) {
        frame_exec(&s_tvar, phase_end());
        overlay_delete();
        s_phase = PH_NONE;
        s_tl_unlock = false;
        s_wake_t = 0;
        ui_rim_power_only(false);
    }
}

static void done_cb(lv_anim_t *a)
{
    (void)a;
    if (!s_frozen) {
        phase_done();
    }
}

// The shown timeline from `from` to the running phase's end (backwards when that is a reversal)
static void run(int32_t from, uint32_t delay)
{
    int32_t to = phase_end();
    lv_anim_delete(&s_tvar, frame_exec);
    s_frozen = false;
    frame_exec(&s_tvar, from);
    if (from == to) {
        phase_done();
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_tvar);
    lv_anim_set_exec_cb(&a, frame_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, (uint32_t)LV_ABS(to - from));
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);   // the frames ease their own parts
    lv_anim_set_completed_cb(&a, done_cb);
    lv_anim_start(&a);
}

static void lock_start(void)
{
    if (s_phase == PH_LOCKED) {
        return;
    }
    if (s_phase == PH_LOCKING) {
        if (s_frozen) {
            s_frozen = false;
            phase_done();       // a held console frame: finish it
        }
        return;
    }
    int32_t from = s_t;
    if (s_phase == PH_NONE) {
        overlay_create();
        s_tl_unlock = false;
        from = 0;
    }
    // else PWR again while it opens: the running timeline plays back from the frame it shows (the iris closes from
    // where it stands, the padlock and the content come back), so nothing jumps
    s_wake_t = 0;
    lv_display_trigger_activity(NULL);      // the lock screen's 10 s / 20 s count from the lock (console too)
    s_phase = PH_LOCKING;
    atomic_store(&s_locked, true);
    ui_rim_power_only(true);
    ui_emit(UI_IN_LOCK, 1);     // the app ends what runs (recording, request, speech)
    run(from, 0);
}

static void unlock_start(void)
{
    if (s_phase == PH_NONE) {
        return;
    }
    if (s_phase == PH_UNLOCKING) {
        if (s_frozen) {
            s_frozen = false;
            phase_done();
        }
        return;
    }
    int32_t from = s_t;
    uint32_t delay = 0;
    if (s_phase == PH_LOCKED) {
        s_tl_unlock = true;
        from = 0;
        if (s_wake_t && lv_tick_elaps(s_wake_t) < WAKE_SHOW_MS) {
            // PWR on the dark lock screen (5.8): it lights first, then the unlock runs (one press, about 1 s in all)
            delay = WAKE_SHOW_MS - lv_tick_elaps(s_wake_t);
        }
    }
    // else PWR again while it locks: the running timeline plays back from the frame it shows
    s_wake_t = 0;
    lv_display_trigger_activity(NULL);      // the screen below must not dim at once after a long lock
    s_phase = PH_UNLOCKING;
    atomic_store(&s_locked, false);
    ui_emit(UI_IN_LOCK, 0);
    run(from, delay);
}

void ui_lockscr_request(int32_t req)
{
    if (req == UI_LOCK_TOGGLE) {
        req = atomic_load(&s_locked) ? UI_LOCK_OFF : UI_LOCK_ON;
    }
    switch (req) {
    case UI_LOCK_ON: lock_start(); break;
    case UI_LOCK_OFF: unlock_start(); break;
    case UI_LOCK_NUDGE: nudge(); break;
    default: break;
    }
}

void ui_lockscr_woke(int from_level)
{
    if (s_ov && from_level >= 3) {
        s_wake_t = lv_tick_get();
        if (!s_wake_t) {
            s_wake_t = 1;
        }
    }
    hl_update();
}

bool ui_lockscr_freeze(bool unlock, int32_t ms)
{
    if (ms < 0) {
        if (!s_frozen) {
            return false;
        }
        s_frozen = false;
        phase_done();
        return true;
    }
    if (!unlock && s_phase == PH_NONE) {
        lock_start();
    } else if (unlock && s_phase == PH_LOCKED) {
        unlock_start();
    }
    phase_t want = unlock ? PH_UNLOCKING : PH_LOCKING;
    if (s_phase != want || s_tl_unlock != unlock) {
        return false;       // e.g. a reversal is running: its frames are the other timeline's
    }
    lv_anim_delete(&s_tvar, frame_exec);
    s_frozen = true;
    frame_exec(&s_tvar, LV_MIN(ms, unlock ? UNLOCK_MS : LOCK_MS));
    return true;
}
