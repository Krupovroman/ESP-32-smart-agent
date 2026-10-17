#include "ui_orb.h"
#include "ui.h"
#include "ui_sym.h"
#include "ui_theme.h"
#include "audio_in.h"
#include "audio_out.h"

#include <math.h>
#include <string.h>
#include "src/core/lv_obj_event_private.h"   // lv_hit_test_info_t fields

#define PI_F        3.14159265f
#define TICK_FAST   UI_TICK_MS
#define TICK_IDLE   100     // breathing check: redraw only when an integer halo radius changes

// A31 boot splash: core r 0 -> full (500 ms ease_out), halos black -> halo (600 ms), eyes open from BLINK at
// 450 ms (110 ms ease_out), then a glance left and right
#define INTRO_CORE_MS   500
#define INTRO_HALO_MS   600
#define INTRO_FACE_MS   200     // face hidden while the core is still small
#define INTRO_EYES_MS   450
#define INTRO_LEFT_MS   700
#define INTRO_RIGHT_MS  1250
#define INTRO_BACK_MS   1800
#define INTRO_END_MS    2100

// ---- Presets (ui.md 5.3). Hero values; compact/mini are the bracketed values of the spec. Radii of
// lv_draw_arc are outer edges; every r_out + stroke/2 stays inside box/2.
typedef struct {
    float box, cx, cy;
    float core, h1, h1b, h2, h2b;                       // halo radii: rest / breathing peak
    float bars, bar_w, bar_in, bar_len0, bar_len1, listen_dr;
    float prog_r, prog_w, arc_a_r, arc_a_w, arc_b_r, arc_b_w, think_dr;
    float echo1_r, echo1_k, echo2_r, echo2_k, echo1_w, echo2_w, speak_dr;
    float hit, cancel, face;
} geo_t;
#define GEO_N (sizeof(geo_t) / sizeof(float))

static const geo_t s_geo[ORB__N] = {
    //            box  cx   cy   core h1  h1b h2  h2b bars bw  bin l0 l1  ldr  pr  pw  aAr aAw aBr aBw tdr
    //            e1r e1k e2r e2k e1w e2w sdr  hit cancel face
    [ORB_HERO] = { 204, 180, 192, 70, 80, 84, 92, 98, 24, 6, 78, 4, 16, 4, 82, 4, 82, 4, 90, 3, 4,
                   76, 12, 84, 14, 3, 2, 4, 80, 110, 1 },
    [ORB_COMPACT] = { 92, 180, 290, 30, 36, 36, 42, 44, 16, 4, 34, 3, 7, 2, 36, 3, 36, 3, 41, 2, 2,
                      32, 6, 37, 7, 2, 2, 2, 40, 70, 1 },
    [ORB_MINI] = { 96, 180, 88, 28, 34, 35, 40, 42, 16, 4, 32, 3, 7, 2, 34, 3, 34, 3, 39, 2, 2,
                   30, 6, 35, 7, 2, 2, 2, 38, 60, 1 },
    [ORB_AMBIENT] = { 40, 180, 262, 12, 18, 19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

// ---- Expressions (eye size in k units, k = core_r / 70)
typedef struct {
    float w, h, dy;
    bool arc, smile;
} expr_def_t;

static const expr_def_t s_expr[EXPR__N] = {
    [EXPR_NEUTRAL] = { 16, 34, 0, false, false },
    [EXPR_WIDE] = { 18, 38, 0, false, false },
    [EXPR_LOOK] = { 16, 30, 0, false, false },
    [EXPR_HAPPY] = { 16, 4, 0, true, true },
    [EXPR_SOFT] = { 16, 18, 6, false, false },
    [EXPR_SLEEPY] = { 16, 12, 4, false, false },
};
#define BLINK_H 4.0f

static const char *const s_expr_names[EXPR__N] = { "neutral", "wide", "look", "happy", "soft", "sleepy" };

// ---- Tweens: pure functions of lv_tick so the draw callback never mutates state
enum { E_LIN, E_IN, E_OUT, E_INOUT };

typedef struct {
    float from, to;
    uint32_t t0, dur;
    uint8_t ease;
} tw_t;

static float ease(float u, int e)
{
    switch (e) {
    case E_IN: return u * u * u;
    case E_OUT: {
        float v = 1.0f - u;
        return 1.0f - v * v * v;
    }
    case E_INOUT: {
        if (u < 0.5f) {
            return 4.0f * u * u * u;
        }
        float v = -2.0f * u + 2.0f;
        return 1.0f - v * v * v / 2.0f;
    }
    default: return u;
    }
}

static float tw_val(const tw_t *t, uint32_t now)
{
    uint32_t el = now - t->t0;
    if (el >= t->dur) {
        return t->to;
    }
    return t->from + (t->to - t->from) * ease((float)el / (float)t->dur, t->ease);
}

static bool tw_busy(const tw_t *t, uint32_t now)
{
    return (now - t->t0) < t->dur;
}

static void tw_set(tw_t *t, float v, uint32_t now)
{
    t->from = t->to = v;
    t->t0 = now - 1;
    t->dur = 1;
}

static void tw_go(tw_t *t, float to, uint32_t dur, uint8_t e, uint32_t now)
{
    float cur = tw_val(t, now);
    if (fabsf(cur - to) < 1e-4f) {
        tw_set(t, to, now);
        return;
    }
    t->from = cur;
    t->to = to;
    t->t0 = now;
    t->dur = dur ? dur : 1;
    t->ease = (uint8_t)e;
}

// ---- Orb state
typedef struct {
    lv_color_t top, bot, h1, h2, stroke;
} pal_t;

typedef struct {
    lv_obj_t *obj;
    lv_timer_t *timer;
    orb_preset_t preset;
    geo_t g_from, g_to;
    tw_t pm;                    // preset morph 0..1
    int32_t state;
    int mode;
    bool face_on;
    uint32_t state_t0;
    pal_t pal_from, pal_to;
    tw_t col;                   // palette morph 0..1
    lv_color_t eye, eye_next;   // eye colour swaps inside a blink
    tw_t bars, arcs, echo, prog, pct;
    orb_expr_t expr_cur, expr_prev, expr_ovr;
    bool ovr_on;
    uint32_t ovr_until;         // 0 = until the next call / state change
    tw_t expr_t;                // 0..1 from expr_prev to expr_cur (A4)
    bool blinking, blink_again;
    uint32_t blink_t0, next_blink, next_glance;
    tw_t gz_x, gz_y, gz_w;      // gaze override target and weight (A3)
    uint32_t gz_until;
    int glance_flip, target_side;
    uint32_t squish_t0, shake_t0, flash_t0;
    uint32_t intro_t0;          // A31 boot splash start
    uint8_t intro_step;         // 0 = not running, 1..4 = glance sequence position
    float mic, spk, spk_d1, spk_d2;
    int16_t last_h1, last_h2;
    bool was_active;
} orb_t;

static bool st_rec(int32_t s)
{
    return s == UI_ST_REC || s == UI_ST_REC_LATCHED || s == UI_ST_REC_CANCEL;
}

static bool st_continuous(int32_t s)
{
    return st_rec(s) || s == UI_ST_UPLOAD || s == UI_ST_THINK || s == UI_ST_SPEAK;
}

static bool st_idle_like(int32_t s)
{
    return s == UI_ST_IDLE || s == UI_ST_ANSWERED;
}

static pal_t pal_for(int32_t st, int mode)
{
    const ui_accent_t *m = ui_accent(ui_mode_accent(mode));
    const ui_accent_t *a;
    lv_color_t k = lv_color_black();
    lv_color_t g = ui_col(UI_COL_OFFLINE);
    switch (st) {
    case UI_ST_REC:
    case UI_ST_REC_LATCHED:
        a = ui_accent(UI_ACC_LISTEN);
        return (pal_t){ ui_col(a->top), ui_col(a->bot), ui_col(a->halo1), k, ui_col(a->top) };
    case UI_ST_REC_CANCEL:
        return (pal_t){ g, g, k, k, g };
    case UI_ST_UPLOAD:
    case UI_ST_THINK:
        return (pal_t){ ui_col(m->mid), ui_col(m->mid), ui_col(m->dim), k, ui_col(m->top) };
    case UI_ST_SPEAK:
        a = ui_accent(UI_ACC_SPEAK);
        return (pal_t){ ui_col(a->top), ui_col(a->bot), ui_col(a->halo1), k, ui_col(a->top) };
    case UI_ST_ERROR:
        a = ui_accent(UI_ACC_ERROR);
        return (pal_t){ ui_col(a->top), ui_col(a->bot), ui_col(a->halo1), k, ui_col(a->top) };
    case UI_ST_OFFLINE:
        return (pal_t){ k, k, k, k, g };
    default:
        return (pal_t){ ui_col(m->top), ui_col(m->bot), ui_col(m->halo1), ui_col(m->halo2), ui_col(m->top) };
    }
}

static uint32_t morph_ms(int32_t st)
{
    switch (st) {
    case UI_ST_REC:
    case UI_ST_REC_LATCHED:
    case UI_ST_UPLOAD:
    case UI_ST_THINK: return 200;           // A5, A9
    case UI_ST_REC_CANCEL:
    case UI_ST_ERROR: return 120;           // A8, A23
    case UI_ST_SPEAK: return 250;           // A11
    default: return 300;                    // A13 back to idle
    }
}

static lv_color_t eye_for(int32_t st)
{
    // ink on full-accent cores, text-1 on the darker mid cores
    return (st == UI_ST_UPLOAD || st == UI_ST_THINK) ? ui_col(UI_COL_TEXT1) : ui_col(UI_COL_INK);
}

static pal_t pal_now(const orb_t *o, uint32_t now)
{
    uint8_t mix = (uint8_t)(tw_val(&o->col, now) * 255.0f + 0.5f);
    pal_t p;
    p.top = lv_color_mix(o->pal_to.top, o->pal_from.top, mix);
    p.bot = lv_color_mix(o->pal_to.bot, o->pal_from.bot, mix);
    p.h1 = lv_color_mix(o->pal_to.h1, o->pal_from.h1, mix);
    p.h2 = lv_color_mix(o->pal_to.h2, o->pal_from.h2, mix);
    p.stroke = lv_color_mix(o->pal_to.stroke, o->pal_from.stroke, mix);
    return p;
}

static void geo_now(const orb_t *o, uint32_t now, geo_t *g)
{
    float u = tw_val(&o->pm, now);
    const float *a = (const float *)&o->g_from, *b = (const float *)&o->g_to;
    float *r = (float *)g;
    for (size_t i = 0; i < GEO_N; i++) {
        r[i] = a[i] + (b[i] - a[i]) * u;
    }
}

static float breath(uint32_t t)
{
    // 3000 ms each way, ease_in_out (a cosine has the same shape)
    return 0.5f - 0.5f * cosf(2.0f * PI_F * (float)(t % 6000) / 6000.0f);
}

static orb_expr_t expr_base(const orb_t *o, uint32_t now)
{
    switch (o->state) {
    case UI_ST_REC:
    case UI_ST_REC_LATCHED: return EXPR_WIDE;
    case UI_ST_THINK: return EXPR_LOOK;
    case UI_ST_ANSWERED: return (now - o->state_t0 < 600) ? EXPR_HAPPY : EXPR_NEUTRAL;
    default: return EXPR_NEUTRAL;
    }
}

static bool ovr_live(const orb_t *o, uint32_t now)
{
    return o->ovr_on && (o->ovr_until == 0 || (int32_t)(o->ovr_until - now) > 0);
}

static orb_expr_t expr_eff(const orb_t *o, uint32_t now)
{
    return ovr_live(o, now) ? o->expr_ovr : expr_base(o, now);
}

static bool face_visible(const orb_t *o, const geo_t *g)
{
    if (!o->face_on || g->face < 0.5f) {
        return false;
    }
    switch (o->state) {
    case UI_ST_REC_CANCEL:
    case UI_ST_ERROR:
    case UI_ST_OFFLINE: return false;
    case UI_ST_REC_LATCHED: return o->preset == ORB_HERO;   // compact: the stop mark replaces the eyes
    default: return true;
    }
}

static float blink_val(const orb_t *o, uint32_t now)
{
    if (!o->blinking) {
        return 0;
    }
    uint32_t el = now - o->blink_t0;
    if (el < UI_T_BLINK_IN) {
        return ease((float)el / UI_T_BLINK_IN, E_IN);
    }
    if (el < UI_T_BLINK_IN + UI_T_BLINK_HOLD) {
        return 1;
    }
    el -= UI_T_BLINK_IN + UI_T_BLINK_HOLD;
    return el < UI_T_BLINK_OUT ? 1.0f - ease((float)el / UI_T_BLINK_OUT, E_OUT) : 0;
}

// Gaze in k units: the state's base direction blended with the touch / glance override
static void gaze_now(const orb_t *o, uint32_t now, float *gx, float *gy)
{
    float bx = 0, by = 0;
    switch (o->state) {
    case UI_ST_UPLOAD: by = -6; break;
    case UI_ST_THINK: {
        // LOOK: up-right, saccade to up-left and back every 900 ms (260 ms moves)
        uint32_t el = now - o->state_t0;
        uint32_t win = el / 900;
        float to = (win & 1) ? -6.0f : 6.0f;
        float from = win == 0 ? 0.0f : -to;
        float u = (float)(el % 900) / 260.0f;
        bx = from + (to - from) * ease(u > 1 ? 1 : u, E_INOUT);
        by = -6;
        break;
    }
    case UI_ST_SPEAK:
        if (o->target_side == 0) {
            bx = -8, by = 6;
        } else if (o->target_side == 1) {
            bx = 8, by = 6;
        }
        break;
    case UI_ST_ANSWERED: by = 6; break;
    default: break;
    }
    float w = tw_val(&o->gz_w, now);
    *gx = bx + (tw_val(&o->gz_x, now) - bx) * w;
    *gy = by + (tw_val(&o->gz_y, now) - by) * w;
}

// ---- Drawing helpers (absolute coordinates)

static void circle(lv_layer_t *layer, float cx, float cy, float r, lv_color_t top, lv_color_t bot, bool grad)
{
    int32_t ri = (int32_t)(r + 0.5f);
    if (ri <= 0) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = top;
    if (grad && !lv_color_eq(top, bot)) {
        d.bg_grad.dir = LV_GRAD_DIR_VER;
        d.bg_grad.stops_count = 2;
        d.bg_grad.stops[0].color = top;
        d.bg_grad.stops[0].opa = LV_OPA_COVER;
        d.bg_grad.stops[0].frac = 0;
        d.bg_grad.stops[1].color = bot;
        d.bg_grad.stops[1].opa = LV_OPA_COVER;
        d.bg_grad.stops[1].frac = 255;
    }
    int32_t x = (int32_t)(cx + 0.5f), y = (int32_t)(cy + 0.5f);
    lv_area_t a = { x - ri, y - ri, x + ri - 1, y + ri - 1 };
    lv_draw_rect(layer, &d, &a);
}

static void capsule(lv_layer_t *layer, float cx, float cy, float w, float h, lv_color_t c)
{
    int32_t wi = (int32_t)(w + 0.5f), hi = (int32_t)(h + 0.5f);
    if (wi < 2) {
        wi = 2;
    }
    if (hi < 2) {
        hi = 2;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = c;
    int32_t x1 = (int32_t)(cx - w / 2.0f + 0.5f), y1 = (int32_t)(cy - h / 2.0f + 0.5f);
    lv_area_t a = { x1, y1, x1 + wi - 1, y1 + hi - 1 };
    lv_draw_rect(layer, &d, &a);
}

static void arc(lv_layer_t *layer, float cx, float cy, float r_out, float w, int32_t a0, int32_t a1, lv_color_t c,
                lv_opa_t opa, bool rounded)
{
    if (r_out < 1 || w < 0.5f || opa <= LV_OPA_MIN) {
        return;
    }
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = (int32_t)(cx + 0.5f);
    d.center.y = (int32_t)(cy + 0.5f);
    d.radius = (uint16_t)(r_out + 0.5f);
    d.width = (int32_t)(w + 0.5f) > 0 ? (int32_t)(w + 0.5f) : 1;
    d.color = c;
    d.opa = opa;
    d.rounded = rounded ? 1 : 0;
    d.start_angle = a0;
    d.end_angle = a1;
    lv_draw_arc(layer, &d);
}

static void glyph(lv_layer_t *layer, float cx, float cy, const char *txt, const lv_font_t *f, lv_color_t c)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = txt;
    d.font = f;
    d.color = c;
    d.align = LV_TEXT_ALIGN_CENTER;
    int32_t lh = lv_font_get_line_height(f);
    int32_t x = (int32_t)cx, y = (int32_t)cy;
    lv_area_t a = { x - 30, y - lh / 2, x + 30, y - lh / 2 + lh - 1 };
    lv_draw_label(layer, &d, &a);
}

static void draw_face(const orb_t *o, lv_layer_t *layer, float cx, float cy, float k, uint32_t now)
{
    float u = tw_val(&o->expr_t, now);
    const expr_def_t *pe = &s_expr[o->expr_prev], *ce = &s_expr[o->expr_cur];
    float pw = pe->w, ph = pe->h, cw = ce->w, ch = ce->h;
    if (o->state == UI_ST_REC || o->state == UI_ST_REC_LATCHED) {
        float grow = 4.0f * o->mic;     // WIDE: 38 + 4L
        if (o->expr_prev == EXPR_WIDE) {
            ph += grow;
        }
        if (o->expr_cur == EXPR_WIDE) {
            ch += grow;
        }
    }
    float w, h, dy;
    bool as_arc;
    if (pe->arc == ce->arc) {
        w = pw + (cw - pw) * u;
        h = ph + (ch - ph) * u;
        dy = pe->dy + (ce->dy - pe->dy) * u;
        as_arc = ce->arc;
    } else if (u < 0.5f) {
        // a shape change goes through BLINK at the mid-point (A4)
        float h0 = pe->arc ? BLINK_H : ph;
        w = pe->arc ? 16 : pw;
        h = h0 + (BLINK_H - h0) * (u * 2.0f);
        dy = pe->dy;
        as_arc = false;
    } else {
        w = ce->arc ? 16 : cw;
        h = BLINK_H + (ch - BLINK_H) * ((u - 0.5f) * 2.0f);
        dy = ce->dy;
        as_arc = ce->arc;
    }
    float b = blink_val(o, now);
    if (o->intro_step) {
        // A31: the eyes stay closed until 450 ms, then open like the second half of a blink
        uint32_t iel = now - o->intro_t0;
        float closed = iel < INTRO_EYES_MS ? 1.0f
                       : iel < INTRO_EYES_MS + UI_T_BLINK_OUT
                           ? 1.0f - ease((float)(iel - INTRO_EYES_MS) / UI_T_BLINK_OUT, E_OUT)
                           : 0.0f;
        b = b > closed ? b : closed;
    }
    if (!as_arc) {
        h = h + (BLINK_H - h) * b;
    }
    float gx, gy;
    gaze_now(o, now, &gx, &gy);
    lv_color_t col = o->eye;
    float ey = cy + (-8.0f + gy + dy) * k;
    for (int side = -1; side <= 1; side += 2) {
        float ex = cx + (side * 24.0f + gx) * k;
        if (as_arc) {
            // happy eyes: arcs ∩ r 11k (centre line), width 6k, 200 -> 340 deg
            arc(layer, ex, ey + 6.0f * k, 14.0f * k, 6.0f * k, 200, 340, col, LV_OPA_COVER, true);
        } else {
            capsule(layer, ex, ey, w * k, h * k, col);
        }
    }
    if (ce->smile && as_arc) {
        arc(layer, cx + gx * k * 0.5f, cy + 18.0f * k, 14.0f * k, 4.0f * k, 30, 150, col, LV_OPA_COVER, true);
    }
    float echo = tw_val(&o->echo, now);
    if (o->state == UI_ST_SPEAK && echo > 0.05f) {
        float mh = (4.0f + 12.0f * o->spk) * k * echo;
        capsule(layer, cx + gx * k * 0.5f, cy + 28.0f * k, 20.0f * k, mh, col);
    }
}

static void orb_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    orb_t *o = lv_obj_get_user_data(obj);
    if (!o) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    uint32_t now = lv_tick_get();
    geo_t g;
    geo_now(o, now, &g);
    lv_area_t co;
    lv_obj_get_coords(obj, &co);
    float cx = co.x1 + lv_area_get_width(&co) / 2.0f;
    float cy = co.y1 + lv_area_get_height(&co) / 2.0f;
    float half = lv_area_get_width(&co) / 2.0f - 1.0f;
    float k = g.core / 70.0f;
    pal_t p = pal_now(o, now);
    float bars = tw_val(&o->bars, now), arcs = tw_val(&o->arcs, now);
    float echo = tw_val(&o->echo, now), prog = tw_val(&o->prog, now);
    float L = o->mic, T = o->spk;

    // A23 error shake: core and face only, inside the box
    float dx = 0;
    uint32_t el = now - o->shake_t0;
    if (o->state == UI_ST_ERROR && el < 360) {
        float u = (float)el / 360.0f;
        dx = 6.0f * k * sinf(6.0f * PI_F * u) * (1.0f - u);
    }

    float core = g.core;
    if (st_rec(o->state)) {
        core += g.listen_dr * L * bars;
    }
    if (o->state == UI_ST_SPEAK) {
        core += g.speak_dr * T * echo;
    }
    if (o->state == UI_ST_THINK) {
        float ph = (float)((now - o->state_t0) % 1200) / 1200.0f;
        core -= g.think_dr * (0.5f - 0.5f * cosf(2.0f * PI_F * ph)) * arcs;
    }
    el = now - o->squish_t0;
    if (el < 200) {
        // A5 press-in: 70 -> 64 (90 ms ease_out) -> 70 (110 ms ease_out)
        float s = el < 90 ? ease((float)el / 90.0f, E_OUT) : 1.0f - ease((float)(el - 90) / 110.0f, E_OUT);
        core -= 6.0f * k * s;
    }
    uint32_t iel = now - o->intro_t0;
    bool intro = o->intro_step != 0;
    if (intro && iel < INTRO_CORE_MS) {
        core *= ease((float)iel / INTRO_CORE_MS, E_OUT);   // A31: the core forms
    }

    float h1 = g.h1, h2 = g.h2;
    if (st_idle_like(o->state)) {
        h1 += (g.h1b - g.h1) * breath(now);
        h2 += (g.h2b - g.h2) * breath(now + 1500);
    }
    lv_color_t h1c = p.h1;
    el = now - o->flash_t0;
    if (o->state == UI_ST_ANSWERED && el < 900) {
        // A14: halo1 flashes to the mode mid and back
        float f = el < 300 ? ease((float)el / 300.0f, E_OUT) : 1.0f - ease((float)(el - 300) / 600.0f, E_INOUT);
        h1c = lv_color_mix(ui_col(ui_accent(ui_mode_accent(o->mode))->mid), p.h1, (uint8_t)(f * 255.0f));
    }

    lv_color_t black = lv_color_black();
    lv_color_t h2c = p.h2;
    if (intro && iel < INTRO_HALO_MS) {
        uint8_t mix = (uint8_t)(255.0f * ease((float)iel / INTRO_HALO_MS, E_OUT));   // halos rise out of black
        h1c = lv_color_mix(h1c, black, mix);
        h2c = lv_color_mix(h2c, black, mix);
    }
    if (!lv_color_eq(h2c, black) && h2 > 0) {
        circle(layer, cx, cy, LV_MIN(h2, half), h2c, h2c, false);
    }
    if (!lv_color_eq(h1c, black) && h1 > 0) {
        circle(layer, cx, cy, LV_MIN(h1, half), h1c, h1c, false);
    }
    if (o->state == UI_ST_OFFLINE) {
        arc(layer, cx, cy, g.core, 3, 0, 360, p.stroke, LV_OPA_COVER, false);     // hollow orb
    } else {
        circle(layer, cx + dx, cy, core, p.top, p.bot, true);
    }

    if (bars > 0.01f && g.bars >= 1.0f) {
        int n = (int)(g.bars + 0.5f);
        float phase = (float)now * (0.09f / UI_TICK_MS);    // +0.09 rad per 33 ms tick
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = p.stroke;
        d.width = (int32_t)(g.bar_w + 0.5f);
        d.round_start = 1;
        d.round_end = 1;
        d.opa = LV_OPA_COVER;
        float rmax = half - g.bar_w / 2.0f;
        for (int i = 0; i < n; i++) {
            float a = 2.0f * PI_F * (float)i / (float)n - PI_F / 2.0f;
            float li = L * (0.6f + 0.4f * sinf(phase + (float)i * 2.0f * PI_F / 7.0f));
            float r0 = g.bar_in;
            float r1 = r0 + (g.bar_len0 + g.bar_len1 * li) * bars;
            if (r1 > rmax) {
                r1 = rmax;
            }
            float ca = cosf(a), sa = sinf(a);
            d.p1.x = (lv_value_precise_t)(cx + r0 * ca);
            d.p1.y = (lv_value_precise_t)(cy + r0 * sa);
            d.p2.x = (lv_value_precise_t)(cx + r1 * ca);
            d.p2.y = (lv_value_precise_t)(cy + r1 * sa);
            lv_draw_line(layer, &d);
        }
    }
    if (prog > 0.01f) {
        lv_opa_t opa = (lv_opa_t)(255.0f * prog);
        arc(layer, cx, cy, g.prog_r, g.prog_w, 0, 360, ui_col(UI_COL_SURFACE2), opa, false);
        int32_t pct = (int32_t)(tw_val(&o->pct, now) + 0.5f);
        if (pct > 0) {
            arc(layer, cx, cy, g.prog_r, g.prog_w, 270, 270 + pct * 360 / 100, p.stroke, opa, true);
        }
    }
    if (arcs > 0.01f) {
        float ts = (float)(now - o->state_t0) / 1000.0f;
        int32_t aa = (int32_t)fmodf(300.0f * ts, 360.0f);
        int32_t ab = 360 - (int32_t)fmodf(200.0f * ts, 360.0f);
        arc(layer, cx, cy, g.arc_a_r, g.arc_a_w, aa, aa + 70, p.stroke, (lv_opa_t)(255.0f * arcs), true);
        arc(layer, cx, cy, g.arc_b_r, g.arc_b_w, ab, ab + 40, p.stroke, (lv_opa_t)(140.0f * arcs), true);
    }
    if (echo > 0.01f) {
        arc(layer, cx, cy, LV_MIN(g.echo1_r + g.echo1_k * T, half), g.echo1_w, 0, 360, p.stroke,
            (lv_opa_t)(160.0f * echo), false);
        arc(layer, cx, cy, LV_MIN(g.echo2_r + g.echo2_k * T, half), g.echo2_w, 0, 360, p.stroke,
            (lv_opa_t)(90.0f * echo), false);
    }

    lv_color_t ink = ui_col(UI_COL_INK);
    switch (o->state) {
    case UI_ST_REC_CANCEL: glyph(layer, cx, cy, SYM_TIMES, &ui_28, ink); break;
    case UI_ST_ERROR: glyph(layer, cx + dx, cy, SYM_WARN, &ui_28, ink); break;
    case UI_ST_OFFLINE: glyph(layer, cx, cy, SYM_WIFI, &ui_28, ui_col(UI_COL_TEXT3)); break;
    case UI_ST_REC_LATCHED:
        glyph(layer, cx, o->preset == ORB_HERO ? cy + 30.0f * k : cy, SYM_STOP, &ui_20, ink);
        break;
    default: break;
    }
    if (face_visible(o, &g) && !(intro && iel < INTRO_FACE_MS)) {
        draw_face(o, layer, cx + dx, cy, k, now);
    }
}

static void face_area(const orb_t *o, lv_area_t *a)
{
    lv_area_t co;
    lv_obj_get_coords(o->obj, &co);
    int32_t cx = co.x1 + lv_area_get_width(&co) / 2, cy = co.y1 + lv_area_get_height(&co) / 2;
    float k = o->g_to.core / 70.0f;
    int32_t hx = (int32_t)(42.0f * k) + 2, top = (int32_t)(36.0f * k) + 2, bot = (int32_t)(38.0f * k) + 2;
    *a = (lv_area_t){ cx - hx, cy - top, cx + hx, cy + bot };
}

static void apply_geometry(orb_t *o, uint32_t now)
{
    geo_t g;
    geo_now(o, now, &g);
    int32_t box = (int32_t)(g.box + 0.5f);
    box += box & 1;     // even box: the centre stays on a pixel
    int32_t x = (int32_t)(g.cx + 0.5f) - box / 2, y = (int32_t)(g.cy + 0.5f) - box / 2;
    if (lv_obj_get_x(o->obj) != x || lv_obj_get_y(o->obj) != y) {
        lv_obj_set_pos(o->obj, x, y);
    }
    if (lv_obj_get_width(o->obj) != box || lv_obj_get_height(o->obj) != box) {
        lv_obj_set_size(o->obj, box, box);
    }
}

static void schedule_blink(orb_t *o, uint32_t now)
{
    o->next_blink = now + lv_rand(2500, 6000);
}

static void start_blink(orb_t *o, uint32_t now)
{
    o->blinking = true;
    o->blink_t0 = now;
}

static void orb_tick(lv_timer_t *t)
{
    orb_t *o = lv_timer_get_user_data(t);
    uint32_t now = lv_tick_get();
    bool cont = st_continuous(o->state);

    if (cont) {
        // Mic L = clamp((dBFS + 50) / 40); the meters report (dBFS + 60) * 2. Attack 0.6, release 0.12.
        float m = ((float)audio_in_level() - 20.0f) / 80.0f;
        float s = ((float)audio_out_level() - 20.0f) / 80.0f;
        m = m < 0 ? 0 : m > 1 ? 1 : m;
        s = s < 0 ? 0 : s > 1 ? 1 : s;
        o->mic += (m > o->mic ? 0.6f : 0.12f) * (m - o->mic);
        // two ticks (66 ms) of delay for the I2S latency of the TTS meter
        float sd = o->spk_d2;
        o->spk_d2 = o->spk_d1;
        o->spk_d1 = s;
        o->spk += (sd > o->spk ? 0.6f : 0.12f) * (sd - o->spk);
    } else {
        o->mic = 0;
        o->spk = 0;
    }

    bool morph = tw_busy(&o->pm, now);
    apply_geometry(o, now);

    if (o->ovr_on && o->ovr_until && (int32_t)(o->ovr_until - now) <= 0) {
        o->ovr_on = false;
    }
    orb_expr_t eff = expr_eff(o, now);
    if (eff != o->expr_cur) {
        o->expr_prev = o->expr_cur;
        o->expr_cur = eff;
        tw_set(&o->expr_t, 0, now);
        tw_go(&o->expr_t, 1, 160, E_INOUT, now);
    }

    geo_t g;
    geo_now(o, now, &g);
    bool face = face_visible(o, &g);
    if (face && st_idle_like(o->state)) {
        if (!o->blinking && (int32_t)(now - o->next_blink) >= 0) {
            start_blink(o, now);
            o->blink_again = lv_rand(0, 99) < 20;      // 20 % double blink
            schedule_blink(o, now);
        }
        if (tw_val(&o->gz_w, now) < 0.01f && (int32_t)(now - o->next_glance) >= 0) {
            float gx, gy;
            if (o->mode == 1) {
                // translator: alternate glances toward chip A (left) and chip B (right)
                o->glance_flip ^= 1;
                gx = o->glance_flip ? -8.0f : 8.0f;
                gy = 5.0f;
            } else {
                gx = (float)lv_rand(0, 12) - 6.0f;
                gy = (float)lv_rand(0, 8) - 4.0f;
            }
            tw_go(&o->gz_x, gx, 260, E_INOUT, now);
            tw_go(&o->gz_y, gy, 260, E_INOUT, now);
            tw_go(&o->gz_w, 1, 260, E_INOUT, now);
            o->gz_until = now + 260 + lv_rand(800, 2000);
            o->next_glance = now + lv_rand(6000, 12000);
        }
    }
    if (o->blinking) {
        uint32_t el = now - o->blink_t0;
        if (el >= UI_T_BLINK_IN && !lv_color_eq(o->eye, o->eye_next)) {
            o->eye = o->eye_next;               // colour swap hidden by the closed lids
        }
        if (el >= UI_T_BLINK_IN + UI_T_BLINK_HOLD + UI_T_BLINK_OUT) {
            o->blinking = false;
            if (o->blink_again) {
                o->blink_again = false;
                o->next_blink = now + 120;
            }
        }
    } else if (!lv_color_eq(o->eye, o->eye_next)) {
        if (face) {
            start_blink(o, now);
        } else {
            o->eye = o->eye_next;
        }
    }
    if (tw_val(&o->gz_w, now) > 0.99f && o->gz_w.to > 0.5f && (int32_t)(now - o->gz_until) >= 0) {
        tw_go(&o->gz_w, 0, 260, E_INOUT, now);
    }
    if (o->intro_step) {
        // A31 glance: left, right, back to the centre
        uint32_t iel = now - o->intro_t0;
        if (o->intro_step == 1 && iel >= INTRO_LEFT_MS) {
            tw_go(&o->gz_x, -6.0f, 260, E_INOUT, now);
            tw_go(&o->gz_y, 0, 260, E_INOUT, now);
            tw_go(&o->gz_w, 1, 260, E_INOUT, now);
            o->gz_until = now + INTRO_END_MS;   // the sequence returns the gaze itself
            o->intro_step = 2;
        } else if (o->intro_step == 2 && iel >= INTRO_RIGHT_MS) {
            tw_go(&o->gz_x, 6.0f, 260, E_INOUT, now);
            o->intro_step = 3;
        } else if (o->intro_step == 3 && iel >= INTRO_BACK_MS) {
            tw_go(&o->gz_w, 0, 260, E_INOUT, now);
            o->intro_step = 4;
        } else if (o->intro_step == 4 && iel >= INTRO_END_MS) {
            o->intro_step = 0;
        }
    }

    bool fx = (now - o->squish_t0 < 200) || (o->state == UI_ST_ERROR && now - o->shake_t0 < 360) ||
              (o->state == UI_ST_ANSWERED && now - o->flash_t0 < 900) ||
              (o->intro_step && now - o->intro_t0 < INTRO_EYES_MS + UI_T_BLINK_OUT);
    bool tweens = morph || tw_busy(&o->col, now) || tw_busy(&o->bars, now) || tw_busy(&o->arcs, now) ||
                  tw_busy(&o->echo, now) || tw_busy(&o->prog, now) || tw_busy(&o->pct, now);
    // a held glance does not move: only blinks and running tweens need face-box redraws
    bool face_motion = face && (o->blinking || tw_busy(&o->expr_t, now) || tw_busy(&o->gz_w, now) ||
                                tw_busy(&o->gz_x, now) || tw_busy(&o->gz_y, now));

    if (cont || fx || tweens) {
        lv_obj_invalidate(o->obj);
        lv_timer_set_period(t, TICK_FAST);
        o->was_active = true;
        return;
    }
    if (o->was_active) {
        lv_obj_invalidate(o->obj);      // one last frame at the end values
        o->was_active = false;
    }
    if (face_motion) {
        lv_area_t a;
        face_area(o, &a);
        lv_obj_invalidate_area(o->obj, &a);
        lv_timer_set_period(t, TICK_FAST);
        return;
    }
    if (st_idle_like(o->state)) {
        // A1: integer halo radii only change 3-5 times per second; redraw only then
        int16_t r1 = (int16_t)(g.h1 + (g.h1b - g.h1) * breath(now) + 0.5f);
        int16_t r2 = (int16_t)(g.h2 + (g.h2b - g.h2) * breath(now + 1500) + 0.5f);
        if (r1 != o->last_h1 || r2 != o->last_h2) {
            o->last_h1 = r1;
            o->last_h2 = r2;
            lv_obj_invalidate(o->obj);
        }
        lv_timer_set_period(t, TICK_IDLE);
        return;
    }
    if (o->intro_step) {
        lv_timer_set_period(t, TICK_IDLE);  // finish the glance sequence
        return;
    }
    lv_timer_pause(t);
}

static void orb_kick(orb_t *o)
{
    lv_timer_set_period(o->timer, TICK_FAST);
    lv_timer_resume(o->timer);
    lv_timer_ready(o->timer);
    lv_obj_invalidate(o->obj);
}

static void orb_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    orb_t *o = lv_obj_get_user_data(obj);
    lv_event_code_t code = lv_event_get_code(e);
    if (!o) {
        return;
    }
    if (code == LV_EVENT_HIT_TEST) {
        lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
        lv_point_t c;
        ui_orb_center(obj, &c);
        int32_t dx = info->point->x - c.x, dy = info->point->y - c.y;
        int32_t r = ui_orb_hit_radius(obj);
        info->res = dx * dx + dy * dy <= r * r;
    } else if (code == LV_EVENT_DELETE) {
        lv_timer_delete(o->timer);
        lv_obj_set_user_data(obj, NULL);
        lv_free(o);
    }
}

lv_obj_t *ui_orb_create(lv_obj_t *parent, orb_preset_t p)
{
    if ((unsigned)p >= ORB__N) {
        p = ORB_HERO;
    }
    lv_obj_t *obj = lv_obj_create(parent);
    ui_theme_plain(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST | LV_OBJ_FLAG_PRESS_LOCK);

    orb_t *o = lv_malloc_zeroed(sizeof(orb_t));
    LV_ASSERT_MALLOC(o);
    uint32_t now = lv_tick_get();
    o->obj = obj;
    o->preset = p;
    o->g_from = o->g_to = s_geo[p];
    tw_set(&o->pm, 1, now);
    o->state = UI_ST_IDLE;
    o->face_on = true;
    o->target_side = -1;
    o->pal_from = o->pal_to = pal_for(UI_ST_IDLE, 0);
    tw_set(&o->col, 1, now);
    o->eye = o->eye_next = eye_for(UI_ST_IDLE);
    tw_set(&o->bars, 0, now);
    tw_set(&o->arcs, 0, now);
    tw_set(&o->echo, 0, now);
    tw_set(&o->prog, 0, now);
    tw_set(&o->pct, 0, now);
    tw_set(&o->expr_t, 1, now);
    tw_set(&o->gz_x, 0, now);
    tw_set(&o->gz_y, 0, now);
    tw_set(&o->gz_w, 0, now);
    o->squish_t0 = o->shake_t0 = o->flash_t0 = now - 10000;
    schedule_blink(o, now);
    o->next_glance = now + lv_rand(6000, 12000);
    lv_obj_set_user_data(obj, o);
    apply_geometry(o, now);

    lv_obj_add_event_cb(obj, orb_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, orb_event_cb, LV_EVENT_HIT_TEST, NULL);
    lv_obj_add_event_cb(obj, orb_event_cb, LV_EVENT_DELETE, NULL);
    o->timer = lv_timer_create(orb_tick, TICK_IDLE, o);
    return obj;
}

static orb_t *get(lv_obj_t *orb)
{
    return orb ? lv_obj_get_user_data(orb) : NULL;
}

void ui_orb_set_preset(lv_obj_t *orb, orb_preset_t p, bool anim)
{
    orb_t *o = get(orb);
    if (!o || (unsigned)p >= ORB__N) {
        return;
    }
    uint32_t now = lv_tick_get();
    geo_now(o, now, &o->g_from);
    o->g_to = s_geo[p];
    o->preset = p;
    tw_set(&o->pm, 0, now);
    if (anim) {
        tw_go(&o->pm, 1, 320, E_INOUT, now);
    } else {
        tw_set(&o->pm, 1, now);
    }
    apply_geometry(o, now);
    orb_kick(o);
}

orb_preset_t ui_orb_get_preset(lv_obj_t *orb)
{
    orb_t *o = get(orb);
    return o ? o->preset : ORB_HERO;
}

void ui_orb_set_state(lv_obj_t *orb, int32_t st)
{
    orb_t *o = get(orb);
    if (!o || st == o->state || (unsigned)st >= UI_ST__N) {
        return;
    }
    uint32_t now = lv_tick_get();
    int32_t prev = o->state;
    o->pal_from = pal_now(o, now);
    o->state = st;
    o->state_t0 = now;
    o->pal_to = pal_for(st, o->mode);
    tw_set(&o->col, 0, now);
    tw_go(&o->col, 1, morph_ms(st), E_INOUT, now);
    o->eye_next = eye_for(st);

    bool rec = st_rec(st);
    tw_go(&o->bars, rec ? 1 : 0, rec ? 180 : 160, rec ? E_OUT : E_IN, now);
    tw_go(&o->prog, st == UI_ST_UPLOAD ? 1 : 0, 150, E_OUT, now);
    tw_go(&o->arcs, st == UI_ST_THINK ? 1 : 0, 150, st == UI_ST_THINK ? E_OUT : E_IN, now);
    // echo rings: in with the 250 ms speak morph (A11), out 200 ms ease_in (A13)
    tw_go(&o->echo, st == UI_ST_SPEAK ? 1 : 0, st == UI_ST_SPEAK ? 250 : 200, st == UI_ST_SPEAK ? E_OUT : E_IN, now);
    if (st == UI_ST_UPLOAD && prev != UI_ST_UPLOAD) {
        tw_set(&o->pct, 0, now);
    }
    if (rec && !st_rec(prev)) {
        o->squish_t0 = now;
    }
    if (st == UI_ST_ERROR) {
        o->shake_t0 = now;
    }
    if (st == UI_ST_ANSWERED) {
        o->flash_t0 = now;
    }
    if (o->ovr_on && o->ovr_until == 0) {
        o->ovr_on = false;
    }
    orb_kick(o);
}

void ui_orb_set_mode(lv_obj_t *orb, int mode)
{
    orb_t *o = get(orb);
    if (!o || o->mode == mode) {
        return;
    }
    uint32_t now = lv_tick_get();
    o->pal_from = pal_now(o, now);
    o->mode = mode;
    o->pal_to = pal_for(o->state, mode);
    tw_set(&o->col, 0, now);
    tw_go(&o->col, 1, UI_T_MORPH, E_INOUT, now);
    orb_kick(o);
}

void ui_orb_set_expr(lv_obj_t *orb, orb_expr_t e, uint32_t hold_ms)
{
    orb_t *o = get(orb);
    if (!o || (unsigned)e >= EXPR__N) {
        return;
    }
    o->expr_ovr = e;
    o->ovr_on = true;
    o->ovr_until = hold_ms ? (lv_tick_get() + hold_ms) | 1u : 0;
    orb_kick(o);
}

void ui_orb_set_mood(lv_obj_t *orb, int32_t mood)
{
    static const orb_expr_t map[] = { EXPR_NEUTRAL, EXPR_HAPPY, EXPR_SOFT, EXPR_WIDE, EXPR_NEUTRAL };
    if (mood >= 0 && mood < (int32_t)(sizeof(map) / sizeof(map[0]))) {
        ui_orb_set_expr(orb, map[mood], 1500);
    }
}

void ui_orb_look_at(lv_obj_t *orb, lv_point_t p, uint32_t hold_ms)
{
    orb_t *o = get(orb);
    if (!o) {
        return;
    }
    lv_point_t c;
    ui_orb_center(orb, &c);
    float dx = (float)(p.x - c.x), dy = (float)(p.y - c.y);
    float len = sqrtf(dx * dx + dy * dy);
    float gx = len > 1 ? 8.0f * dx / len : 0, gy = len > 1 ? 8.0f * dy / len : 0;
    gy = gy > 6 ? 6 : gy < -6 ? -6 : gy;
    uint32_t now = lv_tick_get();
    tw_go(&o->gz_x, gx, 260, E_INOUT, now);
    tw_go(&o->gz_y, gy, 260, E_INOUT, now);
    tw_go(&o->gz_w, 1, 260, E_INOUT, now);
    o->gz_until = now + 260 + hold_ms;
    orb_kick(o);
}

void ui_orb_set_target_side(lv_obj_t *orb, int side)
{
    orb_t *o = get(orb);
    if (o && o->target_side != side) {
        o->target_side = side;
        orb_kick(o);
    }
}

void ui_orb_set_face(lv_obj_t *orb, bool on)
{
    orb_t *o = get(orb);
    if (o && o->face_on != on) {
        o->face_on = on;
        orb_kick(o);
    }
}

void ui_orb_set_upload(lv_obj_t *orb, int32_t pct)
{
    orb_t *o = get(orb);
    if (!o) {
        return;
    }
    pct = LV_CLAMP(0, pct, 100);
    tw_go(&o->pct, (float)pct, 200, E_OUT, lv_tick_get());
    orb_kick(o);
}

void ui_orb_intro(lv_obj_t *orb)
{
    orb_t *o = get(orb);
    if (o) {
        uint32_t now = lv_tick_get();
        o->intro_t0 = now;
        o->intro_step = 1;
        o->blinking = false;
        o->next_blink = now + INTRO_END_MS + lv_rand(1000, 3000);
        orb_kick(o);
    }
}

void ui_orb_pulse(lv_obj_t *orb)
{
    orb_t *o = get(orb);
    if (o) {
        o->squish_t0 = lv_tick_get();
        orb_kick(o);
    }
}

void ui_orb_center(lv_obj_t *orb, lv_point_t *c)
{
    lv_area_t co;
    lv_obj_get_coords(orb, &co);
    c->x = co.x1 + lv_area_get_width(&co) / 2;
    c->y = co.y1 + lv_area_get_height(&co) / 2;
}

int32_t ui_orb_hit_radius(lv_obj_t *orb)
{
    orb_t *o = get(orb);
    return o ? (int32_t)o->g_to.hit : 0;
}

int32_t ui_orb_cancel_radius(lv_obj_t *orb)
{
    orb_t *o = get(orb);
    return o ? (int32_t)o->g_to.cancel : 0;
}

const char *ui_orb_expr_name(orb_expr_t e)
{
    return (unsigned)e < EXPR__N ? s_expr_names[e] : NULL;
}
