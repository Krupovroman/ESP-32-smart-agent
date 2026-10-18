#include "ui_chat.h"
#include "ui.h"
#include "ui_internal.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

// ---- Model: one PSRAM ring per mode

typedef struct {
    cm_entry_t e[CM_MAX];
    int start, count;
} cm_ring_t;

static cm_ring_t *s_ring;       // [2]
static uint32_t s_next_id = 1;
static uint32_t s_tag;          // app turn of the entries added now

void cm_set_tag(uint32_t tag)
{
    s_tag = tag;
}

void cm_init(void)
{
    if (!s_ring) {
        s_ring = heap_caps_calloc(2, sizeof(cm_ring_t), MALLOC_CAP_SPIRAM);
        assert(s_ring);
    }
}

static void entry_free(cm_entry_t *e)
{
    heap_caps_free(e->text);
    heap_caps_free(e->text2);
    memset(e, 0, sizeof(*e));
}

cm_entry_t *cm_add(int mode, cm_kind_t kind)
{
    cm_ring_t *r = &s_ring[mode ? 1 : 0];
    if (r->count == CM_MAX) {
        entry_free(&r->e[r->start]);
        r->start = (r->start + 1) % CM_MAX;
        r->count--;
    }
    cm_entry_t *e = &r->e[(r->start + r->count) % CM_MAX];
    r->count++;
    memset(e, 0, sizeof(*e));
    e->id = s_next_id++;
    e->kind = (uint8_t)kind;
    e->side = UI_SIDE_AUTO;
    e->tag = s_tag;
    return e;
}

cm_entry_t *cm_at(int mode, int idx)
{
    cm_ring_t *r = &s_ring[mode ? 1 : 0];
    if (idx < 0 || idx >= r->count) {
        return NULL;
    }
    return &r->e[(r->start + idx) % CM_MAX];
}

int cm_count(int mode)
{
    return s_ring[mode ? 1 : 0].count;
}

int cm_mode_of(uint32_t id)
{
    for (int m = 0; m < 2; m++) {
        for (int i = 0; i < cm_count(m); i++) {
            if (cm_at(m, i)->id == id) {
                return m;
            }
        }
    }
    return -1;
}

cm_entry_t *cm_find(uint32_t id)
{
    int m = cm_mode_of(id);
    if (m < 0) {
        return NULL;
    }
    for (int i = 0; i < cm_count(m); i++) {
        if (cm_at(m, i)->id == id) {
            return cm_at(m, i);
        }
    }
    return NULL;
}

cm_entry_t *cm_last_of(int mode, cm_kind_t k)
{
    for (int i = cm_count(mode) - 1; i >= 0; i--) {
        if (cm_at(mode, i)->kind == k) {
            return cm_at(mode, i);
        }
    }
    return NULL;
}

cm_entry_t *cm_last_pending(int mode)
{
    for (int i = cm_count(mode) - 1; i >= 0; i--) {
        cm_entry_t *e = cm_at(mode, i);
        if ((e->kind == CM_USER || e->kind == CM_TURN) && e->pending) {
            return e;
        }
    }
    return NULL;
}

void cm_clear(int mode)
{
    cm_ring_t *r = &s_ring[mode ? 1 : 0];
    for (int i = 0; i < r->count; i++) {
        entry_free(&r->e[(r->start + i) % CM_MAX]);
    }
    r->start = r->count = 0;
}

static char *sanitized_copy(const char *text)
{
    size_t n = strlen(text);
    // "- " list marks grow to "• " (+2 bytes each); nothing else grows
    char *buf = heap_caps_malloc(2 * n + 2, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return NULL;
    }
    size_t len = ui_str_sanitize(text, buf, 2 * n + 2, &ui_20);
    char *fit = heap_caps_realloc(buf, len + 1, MALLOC_CAP_SPIRAM);
    return fit ? fit : buf;
}

void cm_set(char **dst, const char *text)
{
    heap_caps_free(*dst);
    *dst = text ? sanitized_copy(text) : NULL;
}

void cm_append(char **dst, const char *chunk)
{
    if (!chunk || !chunk[0]) {
        return;
    }
    char *add = sanitized_copy(chunk);
    if (!add) {
        return;
    }
    if (!*dst) {
        *dst = add;
        return;
    }
    size_t a = strlen(*dst), b = strlen(add);
    // keep a word gap between streamed sentences
    bool gap = a > 0 && b > 0 && (*dst)[a - 1] != ' ' && (*dst)[a - 1] != '\n' && add[0] != ' ';
    char *joined = heap_caps_realloc(*dst, a + b + 2, MALLOC_CAP_SPIRAM);
    if (joined) {
        if (gap) {
            joined[a++] = ' ';
        }
        memcpy(joined + a, add, b + 1);
        *dst = joined;
    }
    heap_caps_free(add);
}

// ---- Widgets

#define WAVE_BARS   32
#define WAVE_W      3
#define WAVE_GAP    3
#define PENDING_W   220
#define PENDING_H   44
#define USER_MAX_W  220
#define TURN_MAX_W  236
#define TURN_TEXT_W (TURN_MAX_W - 24 - 3)  // minus padding 12/12 and the 3 px language stripe

uint32_t ui_msg_id(lv_obj_t *obj)
{
    while (obj) {
        const char *n = lv_obj_get_name(obj);
        if (n && strcmp(n, "msg_#") == 0) {
            return (uint32_t)(uintptr_t)lv_obj_get_user_data(obj);
        }
        obj = lv_obj_get_parent(obj);
    }
    return 0;
}

lv_obj_t *ui_msg_find(lv_obj_t *list, uint32_t id)
{
    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(list, (int32_t)i);
        if ((uint32_t)(uintptr_t)lv_obj_get_user_data(c) == id) {
            return c;
        }
    }
    return NULL;
}

lv_obj_t *ui_msg_at(lv_obj_t *list, lv_point_t p)
{
    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(list, (int32_t)i);
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        // the 6 px list gap above a wrapper counts as part of it
        if (p.y >= a.y1 - 6 && p.y <= a.y2 && lv_obj_get_user_data(c)) {
            return c;
        }
    }
    return NULL;
}

// The waveform lives in the bubble's draw event; its opacity is the inverse of the text label's, so the
// A17 crossfade needs no extra state.
static void wave_draw_cb(lv_event_t *e)
{
    lv_obj_t *bubble = lv_event_get_current_target(e);
    lv_obj_t *label = lv_obj_get_child(bubble, 0);
    lv_opa_t opa = 255 - (label ? lv_obj_get_style_opa(label, LV_PART_MAIN) : 0);
    const cm_entry_t *en = cm_find(ui_msg_id(bubble));
    if (opa <= LV_OPA_MIN || !en) {
        return;
    }
    lv_area_t co;
    lv_obj_get_coords(bubble, &co);
    int32_t total = WAVE_BARS * WAVE_W + (WAVE_BARS - 1) * WAVE_GAP;
    int32_t x = co.x1 + (lv_area_get_width(&co) - total) / 2;
    int32_t cy = co.y1 + lv_area_get_height(&co) / 2;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = ui_col(UI_COL_TEXT3);
    d.bg_opa = opa;
    d.radius = 1;
    lv_layer_t *layer = lv_event_get_layer(e);
    for (int i = 0; i < WAVE_BARS; i++) {
        int32_t h = en->has_env ? 4 + en->env[i] * 24 / 255 : 4;   // heights 4-28 px
        lv_area_t a = { x, cy - h / 2, x + WAVE_W - 1, cy - h / 2 + h - 1 };
        lv_draw_rect(layer, &d, &a);
        x += WAVE_W + WAVE_GAP;
    }
}

static void appear_exec(void *var, int32_t v)
{
    lv_obj_t *o = var;
    lv_obj_set_style_opa(o, (lv_opa_t)(v * 255 / 1024), 0);
    lv_obj_set_style_translate_y(o, 12 - 12 * v / 1024, 0);
}

static void appear(lv_obj_t *wrap, uint32_t delay)
{
    // A16: style opa + translate_y, neither creates a layer
    appear_exec(wrap, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, wrap);
    lv_anim_set_exec_cb(&a, appear_exec);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, UI_T_BASE);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void fade_label_exec(void *var, int32_t v)
{
    lv_obj_t *label = var;
    lv_obj_set_style_opa(label, (lv_opa_t)v, 0);
    lv_obj_invalidate(lv_obj_get_parent(label));   // the waveform behind it fades too
}

static void fade_done(lv_anim_t *a)
{
    // A17 finished: the bubble may shrink to its text
    lv_obj_t *bubble = lv_obj_get_parent(a->var);
    lv_obj_set_style_min_width(bubble, 0, 0);
    lv_obj_set_style_min_height(bubble, 0, 0);
}

static void crossfade(lv_obj_t *label, bool animate)
{
    if (!animate) {
        lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
        return;
    }
    lv_obj_t *bubble = lv_obj_get_parent(label);
    lv_obj_set_style_min_width(bubble, PENDING_W, 0);
    lv_obj_set_style_min_height(bubble, PENDING_H, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, fade_label_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, fade_done);
    lv_anim_start(&a);
}

static void slide_exec(void *var, int32_t v)
{
    lv_obj_set_style_translate_x(var, v, 0);
}

static lv_obj_t *wrapper(lv_obj_t *list, const cm_entry_t *e, lv_event_cb_t on_click)
{
    lv_obj_t *w = lv_obj_create(list);
    ui_theme_plain(w);
    lv_obj_set_name_static(w, "msg_#");
    lv_obj_set_size(w, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_user_data(w, (void *)(uintptr_t)e->id);
    lv_obj_set_style_pad_top(w, e->new_turn ? 8 : 0, 0);   // 6 px list gap + 8 = 14 px between turns
    if (on_click) {
        lv_obj_add_flag(w, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(w, on_click, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_remove_flag(w, LV_OBJ_FLAG_CLICKABLE);
    }
    return w;
}

static lv_obj_t *plain_child(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    ui_theme_plain(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_style_t *st, const char *name)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, (lv_style_t *)st, 0);
    lv_obj_set_name_static(l, name);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static const char *code_upper(const char *c, char *buf)
{
    for (int i = 0; i < 3; i++) {
        buf[i] = (char)toupper((unsigned char)c[i]);
        if (!c[i]) {
            break;
        }
    }
    buf[3] = 0;
    return buf;
}

static void build_user(lv_obj_t *w, const cm_entry_t *e)
{
    lv_obj_t *b = plain_child(w);
    lv_obj_set_name_static(b, "bubble");
    lv_obj_add_style(b, &st_bubble_user, 0);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, USER_MAX_W, 0);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(b, wave_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t *l = label(b, &st_text16, "text");
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(l, USER_MAX_W - 24, 0);
}

static void refresh_user(lv_obj_t *w, const cm_entry_t *e, bool animate)
{
    lv_obj_t *b = lv_obj_get_child(w, 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (e->pending) {
        lv_label_set_text_static(l, "");
        lv_obj_set_style_opa(l, LV_OPA_TRANSP, 0);
        lv_obj_set_size(b, PENDING_W, PENDING_H);
        return;
    }
    bool was_pending = lv_obj_get_style_opa(l, LV_PART_MAIN) == LV_OPA_TRANSP;
    lv_label_set_text(l, e->text ? e->text : "");
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    crossfade(l, animate && was_pending);
}

static void build_bot(lv_obj_t *w, const cm_entry_t *e)
{
    lv_obj_set_flex_flow(w, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *l = label(w, &st_reply, "text");
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_t *n = label(w, &st_text16_3, "note");
    lv_label_set_text_static(n, ui_s(S_TTS_NOTE));
    (void)e;
}

static void refresh_bot(lv_obj_t *w, const cm_entry_t *e)
{
    lv_label_set_text(lv_obj_get_child(w, 0), e->text ? e->text : "");
    lv_obj_set_flag(lv_obj_get_child(w, 1), LV_OBJ_FLAG_HIDDEN, !e->tts_failed);
}

static void build_turn(lv_obj_t *w, const cm_entry_t *e)
{
    lv_obj_t *b = plain_child(w);
    lv_obj_set_name_static(b, "bubble");
    lv_obj_add_style(b, &st_turn, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, TURN_MAX_W, 0);
    lv_obj_add_event_cb(b, wave_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    // child 0 carries the crossfade opacity for the waveform; the other lines follow it
    lv_obj_t *col = plain_child(b);
    lv_obj_set_name_static(col, "lines");
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_t *l1 = label(col, &st_text16, "line1");
    lv_label_set_recolor(l1, true);
    lv_obj_t *l2 = label(col, &st_text16, "line2");
    lv_obj_set_width(l2, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(l2, TURN_TEXT_W, 0);
    lv_obj_t *l3 = label(col, &st_text28, "line3");
    lv_obj_set_width(l3, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(l3, TURN_TEXT_W, 0);
    (void)e;
}

static void refresh_turn(lv_obj_t *w, const cm_entry_t *e, bool animate)
{
    lv_obj_t *b = lv_obj_get_child(w, 0);
    lv_obj_t *col = lv_obj_get_child(b, 0);
    lv_obj_t *l1 = lv_obj_get_child(col, 0), *l2 = lv_obj_get_child(col, 1), *l3 = lv_obj_get_child(col, 2);
    lv_obj_remove_style(b, &st_turn_a, 0);
    lv_obj_remove_style(b, &st_turn_b, 0);
    if (e->pending) {
        lv_obj_set_style_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_size(b, PENDING_W, PENDING_H);
        lv_obj_add_flag(col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 0);
        return;
    }
    bool was_pending = lv_obj_has_flag(col, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    // line 1 "RU → EN" with the target code in the target colour
    char s[8], d[8], buf[64];
    const ui_accent_t *tc = ui_accent(e->side == UI_SIDE_B ? UI_ACC_LANG_A : UI_ACC_LANG_B);
    snprintf(buf, sizeof(buf), "%s " TXT_ARROW " #%06lX %s#", code_upper(e->src, s), (unsigned long)tc->top,
             code_upper(e->dst, d));
    lv_label_set_text(l1, buf);
    lv_label_set_text(l2, e->text ? e->text : "");
    const char *tr = e->text2 ? e->text2 : "";
    // short translations in ui_28 when every glyph exists there (3.4)
    bool big = ui_str_cp_count(tr) <= 48 && ui_str_font_has_all(tr, &ui_28);
    lv_obj_remove_style(l3, &st_text28, 0);
    lv_obj_remove_style(l3, &st_text20, 0);
    lv_obj_add_style(l3, big ? &st_text28 : &st_text20, 0);
    lv_label_set_text(l3, tr);
    lv_obj_set_flag(l3, LV_OBJ_FLAG_HIDDEN, tr[0] == 0);

    lv_align_t al = e->side == UI_SIDE_A ? LV_ALIGN_TOP_LEFT : e->side == UI_SIDE_B ? LV_ALIGN_TOP_RIGHT
                                                                                     : LV_ALIGN_TOP_MID;
    if (e->side == UI_SIDE_A) {
        lv_obj_add_style(b, &st_turn_a, 0);
    } else if (e->side == UI_SIDE_B) {
        lv_obj_add_style(b, &st_turn_b, 0);
    }
    bool was_centred = lv_obj_get_style_align(b, LV_PART_MAIN) == LV_ALIGN_TOP_MID;
    lv_obj_align(b, al, 0, 0);
    crossfade(col, animate && was_pending);
    if (animate && was_centred && al != LV_ALIGN_TOP_MID) {
        // A18: the bubble starts where the centred placeholder was and slides to its speaker's side
        lv_obj_update_layout(b);
        int32_t off = (lv_obj_get_width(w) - lv_obj_get_width(b)) / 2;
        int32_t from = e->side == UI_SIDE_A ? off : -off;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, b);
        lv_anim_set_exec_cb(&a, slide_exec);
        lv_anim_set_values(&a, from, 0);
        lv_anim_set_duration(&a, UI_T_BASE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

static void card_btn_cb(lv_event_t *e)
{
    // routing actions open their fixing screen here (W, P, AI, credit, D); retries go to the app with the
    // card's turn tag, so the turn that failed is the one sent again
    int32_t card = (int32_t)(intptr_t)lv_event_get_user_data(e);
    const cm_entry_t *en = cm_find(ui_msg_id(lv_event_get_target_obj(e)));
    int32_t arg = (int32_t)(((en ? en->tag : 0) << 8) | (uint32_t)card);
    if (!ui_open_card(card) && !ui_emit(UI_IN_CARD_ACTION, arg)) {
        ui_toast(ui_s(S_NOT_YET));
    }
}

typedef struct {
    ui_sid_t title, body, action;
    ui_acc_t acc;
} card_def_t;

static const card_def_t s_cards[UI_CARD__N] = {
    [UI_CARD_GENERIC] = { S_C_ERROR_T, S__N, S__N, UI_ACC_ERROR },
    [UI_CARD_NO_WIFI] = { S_C_WIFI_T, S_C_WIFI_B, S_C_WIFI_A, UI_ACC_ERROR },
    [UI_CARD_BAD_KEY] = { S_C_KEY_T, S__N, S_C_KEY_A, UI_ACC_ERROR },
    [UI_CARD_NO_CREDIT] = { S_C_CREDIT_T, S_C_CREDIT_B, S_C_CREDIT_A, UI_ACC_WARNING },
    [UI_CARD_REGION] = { S_C_REGION_T, S_C_REGION_B, S_C_REGION_A, UI_ACC_ERROR },
    [UI_CARD_MODEL] = { S_C_MODEL_T, S__N, S_C_MODEL_A, UI_ACC_ERROR },
    [UI_CARD_RATE] = { S_C_RATE_T, S__N, S__N, UI_ACC_WARNING },
    [UI_CARD_SERVER] = { S_C_SERVER_T, S__N, S_C_RETRY_A, UI_ACC_ERROR },
    [UI_CARD_TLS] = { S_C_TLS_T, S__N, S_C_RETRY_A, UI_ACC_ERROR },
    [UI_CARD_MIC] = { S_C_MIC_T, S__N, S_C_DIAG_A, UI_ACC_ERROR },
    [UI_CARD_NO_SPEAKER] = { S_C_SPK_T, S_C_SPK_B, S_C_SPK_A, UI_ACC_WARNING },
    [UI_CARD_NO_KEY] = { S_C_NOKEY_T, S__N, S_C_NOKEY_A, UI_ACC_ERROR },
};

static void build_card(lv_obj_t *w, const cm_entry_t *e)
{
    const card_def_t *cd = &s_cards[e->card < UI_CARD__N ? e->card : UI_CARD_GENERIC];
    const ui_accent_t *acc = ui_accent(cd->acc);
    // 3.14: width 252, radius 20, status dim fill, padding 14; icon + title / body / action stadium
    lv_obj_t *c = plain_child(w);
    lv_obj_set_name_static(c, "card");
    lv_obj_add_style(c, &st_card, 0);
    lv_obj_set_style_bg_color(c, ui_col(acc->dim), 0);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char buf[160];
    lv_obj_t *t = label(c, &st_text20, "title");
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_recolor(t, true);
    snprintf(buf, sizeof(buf), "#%06lX " SYM_WARN "#  %s", (unsigned long)acc->top, ui_s(cd->title));
    lv_label_set_text(t, buf);

    const char *body = e->text ? e->text : (cd->body < S__N ? ui_s(cd->body) : NULL);
    if (body && body[0]) {
        lv_obj_t *b = label(c, &st_text16, "body");
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_text(b, body);
    }
    if (cd->action < S__N) {
        lv_obj_t *btn = lv_obj_create(c);
        ui_theme_plain(btn);
        lv_obj_set_name_static(btn, "card_btn");
        lv_obj_add_style(btn, &st_card_btn, 0);
        lv_obj_set_style_bg_color(btn, ui_col(acc->top), 0);
        lv_obj_set_style_bg_color(btn, ui_col(acc->mid), LV_STATE_PRESSED);
        // 160x50 stadium (3.14), wider for long labels such as "Проверить звук"
        lv_obj_set_size(btn, LV_SIZE_CONTENT, 50);
        lv_obj_set_style_min_width(btn, 160, 0);
        lv_obj_set_style_pad_hor(btn, 20, 0);
        lv_obj_set_style_margin_top(btn, 4, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        ui_theme_chain_ver(btn);    // a vertical drag that starts on the button still scrolls the chat
        lv_obj_add_event_cb(btn, card_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)e->card);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text_static(bl, ui_s(cd->action));
        lv_obj_center(bl);
    }
}

static void build_divider(lv_obj_t *w, const cm_entry_t *e)
{
    lv_obj_t *l = label(w, &st_divider, "text");
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, e->text ? e->text : "");
}

lv_obj_t *ui_msg_create(lv_obj_t *list, const cm_entry_t *e, bool animate, uint32_t delay_ms,
                        lv_event_cb_t on_click)
{
    lv_obj_t *w = wrapper(list, e, (e->kind == CM_CARD || e->kind == CM_DIVIDER) ? NULL : on_click);
    switch (e->kind) {
    case CM_USER: build_user(w, e); break;
    case CM_BOT: build_bot(w, e); break;
    case CM_TURN: build_turn(w, e); break;
    case CM_CARD: build_card(w, e); break;
    default: build_divider(w, e); break;
    }
    ui_msg_refresh(w, e, false);
    if (animate) {
        appear(w, delay_ms);
    }
    return w;
}

void ui_msg_refresh(lv_obj_t *wrap, const cm_entry_t *e, bool animate)
{
    if (!wrap || !e) {
        return;
    }
    switch (e->kind) {
    case CM_USER: refresh_user(wrap, e, animate); break;
    case CM_BOT: refresh_bot(wrap, e); break;
    case CM_TURN: refresh_turn(wrap, e, animate); break;
    default: break;
    }
}
