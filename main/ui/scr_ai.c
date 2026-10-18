// P (provider), AI (chat model picker), the key / credit check, V (voice), the TTS model and voice pickers and the
// speech speed of docs/ui.md 3.7. Model lists and GET /key run in the UI worker (the ai client blocks it while its
// own task does the HTTPS work); every choice is saved through the debounced settings writer.
#include "ui_internal.h"
#include "ui_list.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_work.h"
#include "ai_client.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

#define LIST_MAX    12      // rows fetched from /models (3.7: at most 12)
#define VOICES_MAX  40

static void masked_key(char *out, size_t len)
{
    char k[208];
    size_t n = ui_cfg_str("ai_key", k, sizeof(k));
    if (n == 0) {
        ui_str_copy(out, len, ui_s(S_P_KEY_NONE));
    } else if (n <= 10) {
        snprintf(out, len, "*** (%u)", (unsigned)n);
    } else {
        snprintf(out, len, "%.6s" TXT_ELLIPSIS "%s", k, k + n - 4);
    }
    memset(k, 0, sizeof(k));
}

// OpenRouter names are "Vendor: Model": the vendor is implied by the id and costs row width
static const char *display_name(const char *name, const char *id)
{
    if (!name || !name[0]) {
        return id;
    }
    const char *colon = strstr(name, ": ");
    return colon && colon[2] ? colon + 2 : name;
}

static void price(float usd, char *out, size_t len)
{
    char v[16];
    snprintf(v, sizeof(v), usd < 0.01f ? "%.4f" : "%.3f", (double)usd);
    snprintf(out, len, ui_s(S_AI_PRICE_FMT), v);
}

// ---- P

static lv_obj_t *s_p_scr, *s_p_url, *s_p_key, *s_p_model, *s_p_web;

// ai_web (protocol §2.8): 0 off, 1 auto (default), 2 always
static const char *web_text(int32_t v)
{
    return ui_s(v == 2 ? S_WEB_ALWAYS : v == 0 ? S_OFF : S_AUTO);
}

static void p_refresh(void)
{
    char buf[112], tmp[136];
    ui_cfg_str("ai_url", tmp, sizeof(tmp));
    ui_url_host(tmp, buf, sizeof(buf));
    ui_row_set_sub(s_p_url, buf);
    masked_key(buf, sizeof(buf));
    ui_row_set_sub(s_p_key, buf);
    ui_cfg_str("ai_model", tmp, sizeof(tmp));
    ui_short_model(tmp, buf, sizeof(buf));
    ui_row_set_sub(s_p_model, buf);
    ui_row_set_sub(s_p_web, web_text(ui_cfg_int("ai_web")));
}

// Credentials are written at once (an explicit save on a settings screen, not a slider): the AI client reads the
// stored values, so "Проверить ключ" or "Загрузить список" right after would still use the old ones
static void url_done(const char *text, bool ok, void *ctx)
{
    (void)ctx;
    if (ok) {
        ui_save_str("ai_url", text);
        ui_save_flush();
        ui_toast(ui_s(S_DONE));
    }
}

static void key_done(const char *text, bool ok, void *ctx)
{
    (void)ctx;
    if (ok && text[0]) {
        ui_save_str("ai_key", text);
        ui_save_flush();
        ui_toast(ui_s(S_DONE));
    }
}

static void p_cb(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0: {
        char url[136];
        ui_cfg_str("ai_url", url, sizeof(url));
        ui_text_open(&(ui_text_req_t){ .field = UI_FIELD_URL, .prompt = ui_s(S_K_URL), .init = url,
                                       .done = url_done });
        break;
    }
    case 1: {
        // OpenRouter keys on the hex keypad; a key of another format opens the full keyboard
        char k[16];
        size_t n = ui_cfg_str("ai_key", k, sizeof(k));
        bool hex = n == 0 || !strncmp(k, UI_KEY_PREFIX, strlen(UI_KEY_PREFIX));
        memset(k, 0, sizeof(k));
        ui_text_open(&(ui_text_req_t){ .field = hex ? UI_FIELD_KEY_HEX : UI_FIELD_KEY, .prompt = ui_s(S_K_KEY),
                                       .done = key_done });
        break;
    }
    case 2: ui_nav_push(UI_SCR_MODELS, 0); break;
    case 3: ui_nav_push(UI_SCR_KEYINFO, 0); break;
    case 4: ui_nav_push(UI_SCR_QR, 0); break;
    case 5: {
        // a tap cycles Авто -> Всегда -> Выкл -> Авто, like "Динамик" in S
        int32_t v = ui_cfg_int("ai_web");
        int32_t to = v == 1 ? 2 : v == 2 ? 0 : 1;
        ui_save_int("ai_web", to);
        ui_row_set_sub(s_p_web, web_text(to));
        break;
    }
    default: break;
    }
}

static void p_loaded_cb(lv_event_t *e)
{
    (void)e;
    p_refresh();
}

static void p_delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s_p_scr) {
        s_p_scr = s_p_url = s_p_key = s_p_model = s_p_web = NULL;
    }
}

lv_obj_t *scr_provider_build(uint32_t arg)
{
    (void)arg;
    lv_obj_t *list;
    s_p_scr = ui_list_screen("provider", ui_s(S_T_PROVIDER), &list);
    lv_obj_add_event_cb(s_p_scr, p_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_p_scr, p_delete_cb, LV_EVENT_DELETE, NULL);
    s_p_url = ui_list_row(list, "p_url", SYM_GLOBE, ui_s(S_P_URL), "", UI_ROW_CHEVRON, p_cb, (void *)0);
    s_p_key = ui_list_row(list, "p_key", SYM_KEY, ui_s(S_T_KEY), "", UI_ROW_CHEVRON, p_cb, (void *)1);
    s_p_model = ui_list_row(list, "p_model", SYM_ROBOT, ui_s(S_T_MODELS), "", UI_ROW_CHEVRON, p_cb, (void *)2);
    s_p_web = ui_list_row(list, "p_web", SYM_GLOBE, ui_s(S_P_WEB), "", UI_ROW_NONE, p_cb, (void *)5);
    ui_list_row(list, "p_check", SYM_INFO, ui_s(S_P_CHECK), NULL, UI_ROW_CHEVRON, p_cb, (void *)3);
    ui_list_row(list, "p_phone", SYM_QRCODE, ui_s(S_P_PHONE), ui_s(S_S_PHONE_SUB), UI_ROW_CHEVRON, p_cb, (void *)4);
    ui_list_refresh(list);
    return s_p_scr;
}

// ---- Generic picker of ids (chat models, TTS models, voices)

typedef struct {
    char id[65];
    char extra[64];     // TTS model: its default voice
} pick_t;

typedef struct {
    lv_obj_t *scr, *list, *load;
    pick_t *items;      // PSRAM, ui_work_alloc
    int n, cap;
    const char *key;    // setting the choice is saved to
    uint32_t gen;
    bool busy;
} picker_t;

static void picker_busy(picker_t *p, bool on)
{
    if (p->busy != on) {
        p->busy = on;
        ui_busy(on);
    }
}

static int picker_add(picker_t *p, const char *id, const char *extra)
{
    for (int i = 0; i < p->n; i++) {
        if (!strcmp(p->items[i].id, id)) {
            return -1;      // already listed (curated rows come first)
        }
    }
    if (p->n >= p->cap) {
        return -1;
    }
    strlcpy(p->items[p->n].id, id, sizeof(p->items[0].id));
    strlcpy(p->items[p->n].extra, extra ? extra : "", sizeof(p->items[0].extra));
    return p->n++;
}

static lv_obj_t *picker_row(picker_t *p, int idx, const char *prefix, const char *title, const char *sub,
                            lv_event_cb_t cb)
{
    char name[24], cur[100];
    snprintf(name, sizeof(name), "%s_%d", prefix, idx);
    lv_obj_t *r = ui_list_row(p->list, name, NULL, title, sub, UI_ROW_CHECK, cb, (void *)(intptr_t)idx);
    ui_cfg_str(p->key, cur, sizeof(cur));
    ui_row_set_check(r, !strcmp(cur, p->items[idx].id));
    return r;
}

static void picker_delete(picker_t *p, lv_event_t *e)
{
    if (lv_event_get_current_target(e) != p->scr) {
        return;
    }
    picker_busy(p, false);
    heap_caps_free(p->items);
    uint32_t gen = p->gen;
    memset(p, 0, sizeof(*p));
    p->gen = gen + 1;       // results still in flight are dropped
}

static bool picker_init(picker_t *p, const char *key, int cap)
{
    // a previous screen of this picker may still await its async delete: its state goes now
    picker_busy(p, false);
    heap_caps_free(p->items);
    p->gen++;
    uint32_t gen = p->gen;
    memset(p, 0, sizeof(*p));
    p->gen = gen;
    p->items = ui_work_alloc(sizeof(pick_t) * (size_t)cap);
    p->cap = p->items ? cap : 0;
    p->key = key;
    return p->items != NULL;
}

// ---- AI: chat models

static picker_t s_ai;

typedef struct {
    uint32_t gen;
    esp_err_t err;
    int n, total;
    ai_model_t m[LIST_MAX];
} models_job_t;

static void models_job(void *arg)
{
    models_job_t *j = arg;
    j->err = ai_client_list_models(0, j->m, LIST_MAX, &j->n, &j->total);
}

static void ai_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_ai.n) {
        ui_save_str("ai_model", s_ai.items[i].id);
        ui_nav_back();
    }
}

static void models_done(void *arg)
{
    models_job_t *j = arg;
    if (!s_ai.scr || j->gen != s_ai.gen) {
        return;
    }
    picker_busy(&s_ai, false);
    if (j->err != ESP_OK) {
        ui_row_set_sub(s_ai.load, ui_s(S_ERR_LOAD));
        return;
    }
    ui_row_set_sub(s_ai.load, NULL);
    // rows go before "Свой ID…" (the last child)
    lv_obj_t *custom = lv_obj_get_child(s_ai.list, -1);
    for (int k = 0; k < j->n; k++) {
        const ai_model_t *m = &j->m[k];
        int idx = picker_add(&s_ai, m->id, NULL);
        if (idx < 0) {
            continue;
        }
        char sub[64];
        if (m->expires[0]) {
            // "2026-10-20" -> "истекает 20.10" (3.7)
            char d[12];
            if (strlen(m->expires) >= 10 && m->expires[4] == '-' && m->expires[7] == '-') {
                snprintf(d, sizeof(d), "%.2s.%.2s", m->expires + 8, m->expires + 5);
            } else {
                ui_str_copy(d, sizeof(d), m->expires);
            }
            snprintf(sub, sizeof(sub), ui_s(S_AI_EXPIRES_FMT), d);
        } else {
            price(m->est_turn_usd, sub, sizeof(sub));
        }
        lv_obj_t *r = picker_row(&s_ai, idx, "ai", display_name(m->name, m->id), sub, ai_pick_cb);
        if (m->expires[0]) {
            ui_row_set_sub_accent(r, UI_ACC_WARNING);
        }
        lv_obj_move_to_index(r, lv_obj_get_index(custom));
    }
    ui_list_refresh(s_ai.list);
}

static void ai_load_cb(lv_event_t *e)
{
    (void)e;
    if (s_ai.busy) {
        return;
    }
    models_job_t *j = ui_work_alloc(sizeof(models_job_t));
    if (j) {
        j->gen = s_ai.gen;
        if (ui_work(models_job, models_done, j)) {
            picker_busy(&s_ai, true);
            ui_row_set_sub(s_ai.load, ui_s(S_LOADING));
        }
    }
}

static void custom_done(const char *text, bool ok, void *ctx)
{
    (void)ctx;
    if (ok && text[0]) {
        ui_save_str("ai_model", text);
        if (ui_nav_current() == UI_SCR_MODELS) {
            ui_nav_back();
        }
    }
}

static void ai_custom_cb(lv_event_t *e)
{
    (void)e;
    ui_text_open(&(ui_text_req_t){ .field = UI_FIELD_MODEL, .prompt = ui_s(S_K_MODEL), .init = "google/",
                                   .done = custom_done });
}

static void ai_delete_cb(lv_event_t *e)
{
    picker_delete(&s_ai, e);
}

lv_obj_t *scr_models_build(uint32_t arg)
{
    (void)arg;
    static const struct {
        const char *id, *name;
        float usd;
    } curated[] = {
        // "Gemini 3.1 Flash-Lite" is 196 px: a row title without an icon holds 180
        { "google/gemini-3.1-flash-lite", "Gemini 3.1 Lite", 0.00058f },
        { "google/gemini-3.5-flash-lite", "Gemini 3.5 Lite", 0.00073f },
    };
    if (!picker_init(&s_ai, "ai_model", LIST_MAX + 4)) {
        return NULL;
    }
    s_ai.scr = ui_list_screen("models", ui_s(S_T_MODELS), &s_ai.list);
    lv_obj_add_event_cb(s_ai.scr, ai_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_t *sel = NULL;
    for (size_t i = 0; i < sizeof(curated) / sizeof(curated[0]); i++) {
        int idx = picker_add(&s_ai, curated[i].id, NULL);
        char sub[64];
        price(curated[i].usd, sub, sizeof(sub));
        lv_obj_t *r = picker_row(&s_ai, idx, "ai", curated[i].name, sub, ai_pick_cb);
        char cur[100];
        ui_cfg_str("ai_model", cur, sizeof(cur));
        if (!strcmp(cur, curated[i].id)) {
            sel = r;
        }
    }
    // a current model that is not curated still shows (and keeps its check mark)
    char cur[100];
    ui_cfg_str("ai_model", cur, sizeof(cur));
    if (cur[0] && !sel) {
        int idx = picker_add(&s_ai, cur, NULL);
        if (idx >= 0) {
            char nm[64];
            ui_short_model(cur, nm, sizeof(nm));
            picker_row(&s_ai, idx, "ai", nm, NULL, ai_pick_cb);
        }
    }
    s_ai.load = ui_list_row(s_ai.list, "ai_load", SYM_SYNC, ui_s(S_AI_LOAD), NULL, UI_ROW_NONE, ai_load_cb, NULL);
    ui_list_row(s_ai.list, "ai_custom", SYM_KEYBOARD, ui_s(S_AI_CUSTOM), NULL, UI_ROW_CHEVRON, ai_custom_cb, NULL);
    ui_list_refresh(s_ai.list);
    return s_ai.scr;
}

// ---- Key and credit (GET /key, protocol 8)

static lv_obj_t *s_ki_scr, *s_ki_list;
static uint32_t s_ki_gen;
static bool s_ki_busy;

typedef struct {
    uint32_t gen;
    esp_err_t err;
    ai_key_info_t k;
    or_err_t e;
} key_job_t;

static void key_job(void *arg)
{
    key_job_t *j = arg;
    j->err = ai_client_key_info(&j->k, &j->e);
}

static void usd(double v, char *out, size_t len)
{
    snprintf(out, len, "$%.2f", v);
}

static void key_info_done(void *arg)
{
    key_job_t *j = arg;
    if (s_ki_busy) {
        s_ki_busy = false;
        ui_busy(false);
    }
    if (!s_ki_scr || j->gen != s_ki_gen) {
        return;
    }
    ui_list_trim(s_ki_list, 0);
    char a[24], b[24], buf[64];
    if (j->err != ESP_OK) {
        ui_list_info(s_ki_list, "ki_error", ui_s(S_E_GENERIC), j->e.msg_ru[0] ? j->e.msg_ru : esp_err_to_name(j->err));
        ui_list_refresh(s_ki_list);
        return;
    }
    const ai_key_info_t *k = &j->k;
    if (k->has_remaining) {
        usd(k->remaining, a, sizeof(a));
        if (k->remaining_of > 0) {
            snprintf(b, sizeof(b), "%.2f", k->remaining_of);
            snprintf(buf, sizeof(buf), ui_s(S_KI_OF_FMT), a + 1, b);
        } else {
            ui_str_copy(buf, sizeof(buf), a);
        }
    } else {
        ui_str_copy(buf, sizeof(buf), ui_s(S_KI_NO_LIMIT));
    }
    lv_obj_t *r = ui_list_info(s_ki_list, "ki_remaining", ui_s(S_KI_REMAINING), buf);
    if (k->has_remaining && k->remaining < 1.0) {
        ui_row_set_sub_accent(r, k->remaining <= 0 ? UI_ACC_ERROR : UI_ACC_WARNING);
    }
    if (k->limit_reset[0]) {
        ui_sid_t id = !strcmp(k->limit_reset, "daily") ? S_KI_DAILY : !strcmp(k->limit_reset, "weekly") ? S_KI_WEEKLY
                                                                                                          : S_KI_MONTHLY;
        ui_list_info(s_ki_list, "ki_reset", ui_s(S_KI_RESET), ui_s(id));
    }
    usd(k->usage_daily, a, sizeof(a));
    ui_list_info(s_ki_list, "ki_today", ui_s(S_KI_TODAY), a);
    usd(k->usage_monthly, a, sizeof(a));
    ui_list_info(s_ki_list, "ki_month", ui_s(S_KI_MONTH), a);
    usd(k->usage, a, sizeof(a));
    ui_list_info(s_ki_list, "ki_total", ui_s(S_KI_TOTAL), a);
    if (k->label[0]) {
        ui_list_info(s_ki_list, "ki_label", ui_s(S_T_KEY), k->label);
    }
    if (k->free_tier) {
        ui_list_section(s_ki_list, "ki_free", ui_s(S_KI_FREE));
    }
    ui_list_refresh(s_ki_list);
}

static void ki_delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s_ki_scr) {
        s_ki_scr = s_ki_list = NULL;
        s_ki_gen++;
    }
}

lv_obj_t *scr_keyinfo_build(uint32_t arg)
{
    (void)arg;
    s_ki_gen++;
    s_ki_scr = ui_list_screen("keyinfo", ui_s(S_T_KEY), &s_ki_list);
    lv_obj_add_event_cb(s_ki_scr, ki_delete_cb, LV_EVENT_DELETE, NULL);
    char buf[64];
    masked_key(buf, sizeof(buf));
    ui_list_info(s_ki_list, "ki_key", ui_s(S_T_KEY), buf);
    ui_list_info(s_ki_list, "ki_loading", ui_s(S_KI_REMAINING), ui_s(S_LOADING));
    ui_list_refresh(s_ki_list);
    key_job_t *j = ui_work_alloc(sizeof(key_job_t));
    if (j) {
        j->gen = s_ki_gen;
        if (ui_work(key_job, key_info_done, j) && !s_ki_busy) {
            s_ki_busy = true;
            ui_busy(true);
        }
    }
    return s_ki_scr;
}

// ---- V: voice

static lv_obj_t *s_v_scr, *s_v_reply, *s_v_model, *s_v_voice, *s_v_speed;

static void speed_text(int32_t v, char *out, size_t len)
{
    snprintf(out, len, "%d.%02d" TXT_TIMES, (int)(v / 100), (int)(v % 100));
}

static void v_refresh(void)
{
    char buf[100], tmp[100];
    ui_row_set_toggle(s_v_reply, !ui_get(g_s_text_only) && ui_get(g_s_speaker));
    ui_cfg_str("tts_model", tmp, sizeof(tmp));
    ui_short_model(tmp, buf, sizeof(buf));
    ui_row_set_sub(s_v_model, buf);
    ui_cfg_str("tts_voice", tmp, sizeof(tmp));
    ui_short_voice(tmp, buf, sizeof(buf));
    ui_row_set_sub(s_v_voice, buf);
    speed_text(ui_cfg_int("tts_speed"), buf, sizeof(buf));
    ui_row_set_sub(s_v_speed, buf);
}

static void v_cb(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0: {
        if (!ui_get(g_s_speaker)) {
            ui_toast(ui_s(S_C_SPK_T));
            break;
        }
        int32_t to = !ui_get(g_s_text_only);
        ui_set(g_s_text_only, to);
        ui_emit(UI_IN_TEXT_ONLY, to);
        ui_save_int("text_only", to);
        v_refresh();
        break;
    }
    case 1: ui_nav_push(UI_SCR_TTS_MODELS, 0); break;
    case 2: ui_nav_push(UI_SCR_VOICES, 0); break;
    case 3: ui_nav_push(UI_SCR_SPEED, 0); break;
    default: break;
    }
}

static void v_loaded_cb(lv_event_t *e)
{
    (void)e;
    v_refresh();
}

static void v_delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s_v_scr) {
        s_v_scr = s_v_reply = s_v_model = s_v_voice = s_v_speed = NULL;
    }
}

lv_obj_t *scr_voice_build(uint32_t arg)
{
    (void)arg;
    lv_obj_t *list;
    s_v_scr = ui_list_screen("voice", ui_s(S_T_VOICE), &list);
    lv_obj_add_event_cb(s_v_scr, v_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_v_scr, v_delete_cb, LV_EVENT_DELETE, NULL);
    s_v_reply = ui_list_row(list, "v_reply", SYM_COMMENT, ui_s(S_VOICE_REPLY), NULL, UI_ROW_TOGGLE, v_cb, (void *)0);
    s_v_model = ui_list_row(list, "v_model", SYM_ROBOT, ui_s(S_V_MODEL), "", UI_ROW_CHEVRON, v_cb, (void *)1);
    s_v_voice = ui_list_row(list, "v_voice", SYM_PLAY, ui_s(S_T_VOICE), "", UI_ROW_CHEVRON, v_cb, (void *)2);
    s_v_speed = ui_list_row(list, "v_speed", SYM_SYNC, ui_s(S_V_SPEED), "", UI_ROW_CHEVRON, v_cb, (void *)3);
    ui_list_refresh(list);
    return s_v_scr;
}

// ---- TTS models

static picker_t s_tts;

typedef struct {
    uint32_t gen;
    esp_err_t err;
    int n, total;
    ai_tts_model_t m[LIST_MAX];
} tts_job_t;

static void tts_job(void *arg)
{
    tts_job_t *j = arg;
    j->err = ai_client_list_tts_models(0, j->m, LIST_MAX, &j->n, &j->total);
}

static void tts_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_tts.n) {
        return;
    }
    ui_save_str("tts_model", s_tts.items[i].id);
    if (s_tts.items[i].extra[0]) {
        ui_save_str("tts_voice", s_tts.items[i].extra);     // a voice of the old model would be rejected
    }
    ui_nav_back();
}

static void tts_done(void *arg)
{
    tts_job_t *j = arg;
    if (!s_tts.scr || j->gen != s_tts.gen) {
        return;
    }
    picker_busy(&s_tts, false);
    ui_row_set_sub(s_tts.load, j->err == ESP_OK ? NULL : ui_s(S_ERR_LOAD));
    lv_obj_t *anchor = s_tts.load;
    for (int k = 0; k < j->n && j->err == ESP_OK; k++) {
        const ai_tts_model_t *m = &j->m[k];
        int idx = picker_add(&s_tts, m->id, m->first_voice);
        if (idx < 0) {
            continue;
        }
        char v[16], sub[64];
        snprintf(v, sizeof(v), "%.3f", (double)m->usd_1k_ru);
        snprintf(sub, sizeof(sub), ui_s(S_V_PRICE_FMT), v);
        lv_obj_t *r = picker_row(&s_tts, idx, "tts", display_name(m->name, m->id), sub, tts_pick_cb);
        lv_obj_move_to_index(r, lv_obj_get_index(anchor));      // fetched rows above "Все модели"
    }
    ui_list_refresh(s_tts.list);
}

static void tts_load_cb(lv_event_t *e)
{
    (void)e;
    if (s_tts.busy) {
        return;
    }
    tts_job_t *j = ui_work_alloc(sizeof(tts_job_t));
    if (j) {
        j->gen = s_tts.gen;
        if (ui_work(tts_job, tts_done, j)) {
            picker_busy(&s_tts, true);
            ui_row_set_sub(s_tts.load, ui_s(S_LOADING));
        }
    }
}

static void tts_delete_cb(lv_event_t *e)
{
    picker_delete(&s_tts, e);
}

lv_obj_t *scr_tts_models_build(uint32_t arg)
{
    (void)arg;
    static const struct {
        const char *id, *name, *voice;
        float usd;
    } curated[] = {
        { "microsoft/mai-voice-2.1-flash", "MAI-Voice Flash", "ru-RU-Masha:MAI-Voice-2.1-Flash", 0.015f },
        { "google/gemini-3.8-flash-lite-tts", "Gemini 3.8 TTS", "Kore", 0.011f },
    };
    if (!picker_init(&s_tts, "tts_model", LIST_MAX + 4)) {
        return NULL;
    }
    s_tts.scr = ui_list_screen("ttsmodels", ui_s(S_T_TTS), &s_tts.list);
    lv_obj_add_event_cb(s_tts.scr, tts_delete_cb, LV_EVENT_DELETE, NULL);
    for (size_t i = 0; i < sizeof(curated) / sizeof(curated[0]); i++) {
        int idx = picker_add(&s_tts, curated[i].id, curated[i].voice);
        char v[16], sub[64];
        snprintf(v, sizeof(v), "%.3f", (double)curated[i].usd);
        snprintf(sub, sizeof(sub), ui_s(S_V_PRICE_FMT), v);
        picker_row(&s_tts, idx, "tts", curated[i].name, sub, tts_pick_cb);
    }
    char cur[100];
    ui_cfg_str("tts_model", cur, sizeof(cur));
    int idx = cur[0] ? picker_add(&s_tts, cur, NULL) : -1;
    if (idx >= 0) {
        char nm[64];
        ui_short_model(cur, nm, sizeof(nm));
        picker_row(&s_tts, idx, "tts", nm, NULL, tts_pick_cb);
    }
    s_tts.load = ui_list_row(s_tts.list, "tts_load", SYM_SYNC, ui_s(S_AI_LOAD), NULL, UI_ROW_NONE, tts_load_cb, NULL);
    ui_list_refresh(s_tts.list);
    return s_tts.scr;
}

// ---- Voices of the selected TTS model

static picker_t s_vo;

typedef struct {
    uint32_t gen;
    char model[100], locale[8];
    char voices[2048];
} voices_job_t;

static void voices_job(void *arg)
{
    voices_job_t *j = arg;
    // the voices come with the TTS model list (cached by the ai client for 120 s)
    ai_tts_model_t one;
    int n = 0, total = 0;
    ai_client_list_tts_models(0, &one, 1, &n, &total);
    size_t len = ai_client_tts_voices(j->model, j->locale, j->voices, sizeof(j->voices));
    if (len == 0) {
        ai_client_tts_voices(j->model, NULL, j->voices, sizeof(j->voices));
    }
}

static void vo_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_vo.n) {
        ui_save_str("tts_voice", s_vo.items[i].id);
        ui_nav_back();
    }
}

static void vo_add(const char *voice)
{
    int idx = picker_add(&s_vo, voice, NULL);
    if (idx < 0) {
        return;
    }
    char title[48], sub[24] = "";
    ui_short_voice(voice, title, sizeof(title));
    const char *colon = strchr(voice, ':');
    const char *dash = strrchr(voice, '-');
    if (colon && dash && dash < colon && dash - voice < (int)sizeof(sub)) {
        memcpy(sub, voice, (size_t)(dash - voice));      // "ru-RU"
        sub[dash - voice] = 0;
    }
    picker_row(&s_vo, idx, "vo", title, sub[0] ? sub : NULL, vo_pick_cb);
}

static void voices_done(void *arg)
{
    voices_job_t *j = arg;
    if (!s_vo.scr || j->gen != s_vo.gen) {
        return;
    }
    picker_busy(&s_vo, false);
    ui_list_trim(s_vo.list, 0);
    s_vo.n = 0;
    for (char *p = j->voices, *nl; *p && s_vo.n < s_vo.cap; p = nl ? nl + 1 : p + strlen(p)) {
        nl = strchr(p, '\n');
        if (nl) {
            *nl = 0;
        }
        if (*p) {
            vo_add(p);
        }
        if (!nl) {
            break;
        }
    }
    if (s_vo.n == 0) {
        // offline or not listed: the protocol 5.5 defaults
        static const char *const mai[] = { "ru-RU-Masha:MAI-Voice-2.1-Flash", "ru-RU-Lev:MAI-Voice-2.1-Flash",
                                           "ru-RU-Grant:MAI-Voice-2.1-Flash", "ru-RU-Harper:MAI-Voice-2.1-Flash" };
        static const char *const gem[] = { "Kore", "Puck", "Charon", "Zephyr", "Aoede", "Leda" };
        bool is_mai = !strncmp(j->model, "microsoft/", 10);
        const char *const *v = is_mai ? mai : gem;
        size_t n = is_mai ? sizeof(mai) / sizeof(mai[0]) : sizeof(gem) / sizeof(gem[0]);
        for (size_t i = 0; i < n; i++) {
            vo_add(v[i]);
        }
    }
    ui_list_refresh(s_vo.list);
}

static void vo_delete_cb(lv_event_t *e)
{
    picker_delete(&s_vo, e);
}

lv_obj_t *scr_voices_build(uint32_t arg)
{
    (void)arg;
    if (!picker_init(&s_vo, "tts_voice", VOICES_MAX)) {
        return NULL;
    }
    s_vo.scr = ui_list_screen("voices", ui_s(S_T_VOICES), &s_vo.list);
    lv_obj_add_event_cb(s_vo.scr, vo_delete_cb, LV_EVENT_DELETE, NULL);
    ui_list_info(s_vo.list, "vo_loading", ui_s(S_T_VOICES), ui_s(S_LOADING));
    ui_list_refresh(s_vo.list);
    voices_job_t *j = ui_work_alloc(sizeof(voices_job_t));
    if (j) {
        j->gen = s_vo.gen;
        ui_cfg_str("tts_model", j->model, sizeof(j->model));
        // microsoft/* voices are per locale: the language the assistant usually answers in
        strlcpy(j->locale, ui_str_lang() ? "en-US" : "ru-RU", sizeof(j->locale));
        if (ui_work(voices_job, voices_done, j)) {
            picker_busy(&s_vo, true);
        }
    }
    return s_vo.scr;
}

// ---- Speech speed (tts_speed x0.01; 100 = the field is not sent)

static void speed_cb(lv_event_t *e)
{
    ui_save_int("tts_speed", (int32_t)(intptr_t)lv_event_get_user_data(e));
    ui_nav_back();
}

lv_obj_t *scr_speed_build(uint32_t arg)
{
    (void)arg;
    static const int32_t vals[] = { 80, 90, 100, 110, 125 };
    lv_obj_t *list;
    lv_obj_t *scr = ui_list_screen("speed", ui_s(S_T_SPEED), &list);
    int32_t cur = ui_cfg_int("tts_speed");
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        char name[16], title[16];
        snprintf(name, sizeof(name), "spd_%d", (int)vals[i]);
        speed_text(vals[i], title, sizeof(title));
        lv_obj_t *r = ui_list_row(list, name, NULL, title, vals[i] == 100 ? ui_s(S_AI_DEFAULT) : NULL, UI_ROW_CHECK,
                                  speed_cb, (void *)(intptr_t)vals[i]);
        ui_row_set_check(r, vals[i] == cur);
    }
    ui_list_refresh(list);
    return scr;
}
