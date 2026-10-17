#include "ui_rim.h"
#include "ui_geom.h"
#include "ui_theme.h"

// Band r 164-168 (outer radius 168 = UI_R, width 4); POWER uses r 162-168. lv_draw_arc's radius is the
// outer edge and the width grows inwards.
#define RIM_R           UI_R
#define RIM_W           4
#define RIM_PWR_W       6
#define THUMB_FROM      320     // thumb lives in 320 -> 40 deg (right side, through 3 o'clock)
#define THUMB_RANGE     80
#define THUMB_MIN       12
#define SEG_DEG         30      // invalidation pieces: small boxes, at most 12 per full ring
#define BUSY_SPAN       40
#define BUSY_PERIOD_MS  1200
#define BUSY_TICK_MS    33

static lv_obj_t *s_rim;

static int32_t s_rec_deg = -1;
static bool s_rec_warn;

static bool s_busy;
static int32_t s_busy_a;        // comet head angle
static lv_timer_t *s_busy_timer;

static int32_t s_pwr = -1;      // permille
static int32_t s_pwr_deg = -1;

static lv_obj_t *s_scroller;
static int32_t s_th_a0 = -1, s_th_a1 = -1;   // thumb span in degrees, a0 in [320, 400)
static lv_opa_t s_th_opa;
static lv_timer_t *s_th_idle;

// Invalidates the ring band between angles a0 and a1 (a1 > a0, may exceed 360) in SEG_DEG pieces, so a long
// sweep never turns into the 113k px bounding box of the whole ring.
static void inval_span(int32_t a0, int32_t a1, int32_t w)
{
    if (!s_rim || a1 <= a0) {
        return;
    }
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
        lv_draw_arc_get_area(UI_CX, UI_CY, RIM_R, s, e, w, true, &ar);
        lv_obj_invalidate_area(s_rim, &ar);
        a0 += e - s;
    }
}

static void draw_arc(lv_layer_t *layer, int32_t a0, int32_t a1, int32_t w, lv_color_t c, lv_opa_t opa)
{
    if (a1 <= a0 || opa == 0) {
        return;
    }
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = UI_CX;
    d.center.y = UI_CY;
    d.radius = RIM_R;
    d.width = w;
    d.color = c;
    d.opa = opa;
    d.rounded = 1;
    d.start_angle = a0 % 360;
    d.end_angle = (a1 - a0 >= 360) ? d.start_angle + 360 : a1 % 360;
    lv_draw_arc(layer, &d);
}

static void rim_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_th_a0 >= 0 && s_th_opa > 0) {
        draw_arc(layer, s_th_a0, s_th_a1, RIM_W, ui_col(UI_COL_TEXT3), s_th_opa);
    }
    if (s_busy) {
        // comet: bright head, dimmer body and tail (three short arcs, no gradients)
        lv_color_t c = ui_col(UI_COL_TEXT2);
        int32_t h = s_busy_a + 360;
        draw_arc(layer, h - 14, h, RIM_W, c, LV_OPA_COVER);
        draw_arc(layer, h - 27, h - 14, RIM_W, c, 150);
        draw_arc(layer, h - BUSY_SPAN, h - 27, RIM_W, c, 70);
    }
    if (s_rec_deg > 0) {
        const ui_accent_t *a = ui_accent(s_rec_warn ? UI_ACC_WARNING : UI_ACC_LISTEN);
        draw_arc(layer, 270, 270 + s_rec_deg, RIM_W, ui_col(a->top), LV_OPA_COVER);
    }
    if (s_pwr_deg > 0) {
        draw_arc(layer, 270, 270 + s_pwr_deg, RIM_PWR_W, ui_col(ui_accent(UI_ACC_ERROR)->top), LV_OPA_COVER);
    }
}

void ui_rim_init(void)
{
    if (s_rim) {
        return;
    }
    s_rim = lv_obj_create(lv_layer_top());
    ui_theme_plain(s_rim);
    lv_obj_set_pos(s_rim, UI_CX - RIM_R, UI_CY - RIM_R);
    lv_obj_set_size(s_rim, 2 * RIM_R, 2 * RIM_R);
    lv_obj_remove_flag(s_rim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_name_static(s_rim, "rim");
    lv_obj_add_event_cb(s_rim, rim_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}

lv_obj_t *ui_rim_obj(void) { return s_rim; }

// ---- SCROLL

static void thumb_set(int32_t a0, int32_t a1)
{
    if (a0 == s_th_a0 && a1 == s_th_a1) {
        return;
    }
    if (s_th_a0 >= 0) {
        inval_span(s_th_a0, s_th_a1, RIM_W);
    }
    s_th_a0 = a0;
    s_th_a1 = a1;
    if (a0 >= 0) {
        inval_span(a0, a1, RIM_W);
    }
}

static void thumb_opa_exec(void *var, int32_t v)
{
    (void)var;
    s_th_opa = (lv_opa_t)v;
    if (s_th_a0 >= 0) {
        inval_span(s_th_a0, s_th_a1, RIM_W);
    }
}

static void thumb_fade_out(lv_timer_t *t)
{
    (void)t;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_th_opa);
    lv_anim_set_exec_cb(&a, thumb_opa_exec);
    lv_anim_set_values(&a, s_th_opa, 0);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

static void thumb_update(void)
{
    if (!s_scroller) {
        thumb_set(-1, -1);
        return;
    }
    int32_t view = lv_obj_get_height(s_scroller);
    int32_t top = lv_obj_get_scroll_top(s_scroller);
    int32_t bottom = lv_obj_get_scroll_bottom(s_scroller);
    int32_t content = view + top + bottom;
    if (content <= view || view <= 0) {
        thumb_set(-1, -1);
        return;
    }
    int32_t len = THUMB_RANGE * view / content;
    if (len < THUMB_MIN) {
        len = THUMB_MIN;
    }
    int32_t travel = top + bottom;
    int32_t pos = travel > 0 ? (THUMB_RANGE - len) * LV_CLAMP(0, top, travel) / travel : 0;
    thumb_set(THUMB_FROM + pos, THUMB_FROM + pos + len);
}

static void scroll_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        if (lv_event_get_current_target(e) == s_scroller) {
            s_scroller = NULL;
            thumb_set(-1, -1);
        }
        return;
    }
    if (lv_event_get_current_target(e) != s_scroller) {
        return;
    }
    lv_anim_delete(&s_th_opa, thumb_opa_exec);
    if (s_th_opa != LV_OPA_COVER) {
        s_th_opa = LV_OPA_COVER;
        if (s_th_a0 >= 0) {
            inval_span(s_th_a0, s_th_a1, RIM_W);
        }
    }
    thumb_update();
    if (!s_th_idle) {
        s_th_idle = lv_timer_create(thumb_fade_out, 600, NULL);
        lv_timer_set_repeat_count(s_th_idle, 1);
        lv_timer_set_auto_delete(s_th_idle, false);
    }
    lv_timer_reset(s_th_idle);
    lv_timer_set_repeat_count(s_th_idle, 1);
    lv_timer_resume(s_th_idle);
}

lv_obj_t *ui_rim_scroller(void) { return s_scroller; }

void ui_rim_attach_scroll(lv_obj_t *scroller)
{
    if (s_scroller == scroller) {
        return;
    }
    if (s_scroller) {
        lv_obj_remove_event_cb(s_scroller, scroll_cb);
    }
    s_scroller = scroller;
    lv_anim_delete(&s_th_opa, thumb_opa_exec);
    s_th_opa = 0;
    thumb_set(-1, -1);
    if (scroller) {
        lv_obj_add_event_cb(scroller, scroll_cb, LV_EVENT_SCROLL, NULL);
        lv_obj_add_event_cb(scroller, scroll_cb, LV_EVENT_DELETE, NULL);
    }
}

// ---- RECORD

void ui_rim_record(int32_t ms, bool warn)
{
    int32_t deg = ms < 0 ? -1 : (ms >= 30000 ? 360 : ms * 360 / 30000);
    if (deg == s_rec_deg && warn == s_rec_warn) {
        return;
    }
    if (warn != s_rec_warn || deg < s_rec_deg) {
        // colour change or shrink: repaint the old sweep (pieces), then the new one
        if (s_rec_deg > 0) {
            inval_span(270, 270 + s_rec_deg, RIM_W);
        }
        if (deg > 0) {
            inval_span(270, 270 + deg, RIM_W);
        }
    } else if (deg > 0) {
        // growth: only the delta (about 1 deg per 100 ms tick)
        inval_span(270 + (s_rec_deg > 0 ? s_rec_deg : 0) - 1, 270 + deg, RIM_W);
    }
    s_rec_deg = deg;
    s_rec_warn = warn;
}

// ---- BUSY

static void busy_tick(lv_timer_t *t)
{
    (void)t;
    int32_t old = s_busy_a;
    s_busy_a = (s_busy_a + 360 * BUSY_TICK_MS / BUSY_PERIOD_MS) % 360;
    // the union of the old and new comet is one short span
    int32_t a0 = old + 360 - BUSY_SPAN;
    int32_t a1 = s_busy_a + 360;
    if (a1 < old + 360) {
        a1 += 360;
    }
    inval_span(a0, a1, RIM_W);
}

void ui_rim_busy(bool on)
{
    if (on == s_busy) {
        return;
    }
    s_busy = on;
    inval_span(s_busy_a + 360 - BUSY_SPAN, s_busy_a + 360, RIM_W);
    if (on) {
        if (!s_busy_timer) {
            s_busy_timer = lv_timer_create(busy_tick, BUSY_TICK_MS, NULL);
        }
        lv_timer_resume(s_busy_timer);
    } else if (s_busy_timer) {
        lv_timer_pause(s_busy_timer);
    }
}

// ---- POWER

void ui_rim_power(int32_t permille)
{
    int32_t deg = permille < 0 ? -1 : (permille >= 1000 ? 360 : permille * 360 / 1000);
    s_pwr = permille < 0 ? -1 : LV_MIN(permille, 1000);
    if (deg == s_pwr_deg) {
        return;
    }
    int32_t lo = LV_MIN(deg, s_pwr_deg), hi = LV_MAX(deg, s_pwr_deg);
    inval_span(270 + (lo > 0 ? lo : 0) - 1, 270 + hi, RIM_PWR_W);
    s_pwr_deg = deg;
}

int32_t ui_rim_power_get(void) { return s_pwr; }
