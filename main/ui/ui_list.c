// T_LIST rows and the curved list viewport (docs/ui.md 3.7, A24, A25).
#include "ui_list.h"
#include "ui_geom.h"
#include "ui_internal.h"
#include "ui_rim.h"
#include "ui_str.h"
#include "ui_sym.h"

#include <string.h>

#define ROW_MAGIC   0x524F5721u     // "ROW!"
#define FADE_IN     79              // |row centre - 180| up to which the row is fully opaque
#define FADE_OUT    119             // ... and from which it is invisible
#define FADE_STEP   8               // write opa only when it moved this much (or hit 0 / 255)
#define SECTION_H   28

typedef enum { K_ROW, K_INFO, K_SECTION } row_kind_t;

typedef struct {
    uint32_t magic;
    uint8_t kind, right;
    bool enabled, on, locked;
    int8_t rssi;
    lv_opa_t opa;
    lv_obj_t *icon, *title, *sub, *rlabel;
    void *ud;
    lv_event_cb_t cb;
} row_t;

static void row_delete_cb(lv_event_t *e);

// Rows are recognised by their delete callback, never by dereferencing user data: other objects keep integers
// there (chat wrappers hold their entry id)
static row_t *meta(lv_obj_t *obj)
{
    if (!obj) {
        return NULL;
    }
    uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if (d && lv_event_dsc_get_cb(d) == row_delete_cb) {
            row_t *m = lv_obj_get_user_data(obj);
            return m && m->magic == ROW_MAGIC ? m : NULL;
        }
    }
    return NULL;
}

bool ui_list_is_row(lv_obj_t *obj)
{
    return meta(obj) != NULL;
}

void *ui_row_ud(lv_obj_t *row)
{
    row_t *m = meta(row);
    return m ? m->ud : NULL;
}

// ---- Curved-edge fade

static void fade_apply(lv_obj_t *list)
{
    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(list, (int32_t)i);
        row_t *m = meta(c);
        if (!m) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        int32_t d = LV_ABS((a.y1 + a.y2) / 2 - UI_CY);
        int32_t opa = d <= FADE_IN ? 255 : d >= FADE_OUT ? 0 : 255 * (FADE_OUT - d) / (FADE_OUT - FADE_IN);
        if (opa != m->opa && (LV_ABS(opa - (int32_t)m->opa) >= FADE_STEP || opa == 0 || opa == 255)) {
            m->opa = (lv_opa_t)opa;
            lv_obj_set_style_opa(c, m->opa, 0);
        }
        if (m->kind == K_ROW) {
            lv_obj_set_flag(c, LV_OBJ_FLAG_CLICKABLE, m->enabled && m->cb && m->opa >= 128);
        }
    }
}

static void list_cb(lv_event_t *e)
{
    fade_apply(lv_event_get_current_target(e));
}

void ui_list_refresh(lv_obj_t *list)
{
    if (!list) {
        return;
    }
    lv_obj_update_layout(list);
    fade_apply(list);
}

// A24: the list rises 16 px and fades 128 -> 255 over 180 ms ease_out (style opa and translate: no layer)
static void enter_exec(void *var, int32_t v)
{
    lv_obj_set_style_translate_y(var, 16 - 16 * v / 255, 0);
    lv_obj_set_style_opa(var, (lv_opa_t)(128 + 127 * v / 255), 0);
}

static void enter_done(lv_anim_t *a)
{
    lv_obj_remove_local_style_prop(a->var, LV_STYLE_TRANSLATE_Y, 0);
    lv_obj_remove_local_style_prop(a->var, LV_STYLE_OPA, 0);
    fade_apply(a->var);
}

lv_obj_t *ui_list_screen(const char *name, const char *title, lv_obj_t **list)
{
    lv_obj_t *scr = ui_chrome_screen();
    lv_obj_set_name_static(scr, name);
    lv_obj_t *l = lv_obj_create(scr);
    ui_theme_plain(l);
    lv_obj_set_name_static(l, "list");
    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(l, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(l, UI_LIST_X, UI_LIST_Y);
    lv_obj_set_size(l, UI_LIST_W, UI_LIST_H);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 6, 0);
    lv_obj_set_style_pad_top(l, 4, 0);
    // the last row can scroll up to the centre line
    lv_obj_set_style_pad_bottom(l, 134, 0);
    lv_obj_add_event_cb(l, list_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(l, list_cb, LV_EVENT_LAYOUT_CHANGED, NULL);
    lv_obj_add_event_cb(l, list_cb, LV_EVENT_SCROLL_END, NULL);

    ui_chrome_title(scr, title);
    ui_chrome_swipe_back(scr);
    ui_rim_attach_scroll(l);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, l);
    lv_anim_set_exec_cb(&a, enter_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, enter_done);
    lv_anim_start(&a);
    *list = l;
    return scr;
}

// ---- Rows

static void row_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    row_t *m = meta(obj);
    if (m) {
        m->magic = 0;
        lv_obj_set_user_data(obj, NULL);
        lv_free(m);
    }
}

static void row_click_cb(lv_event_t *e)
{
    row_t *m = meta(lv_event_get_current_target(e));
    if (m && m->cb && m->enabled) {
        m->cb(e);
    }
}

// Signal bars (3.7): 4 bars 3 px wide, 2 px apart, heights 5/8/11/14 at the right slot, bottom y+40; colour by
// RSSI (>= -60 success, -60..-75 warning, < -75 error), unlit bars text-3
static void wifi_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    row_t *m = meta(obj);
    if (!m || m->right != UI_ROW_WIFI) {
        return;
    }
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    int lit = m->rssi >= -60 ? 4 : m->rssi >= -67 ? 3 : m->rssi >= -75 ? 2 : 1;
    ui_acc_t acc = m->rssi >= -60 ? UI_ACC_SUCCESS : m->rssi >= -75 ? UI_ACC_WARNING : UI_ACC_ERROR;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 1;
    d.bg_opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    for (int i = 0; i < 4; i++) {
        lv_area_t b;
        b.x1 = c.x1 + 217 + i * 5;
        b.x2 = b.x1 + 2;
        b.y2 = c.y1 + 40;
        b.y1 = b.y2 - (5 + 3 * i) + 1;
        d.bg_color = ui_col(i < lit ? ui_accent(acc)->top : UI_COL_TEXT3);
        lv_draw_rect(lv_event_get_layer(e), &d, &b);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t col, int32_t x, int32_t y, int32_t w,
                       lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, ui_col(col), 0);
    lv_obj_set_style_text_align(l, align, 0);
    // fixed one-line box: DOTS only truncates when the height is fixed too
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));
    lv_label_set_text(l, "");
    return l;
}

static row_t *row_base(lv_obj_t *list, lv_obj_t **out, const char *name, row_kind_t kind, int32_t h)
{
    lv_obj_t *r = lv_obj_create(list);
    ui_theme_plain(r);
    if (name) {
        lv_obj_set_name(r, name);
    }
    lv_obj_set_size(r, UI_LIST_W, h);
    row_t *m = lv_malloc_zeroed(sizeof(row_t));
    LV_ASSERT_MALLOC(m);
    m->magic = ROW_MAGIC;
    m->kind = (uint8_t)kind;
    m->enabled = true;
    m->opa = LV_OPA_COVER;
    lv_obj_set_user_data(r, m);
    lv_obj_add_event_cb(r, row_delete_cb, LV_EVENT_DELETE, NULL);
    *out = r;
    return m;
}

lv_obj_t *ui_list_row(lv_obj_t *list, const char *name, const char *icon, const char *title, const char *sub,
                      ui_row_right_t right, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r;
    row_t *m = row_base(list, &r, name, K_ROW, UI_ROW_H);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    m->right = (uint8_t)right;
    m->cb = cb;
    m->ud = ud;
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_click_cb, LV_EVENT_CLICKED, ud);
    } else {
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }
    ui_theme_chain_ver(r);      // a drag that starts on a row scrolls the list

    // icon centre (28,32), title x 52 y 10 (w <= 164, 140 next to the Wi-Fi bars), subtitle x 52 y 38 (w <= 184)
    m->icon = label(r, &ui_20, UI_COL_TEXT2, 8, 19, 40, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(m->icon, icon ? icon : "");
    int32_t tw = right == UI_ROW_WIFI ? 132 : icon ? 164 : right == UI_ROW_NONE ? 212 : 180;
    m->title = label(r, &ui_20, UI_COL_TEXT1, icon ? 52 : 20, sub ? 10 : 19, tw, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(m->title, title ? title : "");
    m->sub = label(r, &ui_16, UI_COL_TEXT2, icon ? 52 : 20, 38, icon ? 184 : 196, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(m->sub, sub ? sub : "");
    lv_obj_set_flag(m->sub, LV_OBJ_FLAG_HIDDEN, !sub);

    // right slot centred on (226,32)
    switch (right) {
    case UI_ROW_CHEVRON:
        m->rlabel = label(r, &ui_16, UI_COL_TEXT3, 206, 22, 40, LV_TEXT_ALIGN_CENTER);
        lv_label_set_text_static(m->rlabel, SYM_RIGHT);
        break;
    case UI_ROW_CHECK:
        m->rlabel = label(r, &ui_20, ui_accent(ui_mode_accent(ui_get(g_s_mode)))->top, 206, 19, 40,
                          LV_TEXT_ALIGN_CENTER);
        break;
    case UI_ROW_TOGGLE:
        m->rlabel = label(r, &ui_16, UI_COL_TEXT3, 198, 22, 48, LV_TEXT_ALIGN_CENTER);
        break;
    case UI_ROW_WIFI:
        m->rlabel = label(r, &ui_16, UI_COL_TEXT3, 192, 22, 20, LV_TEXT_ALIGN_CENTER);
        lv_obj_add_event_cb(r, wifi_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
        break;
    default:
        break;
    }
    return r;
}

lv_obj_t *ui_list_info(lv_obj_t *list, const char *name, const char *caption, const char *value)
{
    lv_obj_t *r;
    row_t *m = row_base(list, &r, name, K_INFO, UI_ROW_H);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    m->title = label(r, &ui_16, UI_COL_TEXT2, 20, 9, 212, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(m->title, caption ? caption : "");
    m->sub = label(r, &ui_20, UI_COL_TEXT1, 20, 31, 212, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(m->sub, value ? value : "");
    return r;
}

lv_obj_t *ui_list_section(lv_obj_t *list, const char *name, const char *text)
{
    lv_obj_t *r;
    row_t *m = row_base(list, &r, name, K_SECTION, SECTION_H);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    m->title = label(r, &ui_16, UI_COL_TEXT3, 20, 6, 212, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(m->title, text ? text : "");
    return r;
}

void ui_row_set_title(lv_obj_t *row, const char *title)
{
    row_t *m = meta(row);
    if (m && m->title) {
        lv_label_set_text(m->title, title ? title : "");
    }
}

void ui_row_set_sub(lv_obj_t *row, const char *sub)
{
    row_t *m = meta(row);
    if (!m || !m->sub) {
        return;
    }
    lv_label_set_text(m->sub, sub ? sub : "");
    if (m->kind == K_ROW) {
        // a row without a subtitle centres its title
        lv_obj_set_flag(m->sub, LV_OBJ_FLAG_HIDDEN, !sub || !sub[0]);
        lv_obj_set_y(m->title, sub && sub[0] ? 10 : 19);
    }
}

void ui_row_set_sub_accent(lv_obj_t *row, ui_acc_t acc)
{
    row_t *m = meta(row);
    if (m && m->sub) {
        lv_obj_set_style_text_color(m->sub, ui_col(acc < UI_ACC__N ? ui_accent(acc)->top : UI_COL_TEXT2), 0);
    }
}

void ui_info_set(lv_obj_t *row, const char *value)
{
    row_t *m = meta(row);
    if (m && m->kind == K_INFO) {
        lv_label_set_text(m->sub, value ? value : "");
    }
}

void ui_row_set_check(lv_obj_t *row, bool on)
{
    row_t *m = meta(row);
    if (m && m->right == UI_ROW_CHECK) {
        m->on = on;
        lv_label_set_text_static(m->rlabel, on ? SYM_CHECK : "");
    }
}

void ui_row_set_toggle(lv_obj_t *row, bool on)
{
    row_t *m = meta(row);
    if (m && m->right == UI_ROW_TOGGLE) {
        m->on = on;
        lv_label_set_text(m->rlabel, ui_s(on ? S_ON : S_OFF));
        lv_obj_set_style_text_color(m->rlabel, ui_col(on ? UI_COL_TEXT1 : UI_COL_TEXT3), 0);
    }
}

void ui_row_set_wifi(lv_obj_t *row, int rssi, bool locked)
{
    row_t *m = meta(row);
    if (m && m->right == UI_ROW_WIFI) {
        m->rssi = (int8_t)LV_CLAMP(-127, rssi, 0);
        m->locked = locked;
        lv_label_set_text_static(m->rlabel, locked ? SYM_LOCK : "");
        lv_obj_invalidate(row);
    }
}

void ui_row_set_enabled(lv_obj_t *row, bool en)
{
    row_t *m = meta(row);
    if (!m) {
        return;
    }
    m->enabled = en;
    lv_obj_set_style_text_color(m->title, ui_col(en ? UI_COL_TEXT1 : UI_COL_TEXT3), 0);
    if (m->icon) {
        lv_obj_set_style_text_color(m->icon, ui_col(en ? UI_COL_TEXT2 : UI_COL_TEXT3), 0);
    }
    lv_obj_set_state(row, LV_STATE_DISABLED, !en);
    lv_obj_set_flag(row, LV_OBJ_FLAG_CLICKABLE, en && m->cb && m->opa >= 128);
}

void ui_row_set_danger(lv_obj_t *row)
{
    row_t *m = meta(row);
    if (m) {
        lv_color_t c = ui_col(ui_accent(UI_ACC_ERROR)->top);
        lv_obj_set_style_text_color(m->title, c, 0);
        if (m->icon) {
            lv_obj_set_style_text_color(m->icon, c, 0);
        }
    }
}

void ui_list_trim(lv_obj_t *list, uint32_t first)
{
    while (lv_obj_get_child_count(list) > first) {
        lv_obj_delete(lv_obj_get_child(list, -1));
    }
}

void ui_list_focus(lv_obj_t *row)
{
    lv_obj_t *list = row ? lv_obj_get_parent(row) : NULL;
    if (!list || !lv_obj_has_flag(list, LV_OBJ_FLAG_SCROLLABLE)) {
        return;
    }
    lv_obj_update_layout(list);
    lv_area_t a;
    lv_obj_get_coords(row, &a);
    int32_t delta = (a.y1 + a.y2) / 2 - UI_CY;     // > 0: the row is below the centre line
    int32_t y = lv_obj_get_scroll_y(list);
    int32_t max = y + lv_obj_get_scroll_bottom(list);
    int32_t to = LV_CLAMP(0, y + delta, LV_MAX(max, 0));
    if (to != y) {
        lv_obj_scroll_to_y(list, to, LV_ANIM_OFF);
    }
    fade_apply(list);
}
