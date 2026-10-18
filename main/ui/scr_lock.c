// Lock screen (docs/ui.md 3.16, A35-A38). PWR locks and unlocks; nothing about it is stored, the device always
// boots unlocked. One overlay object on the top layer covers whatever screen is open (the stack below stays as it
// is), catches every touch and draws in LV_EVENT_DRAW_MAIN: the iris (a black annulus whose inner radius moves),
// the battery ring on the rim, the padlock and its pulse ring. Clock, date, hint and battery line are child labels.
//   Lock (700 ms): the iris closes from the rim to the centre, the padlock appears there, its shackle drops with an
//   overshoot and clicks, a soft ring pulses out, then the padlock glides up above the clock as the content fades in.
//   Unlock (560 ms): the shackle springs up, the padlock grows and fades with the content, the iris opens on the
//   screen that was locked.
//   PWR again while one of them runs plays the running timeline back from where it stands (no jump).
//   Charging (A38, while locked and lit): every 1.6 s an energy comet runs from 12 o'clock along the filled ring to
//   the level and dissolves into it; the filled arc flashes up as it arrives and fades to 65 % until the next one,
//   the bolt left of the percent glows with it, and small particles rise from below the percent and fade. Full (A39):
//   "Заряжено" and one slow glow of the ring. On USB power that does not keep up: a still amber plug and a note.
// Only what changes is invalidated: the band the iris or the pulse moved over, in 30 deg pieces (never the 360 px box
// of a ring), the padlock's old and new box, the comet's span, the bolt's glow box, the particles' zone, the filled
// arc only when its quantised brightness steps, and the labels (6.1).
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
#define LK_CX           180
#define LK_CY_MID       180     // padlock while it locks: the centre
#define LK_CY_REST      84      // at rest above the clock: k 0.55 -> box x 162-198, y 59-109 (|(18,121)| = 122)
#define LK_K_REST       0.55f
#define LK_LIFT         16.0f   // shackle raised (base px); its short leg then clears the body by 10 px
#define CLOCK_CY        160     // ui_48, x 100-260, y 132-188 (|(80,48)| = 93)
#define CLOCK_W         160
#define DATE_CY         208     // ui_16, x 70-290, y 198-218 (|(110,38)| = 116); the hint takes its place
#define DATE_W          220
#define BAT_CY          262     // ui_20, x 100-260, y 249-275 (|(80,95)| = 124); shifted right by up to 10 px
#define BAT_W           160     // while charging, the bolt left of the text: x 110-270 (|(90,95)| = 131)
#define NOTE_CY         292     // ui_16 "USB: не хватает тока", x 70-290, y 282-302 (|(110,122)| = 164.3)
#define NOTE_W          220
#define TIME_OK         1700000000

// ---- Charging animation (A38) and the full glow (A39), parameters of the one 33 ms ticker
#define CHG_CYCLE_MS    1600    // one pulse; the comet runs in its last CHG_RUN_MS and arrives at the level at 0,
#define CHG_RUN_MS      1000    // then dissolves into it in the next cycle's first CHG_ABSORB_MS
#define CHG_ABSORB_MS   400
#define CHG_RISE_MS     120     // the filled arc brightens to 100 % in this after the arrival and fades to
#define CHG_LOW         166     // ... 65 % (pre-mixed against black) by the next arrival
#define CHG_STEPS       19      // its brightness in 20 steps: the whole arc is repainted only on a step
#define COMET_LEN       52      // deg: tail 50 %, body 31 %, head 19 %, each a rounded arc on the ring band
#define BOLT_GAP        6       // the bolt is drawn left of the percent (scaled glyphs would need a layer, 6.3)
#define GLOW_R2         12      // its glow: two pre-mixed discs (success halo2 / halo1) that swell with the pulse,
#define GLOW_R2_UP      6       // r 12 -> 18 and 8 -> 12; box +-19 around (bolt, 262): within x 131-179, y 243-281
#define GLOW_R1         8
#define GLOW_R1_UP      4
#define GLOW_BOX        19
#define PART_N          7       // particles: born at y 318, rise 34 px while they fade in and out
#define PART_LIFE_MS    1800
#define PART_Y0         318
#define PART_RISE       34
#define PART_X0         138     // their zone, invalidated as one box each frame: x 138-222, y 278-324
#define PART_X1         222     // (|(42,144)| = 150), between the battery line (y 275) and the ring (r 160)
#define PART_Y_TOP      278
#define PART_Y_BOT      324
#define FULL_GLOW_MS    2400    // full: the ring mixes up to 40 % toward white and back, once
#define FULL_GLOW_MAX   100
#define FULL_STEPS      16

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
typedef enum { BK_NONE, BK_BATTERY, BK_CHARGING, BK_FULL, BK_DRAIN } bat_kind_t;

typedef struct {
    float cx, cy, k;            // centre of the shut padlock and scale (1 = 60 x 84 px)
    float lift;                 // shackle raised by this many base px (0 = shut)
    float dx;                   // shake offset, px
    uint8_t a;                  // 0..255 pre-mixed against the black backdrop (no alpha blending of large areas)
} padlock_t;

static atomic_bool s_locked;
static phase_t s_phase;
static lv_obj_t *s_ov, *s_clock, *s_date, *s_hint, *s_bat, *s_note;
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
static lv_timer_t *s_clock_timer;
static int s_last_min = -1;
static uint32_t s_wake_t;       // the lock screen came back from "off" at this tick, 0 = not lately
// battery line and the charging animation (A38) / full glow (A39); the rest state is the pulse's peak, so the
// content fading in and out never jumps
static bat_kind_t s_kind;       // what the battery line shows
static int32_t s_bolt_x;        // centre of the drawn bolt while charging, 0 = none
static lv_timer_t *s_anim_timer;
static uint32_t s_anim_t0;      // lv_tick at the charging clock's 0
static int32_t s_anim_t = -1;   // charging clock (ms) of the frame shown, -1 = not running (no particles)
static int32_t s_anim_hold = -1;    // console: the charging clock held at this, -1 = runs
static int32_t s_comet_a0 = -1, s_comet_a1 = -1;     // comet span, degrees after 12 o'clock (-1 = none)
static uint8_t s_comet_mix;     // its brightness toward white (fades as it dissolves)
static uint8_t s_breath = 255;  // filled arc pre-mixed brightness, quantised (CHG_LOW..255)
static uint8_t s_bglow = 255;   // bolt glow 0..255, quantised
static uint32_t s_glow_t0;      // full glow start (lv_tick), 0 = not running
static int32_t s_glow_hold = -1;    // console: the full glow held at this ms
static uint8_t s_glow_v;        // full glow, mix toward white

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
    lv_obj_set_style_opa(s_note, s_content, 0);
    lv_obj_set_style_opa(s_date, (lv_opa_t)(s_content * (255 - s_hint_mix) / 255), 0);
    lv_obj_set_style_opa(s_hint, (lv_opa_t)(s_content * s_hint_mix / 255), 0);
}

static void inval_glow(int32_t x)
{
    if (s_ov && x > 0) {
        lv_area_t a = { x - GLOW_BOX, BAT_CY - GLOW_BOX, x + GLOW_BOX, BAT_CY + GLOW_BOX };
        lv_obj_invalidate_area(s_ov, &a);
    }
}

static void inval_particles(void)
{
    if (s_ov) {
        lv_area_t a = { PART_X0, PART_Y_TOP, PART_X1 - 1, PART_Y_BOT - 1 };
        lv_obj_invalidate_area(s_ov, &a);
    }
}

static void set_content(uint8_t a)
{
    if (a == s_content) {
        return;
    }
    s_content = a;
    label_opa();
    inval_band(RING_R - RING_W, RING_R);
    inval_glow(s_bolt_x);           // the drawn bolt and its glow reach past the battery label's box
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

static lv_color_t whiten(lv_color_t c, uint8_t mix)
{
    return lv_color_mix(lv_color_white(), c, mix);
}

static int32_t filled_deg(int32_t v)
{
    return (v & UI_BAT_FULL) ? 360 : UI_BAT_PCT(v) * 360 / 100;
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
    int32_t deg = filled_deg(v);
    lv_color_t c = level_colour(pct, full);
    // the charging pulse dims and brightens the filled arc (A38), the full glow lightens it once (A39)
    lv_color_t fill = premix(s_glow_v ? whiten(c, s_glow_v) : c, (uint8_t)(s_content * s_breath / 255));
    if (deg >= 360) {
        arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 0, 360, fill, false);
    } else if (deg > 0) {
        arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 270, 270 + deg, fill, true);
    }
    if (s_comet_a1 > s_comet_a0) {
        // the comet: tail, body and head, each lighter (three short arcs, no gradients), drawn over the arc
        int32_t a0 = s_comet_a0, a1 = s_comet_a1, len = a1 - a0;
        int32_t b0 = a0 + len / 2, b1 = a1 - len * 19 / 100;
        uint8_t m = s_comet_mix;
        arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 270 + a0, 270 + b0, premix(whiten(c, m * 50 / 255), s_content),
                true);
        arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 270 + b0, 270 + b1, premix(whiten(c, m * 110 / 255), s_content),
                true);
        arc_abs(layer, UI_CX, UI_CY, RING_R, RING_W, 270 + b1, 270 + a1, premix(whiten(c, m * 190 / 255), s_content),
                true);
    }
}

static void disc(lv_layer_t *layer, float cx, float cy, float r, lv_color_t c)
{
    rect_abs(layer, cx - r, cy - r, cx + r, cy + r, r, c, c);
}

// Charging: the particles, the bolt's glow and the bolt left of the percent (its text is the s_bat label)
static void draw_charge(lv_layer_t *layer)
{
    if (!s_bolt_x) {
        return;
    }
    const ui_accent_t *ok = ui_accent(UI_ACC_SUCCESS);
    lv_color_t top = ui_col(ok->top);
    if (s_anim_t >= 0) {
        // each rises 34 px over its life with a slight sway, fading in and out (sin envelope); 7 staggered lives
        static const struct {
            int8_t dx;
            uint8_t d;
            uint16_t ofs;
        } k_part[PART_N] = { { -30, 4, 0 },    { 12, 5, 257 },  { -9, 3, 514 },   { 27, 4, 771 },
                             { -19, 5, 1028 }, { 3, 4, 1285 },  { 20, 3, 1542 } };
        lv_color_t pc = whiten(top, 64);
        int32_t fade_in = LV_CLAMP(0, (s_anim_t - CHG_RISE_MS) * 255 / 400, 255);   // none pops in mid-life
        for (int i = 0; i < PART_N; i++) {
            int32_t ph = (s_anim_t + k_part[i].ofs) % PART_LIFE_MS;
            float u = (float)ph / PART_LIFE_MS;
            int32_t a = lv_trigo_sin((int16_t)(180 * ph / PART_LIFE_MS)) * s_content / 32767 * fade_in / 255;
            if (a <= 8) {
                continue;
            }
            float x = UI_CX + k_part[i].dx + 3.0f * lv_trigo_sin((int16_t)(360 * ph / PART_LIFE_MS + 97 * i)) / 32767;
            float y = PART_Y0 - PART_RISE * ease_out(u);
            float r = k_part[i].d / 2.0f;
            disc(layer, x, y, r, premix(pc, (uint8_t)a));
        }
    }
    float e = s_bglow / 255.0f;
    uint8_t k = (uint8_t)(s_content * (96.0f + 159.0f * e) / 255.0f);
    disc(layer, (float)s_bolt_x, BAT_CY, GLOW_R2 + GLOW_R2_UP * e, premix(ui_col(ok->halo2), k));
    disc(layer, (float)s_bolt_x, BAT_CY, GLOW_R1 + GLOW_R1_UP * e, premix(ui_col(ok->halo1), k));
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &ui_20;
    d.color = premix(whiten(top, (uint8_t)(110.0f * e)), s_content);
    d.opa = LV_OPA_COVER;
    d.text = SYM_BOLT;
    d.text_static = 1;
    d.align = LV_TEXT_ALIGN_CENTER;
    int32_t lh = lv_font_get_line_height(&ui_20);
    lv_area_t a = { s_bolt_x - 16, BAT_CY - lh / 2, s_bolt_x + 15, BAT_CY - lh / 2 + lh - 1 };
    lv_draw_label(layer, &d, &a);
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
        draw_charge(layer);
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

// ---- Charging animation (A38) and the full glow (A39)

static bat_kind_t bat_kind(int32_t v)
{
    return UI_BAT_PCT(v) == UI_BAT_NONE ? BK_NONE
         : (v & UI_BAT_FULL)            ? BK_FULL
         : (v & UI_BAT_CHARGING)        ? BK_CHARGING
         : (v & UI_BAT_DRAIN)           ? BK_DRAIN
                                        : BK_BATTERY;
}

// The filled arc's span, plus the rounded cap
static void inval_filled(void)
{
    int32_t deg = filled_deg(ui_get(g_s_battery));
    if (deg >= 354) {
        inval_band(RING_R - RING_W, RING_R);
    } else if (deg > 0) {
        inval_arc(UI_CX, UI_CY, RING_R - RING_W, RING_R, 270 - 3, 270 + deg + 3);
    }
}

// Comet span a0..a1 (degrees after 12 o'clock, clipped to it); a1 <= 0 = none. Only the union of the old and the new
// span is repainted (rounded caps reach about 1.5 deg past it).
static void set_comet(int32_t a0, int32_t a1, uint8_t mix)
{
    a0 = LV_MAX(0, a0);
    if (a1 <= a0) {
        a0 = a1 = -1;
    }
    if (a0 == s_comet_a0 && a1 == s_comet_a1 && mix == s_comet_mix) {
        return;
    }
    int32_t lo = a0, hi = a1;
    if (s_comet_a1 > s_comet_a0) {
        lo = a1 > a0 ? LV_MIN(a0, s_comet_a0) : s_comet_a0;
        hi = a1 > a0 ? LV_MAX(a1, s_comet_a1) : s_comet_a1;
    }
    if (hi > lo) {
        inval_arc(UI_CX, UI_CY, RING_R - RING_W, RING_R, 270 + lo - 3, 270 + hi + 3);
    }
    s_comet_a0 = a0;
    s_comet_a1 = a1;
    s_comet_mix = mix;
}

static void set_breath(int32_t b)
{
    int32_t q = (LV_CLAMP(CHG_LOW, b, 255) - CHG_LOW) * CHG_STEPS / (255 - CHG_LOW);
    uint8_t v = (uint8_t)(CHG_LOW + q * (255 - CHG_LOW) / CHG_STEPS);
    if (v != s_breath) {
        s_breath = v;
        inval_filled();
    }
}

static void set_bolt_glow(int32_t p)
{
    int32_t q = LV_CLAMP(0, p, 255) >> 3;
    uint8_t v = (uint8_t)(q << 3 | q >> 2);     // 32 steps, 31 -> 255
    if (v != s_bglow) {
        s_bglow = v;
        inval_glow(s_bolt_x);
    }
}

static void set_full_glow(int32_t g)
{
    int32_t q = LV_CLAMP(0, g, FULL_GLOW_MAX) * FULL_STEPS / FULL_GLOW_MAX;
    uint8_t v = (uint8_t)(q * FULL_GLOW_MAX / FULL_STEPS);
    if (v != s_glow_v) {
        s_glow_v = v;
        inval_band(RING_R - RING_W, RING_R);
    }
}

// The frame at charging clock t: the comet runs in each cycle's last CHG_RUN_MS (ease in-out, so it lands softly)
// and dissolves at the level in the next cycle's first CHG_ABSORB_MS; the filled arc and the bolt's glow peak at the
// arrival and fade until the next one; the particles follow t.
static void charge_frame(int32_t t)
{
    int32_t span = LV_MAX(UI_BAT_PCT(ui_get(g_s_battery)) * 360 / 100, 10);
    int32_t c = t % CHG_CYCLE_MS, run0 = CHG_CYCLE_MS - CHG_RUN_MS;
    if (c >= run0) {
        int32_t h = (int32_t)((float)span * ease_io((float)(c - run0) / CHG_RUN_MS) + 0.5f);
        set_comet(h - COMET_LEN, h, 255);
    } else if (c < CHG_ABSORB_MS && t >= CHG_CYCLE_MS) {
        float u = (float)c / CHG_ABSORB_MS;
        set_comet(span - (int32_t)((float)COMET_LEN * (1.0f - ease_out(u)) + 0.5f), span, (uint8_t)(255.0f * (1.0f - u)));
    } else {
        set_comet(-1, -1, 0);
    }
    float e;
    if (c < CHG_RISE_MS) {
        e = (float)c / CHG_RISE_MS;
    } else {
        float u = 1.0f - (float)(c - CHG_RISE_MS) / (CHG_CYCLE_MS - CHG_RISE_MS);
        e = u * u;
    }
    set_breath(CHG_LOW + (int32_t)((255 - CHG_LOW) * e + 0.5f));
    set_bolt_glow((int32_t)(255.0f * e + 0.5f));
    if (t != s_anim_t) {
        s_anim_t = t;
        inval_particles();
    }
}

// Back to the rest state (no comet, no particles, the arc and the glow at their peak), repainting what changes
static void charge_clear(void)
{
    set_comet(-1, -1, 0);
    set_breath(255);
    set_bolt_glow(255);
    if (s_anim_t >= 0) {
        s_anim_t = -1;
        inval_particles();
    }
    s_anim_hold = -1;
}

// Full glow at ms into it: up and back down (sin), FULL_GLOW_MS in all
static int32_t full_glow_at(int32_t ms)
{
    return ms >= FULL_GLOW_MS ? 0 : FULL_GLOW_MAX * lv_trigo_sin((int16_t)(180 * ms / FULL_GLOW_MS)) / 32767;
}

static bool lit_locked(void)
{
    return s_ov && s_phase == PH_LOCKED && ui_sleep_level() < 3;
}

static void anim_tick(lv_timer_t *t)
{
    bool chg = s_kind == BK_CHARGING && s_phase == PH_LOCKED;
    if (!lit_locked()) {
        lv_timer_pause(t);          // anim_update() resumes it: the wake, a battery change, the lock's end
        return;
    }
    uint32_t now = lv_tick_get();
    if (chg && s_anim_hold < 0) {
        charge_frame((int32_t)(now - s_anim_t0));
    }
    if (s_glow_t0 && s_glow_hold < 0) {
        uint32_t el = now - s_glow_t0;
        set_full_glow(full_glow_at((int32_t)LV_MIN(el, FULL_GLOW_MS)));
        if (el >= FULL_GLOW_MS) {
            s_glow_t0 = 0;
        }
    }
    if (!(chg && s_anim_hold < 0) && !(s_glow_t0 && s_glow_hold < 0)) {
        lv_timer_pause(t);
    }
}

// Starts, stops or resumes the animation for the current battery state and lock phase
static void anim_update(void)
{
    if (!s_ov) {
        return;
    }
    bool chg = s_kind == BK_CHARGING && s_phase == PH_LOCKED;
    if (!chg) {
        charge_clear();
    } else if (s_anim_t < 0 && s_anim_hold < 0) {
        s_anim_t0 = lv_tick_get() - CHG_RISE_MS;    // starts at the peak: the rest state, no jump
    }
    if (s_kind != BK_FULL || s_phase != PH_LOCKED) {
        s_glow_t0 = 0;
        s_glow_hold = -1;
        set_full_glow(0);
    }
    bool run = lit_locked() && ((chg && s_anim_hold < 0) || (s_glow_t0 && s_glow_hold < 0));
    if (run && !s_anim_timer) {
        s_anim_timer = lv_timer_create(anim_tick, UI_TICK_MS, NULL);
    }
    if (s_anim_timer) {
        if (run) {
            lv_timer_resume(s_anim_timer);
        } else {
            lv_timer_pause(s_anim_timer);
        }
    }
}

// One slow glow when the lock screen shows "full": at the lock's end, when the state becomes full, on a wake
static void full_glow_start(void)
{
    if (s_kind == BK_FULL && lit_locked() && s_glow_hold < 0) {
        s_glow_t0 = lv_tick_get();
        if (!s_glow_t0) {
            s_glow_t0 = 1;
        }
    }
    anim_update();
}

bool ui_lockscr_chg_hold(bool glow, int32_t ms)
{
    if (!s_ov || s_phase != PH_LOCKED) {
        return false;
    }
    if (ms < 0) {
        // runs on from the frame held (no jump)
        bool held = s_anim_hold >= 0 || s_glow_hold >= 0;
        uint32_t now = lv_tick_get();
        if (s_anim_hold >= 0) {
            s_anim_t0 = now - (uint32_t)s_anim_hold;
        }
        if (s_glow_hold >= 0) {
            s_glow_t0 = s_glow_hold < FULL_GLOW_MS ? LV_MAX(1u, now - (uint32_t)s_glow_hold) : 0;
        }
        s_anim_hold = s_glow_hold = -1;
        anim_update();
        return held;
    }
    if (glow ? s_kind != BK_FULL : s_kind != BK_CHARGING) {
        return false;
    }
    if (glow) {
        s_glow_hold = ms;
        set_full_glow(full_glow_at(LV_MIN(ms, FULL_GLOW_MS)));
    } else {
        s_anim_hold = ms;
        charge_frame(ms);
    }
    anim_update();                  // the ticker stops while everything is held
    return true;
}

static void bat_refresh(void)
{
    if (!s_ov) {
        return;
    }
    int32_t v = ui_get(g_s_battery);
    int pct = UI_BAT_PCT(v);
    bat_kind_t k = bat_kind(v), was = s_kind;
    char buf[64], num[16];
    unsigned long ok = (unsigned long)ui_accent(UI_ACC_SUCCESS)->top;
    int32_t dx = 0, bolt_x = 0;
    snprintf(num, sizeof(num), ui_s(S_LK_PCT), pct);
    switch (k) {
    case BK_NONE:
        buf[0] = 0;
        break;
    case BK_FULL:
        snprintf(buf, sizeof(buf), "#%06lX " SYM_CHECK " %s#", ok, ui_s(S_LK_FULL));
        break;
    case BK_CHARGING: {
        // the percent alone; the bolt is drawn left of it (draw_charge), the pair centred
        snprintf(buf, sizeof(buf), "%s", num);
        lv_point_t sz;
        lv_text_get_size(&sz, buf, &ui_20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t bw = lv_font_get_glyph_width(&ui_20, 0xF0E7, 0);
        bolt_x = UI_CX - (bw + BOLT_GAP + sz.x) / 2 + bw / 2;
        dx = (bw + BOLT_GAP) / 2;
        break;
    }
    case BK_DRAIN:
        // on USB power that does not keep up (the speaker loud on a PC port): a still plug, the note below
        snprintf(buf, sizeof(buf), "#%06lX " SYM_PLUG "# %s", (unsigned long)ui_accent(UI_ACC_WARNING)->top, num);
        break;
    default:
        if (pct < 15) {
            snprintf(buf, sizeof(buf), "#%06lX %s#", (unsigned long)ui_accent(UI_ACC_ERROR)->top, num);
        } else {
            snprintf(buf, sizeof(buf), "%s", num);
        }
        break;
    }
    lv_label_set_text(s_bat, buf);
    lv_obj_set_style_translate_x(s_bat, dx, 0);
    lv_label_set_text_static(s_note, k == BK_DRAIN ? ui_s(S_LK_DRAIN) : "");
    if (bolt_x != s_bolt_x) {
        inval_glow(s_bolt_x);
        s_bolt_x = bolt_x;
        inval_glow(s_bolt_x);
    }
    if (s_content > 0) {
        inval_band(RING_R - RING_W, RING_R);
    }
    s_kind = k;
    if (k == BK_FULL && was != BK_FULL) {
        full_glow_start();          // calls anim_update()
    } else {
        anim_update();
    }
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
    s_last_min = -1;
    s_kind = BK_NONE;
    s_bolt_x = 0;
    s_anim_t = s_anim_hold = s_glow_hold = -1;
    s_comet_a0 = s_comet_a1 = -1;
    s_comet_mix = 0;
    s_breath = s_bglow = 255;
    s_glow_t0 = 0;
    s_glow_v = 0;
    s_clock = label("lock_clock", &st_text48, CLOCK_CY, CLOCK_W);
    s_date = label("lock_date", &st_text16, DATE_CY, DATE_W);
    s_hint = label("lock_hint", &st_text16_1, DATE_CY, DATE_W);
    lv_label_set_text_static(s_hint, ui_s(S_LK_HINT));
    s_bat = label("lock_bat", &st_text20, BAT_CY, BAT_W);
    lv_label_set_recolor(s_bat, true);
    s_note = label("lock_note", &st_text16, NOTE_CY, NOTE_W);      // text-2
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
    if (s_anim_timer) {
        lv_timer_delete(s_anim_timer);
        s_anim_timer = NULL;
    }
    if (s_ov) {
        lv_obj_delete(s_ov);
        s_ov = NULL;
    }
    s_anim_t = s_anim_hold = s_glow_hold = -1;
    s_glow_t0 = 0;
    s_bolt_x = 0;
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
        full_glow_start();                 // and the charging animation (anim_update)
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
    anim_update();                          // the charging animation ends (rest state) before the content fades
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
        full_glow_start();      // lit again: "Заряжено" glows once more
        return;
    }
    anim_update();
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
