// Q, phone setup T_QR (docs/ui.md 3.10): the SoftAP portal with a Wi-Fi QR code (step 1), the portal URL once a
// phone joined (step 2), the list of what arrived (step 3), then home with "Готово". Leaving stops the portal.
// portal_start/stop, the station count and the settings comparison run in the UI worker.
#include "ui_internal.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_work.h"
#include "board.h"
#include "net.h"
#include "portal.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"

// panel x 69-291, y 58-280 (222x222, r16): corner centres (85,74) / (275,74) / (85,264) -> |(95,106)|+16 = 158.3;
// qrcode 174 at (93,82) = panel + (24,24): a version-3 code is 29 modules x 6 px with a 4-module quiet zone.
// Caption 1 centre (180,296) w <= 220; caption 2 centre (180,318) w <= 150; step 3 lines at y 130/166/202.
#define PANEL_X     69
#define PANEL_Y     58
#define PANEL_S     222
#define QR_S        174
#define QR_OFS      24
#define POLL_MS     1000
#define PORTAL_MAX_MS (10 * 60 * 1000)
#define URL         "http://192.168.4.1/"

typedef enum { Q_STARTING, Q_JOIN, Q_OPEN, Q_RECEIVED, Q_DONE, Q_FAIL } q_step_t;

static lv_obj_t *s_scr, *s_panel, *s_qr, *s_cap1, *s_cap2, *s_lines[3];
static lv_timer_t *s_poll;
static q_step_t s_step;
static bool s_started, s_polling, s_busy;
static int s_rx_polls;          // polls since data first arrived
static uint32_t s_open_t;
static char s_ap_ssid[33], s_ap_pass[12];
static uint32_t s_gen;

// what the phone sent, compared with the state when the portal started (worker task only); values are kept as
// FNV-1a digests so no copy of the key lingers
static struct {
    uint64_t key, model, url;
    char ssid[33];
    int saved;
    char list[NET_MAX_SAVED][33];       // saved networks: net's own reconnects to them are not phone data
} s_snap;

static bool snap_known(const char *ssid)
{
    for (int i = 0; i < s_snap.saved && i < NET_MAX_SAVED; i++) {
        if (!strcmp(s_snap.list[i], ssid)) {
            return true;
        }
    }
    return false;
}

static uint64_t digest(const char *key)
{
    char v[208];
    settings_get_str(key, v, sizeof(v));
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        h = (h ^ *p) * 1099511628211ull;
    }
    memset(v, 0, sizeof(v));
    return h;
}

static void busy(bool on)
{
    if (on != s_busy) {
        s_busy = on;
        ui_busy(on);
    }
}

// ---- Worker jobs

typedef struct {
    uint32_t gen;
    esp_err_t err;
    char ssid[33], pass[12];
} start_job_t;

static void start_job(void *arg)
{
    start_job_t *j = arg;
    j->err = portal_start();
    if (j->err == ESP_OK) {
        portal_credentials(j->ssid, sizeof(j->ssid), j->pass, sizeof(j->pass));
    }
    s_snap.key = digest("ai_key");
    s_snap.model = digest("ai_model");
    s_snap.url = digest("ai_url");
    net_status_t st;
    net_status(&st);
    strlcpy(s_snap.ssid, st.state == NET_UP ? st.ssid : "", sizeof(s_snap.ssid));
    s_snap.saved = net_saved_list(s_snap.list, NET_MAX_SAVED);
}

typedef struct {
    uint32_t gen;
    int stations;
    bool key_rx, model_rx, wifi_rx, up, failed;
    char ip[16];
} poll_job_t;

static void poll_job(void *arg)
{
    poll_job_t *j = arg;
    wifi_sta_list_t l;
    j->stations = esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
    j->key_rx = digest("ai_key") != s_snap.key;
    j->model_rx = digest("ai_model") != s_snap.model || digest("ai_url") != s_snap.url;
    net_status_t st;
    net_status(&st);
    // the portal's /save starts net_connect: an unknown SSID being joined, or a changed saved list, means Wi-Fi
    // data arrived (net reconnecting to a network it already knew does not count)
    j->wifi_rx = (st.state != NET_OFF && st.ssid[0] && strcmp(st.ssid, s_snap.ssid) != 0 && !snap_known(st.ssid)) ||
                 st.saved != s_snap.saved;
    j->up = st.state == NET_UP;
    j->failed = st.state == NET_FAILED;
    strlcpy(j->ip, st.ip, sizeof(j->ip));
}

static void stop_job(void *arg)
{
    (void)arg;
    portal_stop();
}

// ---- Screen

static void caption(lv_obj_t *l, const char *text)
{
    lv_obj_set_flag(l, LV_OBJ_FLAG_HIDDEN, !text);
    if (text) {
        lv_label_set_text(l, text);
    }
}

static void show_qr(const char *payload)
{
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        lv_obj_add_flag(s_lines[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_qrcode_update(s_qr, payload, (uint32_t)strlen(payload));
}

static void show_step(q_step_t st, const poll_job_t *p)
{
    char a[64], b[64];
    s_step = st;
    switch (st) {
    case Q_STARTING:
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        caption(s_cap1, ui_s(S_Q_STARTING));
        caption(s_cap2, NULL);
        break;
    case Q_JOIN: {
        // WIFI:T:WPA;S:<ssid>;P:<pass>;; (38 bytes -> version 3 at ECC M)
        char payload[80];
        snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", s_ap_ssid, s_ap_pass);
        show_qr(payload);
        snprintf(a, sizeof(a), ui_s(S_Q_NET_FMT), s_ap_ssid);
        snprintf(b, sizeof(b), ui_s(S_Q_PASS_FMT), s_ap_pass);
        caption(s_cap1, a);
        caption(s_cap2, b);
        break;
    }
    case Q_OPEN:
        show_qr(URL);
        caption(s_cap1, ui_s(S_Q_OPEN));
        caption(s_cap2, ui_s(S_Q_WAIT));
        break;
    case Q_RECEIVED: {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        const char *got[3];
        int n = 0;
        if (p && p->wifi_rx) {
            got[n++] = ui_s(S_Q_WIFI_OK);
        }
        if (p && p->key_rx) {
            got[n++] = ui_s(S_Q_KEY_OK);
        }
        if (p && p->model_rx) {
            got[n++] = ui_s(S_Q_MODEL_OK);
        }
        for (int i = 0; i < 3; i++) {
            lv_obj_set_flag(s_lines[i], LV_OBJ_FLAG_HIDDEN, i >= n);
            if (i < n) {
                lv_label_set_text(s_lines[i], got[i]);
            }
        }
        caption(s_cap1, p && p->failed ? ui_s(S_WC_FAILED) : ui_s(S_S_CONNECTING));
        caption(s_cap2, NULL);
        break;
    }
    case Q_FAIL:
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        caption(s_cap1, ui_s(S_Q_ERROR));
        caption(s_cap2, NULL);
        busy(false);
        break;
    case Q_DONE:
        break;
    }
}

static void finish(void)
{
    s_step = Q_DONE;
    if (s_poll) {
        lv_timer_pause(s_poll);
    }
    if (ui_onboarding()) {
        ui_onboard_resume();
    } else {
        ui_nav_home();
        ui_toast(ui_s(S_DONE));
    }
}

static void poll_done(void *arg)
{
    poll_job_t *p = arg;
    s_polling = false;
    if (!s_scr || p->gen != s_gen || s_step == Q_DONE || s_step == Q_FAIL) {
        return;
    }
    bool rx = p->wifi_rx || p->key_rx || p->model_rx;
    if (rx) {
        // done once Wi-Fi is up with the new data; the portal starts its connect 0.5 s after the answer, so a
        // key alone is only final after two more polls with nothing reconnecting (and one poll that read the
        // new SSID with the old network's state is never final)
        s_rx_polls++;
        if (p->up && s_rx_polls >= (p->wifi_rx ? 2 : 3)) {
            finish();
            return;
        }
        show_step(Q_RECEIVED, p);
        return;
    }
    q_step_t want = p->stations > 0 ? Q_OPEN : Q_JOIN;
    if (want != s_step) {
        show_step(want, p);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_tick_elaps(s_open_t) > PORTAL_MAX_MS && ui_nav_current() == UI_SCR_QR) {
        ui_nav_back();      // the portal closes after 10 min (3.10); leaving the screen stops it
        return;
    }
    if (!s_started || s_polling) {
        return;
    }
    poll_job_t *j = ui_work_alloc(sizeof(poll_job_t));
    if (j) {
        j->gen = s_gen;
        s_polling = ui_work(poll_job, poll_done, j);
    }
}

static void start_done(void *arg)
{
    start_job_t *j = arg;
    if (j->err == ESP_OK) {
        s_started = true;           // stop it on exit even if the screen is already gone
    }
    if (!s_scr || j->gen != s_gen) {
        if (s_started && !s_scr) {
            s_started = false;
            ui_work(stop_job, NULL, NULL);
        }
        return;
    }
    if (j->err != ESP_OK) {
        show_step(Q_FAIL, NULL);
        return;
    }
    strlcpy(s_ap_ssid, j->ssid, sizeof(s_ap_ssid));
    strlcpy(s_ap_pass, j->pass, sizeof(s_ap_pass));
    show_step(Q_JOIN, NULL);
}

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != s_scr) {
        return;
    }
    s_scr = NULL;
    s_gen++;
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    busy(false);
    if (s_started) {
        s_started = false;
        ui_work(stop_job, NULL, NULL);
    }
    memset(s_ap_pass, 0, sizeof(s_ap_pass));
    board_backlight_set((uint8_t)ui_cfg_int("bright"));
}

static lv_obj_t *centred(lv_obj_t *scr, const char *name, const lv_style_t *st, int32_t cy, int32_t w)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_name_static(l, name);
    lv_obj_add_style(l, (lv_style_t *)st, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    int32_t h = lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN));
    lv_obj_set_size(l, w, h);
    lv_obj_set_pos(l, 180 - w / 2, cy - h / 2);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

lv_obj_t *scr_qr_build(uint32_t arg)
{
    (void)arg;
    s_gen++;
    s_scr = ui_chrome_screen();
    lv_obj_set_name_static(s_scr, "phone");
    lv_obj_add_event_cb(s_scr, delete_cb, LV_EVENT_DELETE, NULL);

    s_panel = lv_obj_create(s_scr);
    ui_theme_plain(s_panel);
    lv_obj_set_name_static(s_panel, "qr_panel");
    lv_obj_add_style(s_panel, &st_qr_panel, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_panel, PANEL_X, PANEL_Y);
    lv_obj_set_size(s_panel, PANEL_S, PANEL_S);
    s_qr = lv_qrcode_create(s_panel);
    lv_obj_set_name_static(s_qr, "qr_code");
    // 9.5 order: size, colours, quiet zone, then the data (a later size change clears the code)
    lv_qrcode_set_size(s_qr, QR_S);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(s_qr, false);
    lv_obj_set_pos(s_qr, QR_OFS, QR_OFS);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) {
        static const char *const names[3] = { "qr_line0", "qr_line1", "qr_line2" };
        s_lines[i] = centred(s_scr, names[i], &st_text20, 130 + 36 * i, 220);
    }
    s_cap1 = centred(s_scr, "qr_cap1", &st_text16_1, 296, 220);
    s_cap2 = centred(s_scr, "qr_cap2", &st_text16, 318, 150);
    // created after the panel: the title ends above its top edge (Q title <= 100 px)
    ui_chrome_title(s_scr, ui_s(S_T_PHONE));
    ui_chrome_swipe_back(s_scr);

    board_backlight_set(100);       // a QR is scanned at full brightness (3.10)
    s_started = s_polling = false;
    s_rx_polls = 0;
    s_open_t = lv_tick_get();
    show_step(Q_STARTING, NULL);
    busy(true);
    start_job_t *j = ui_work_alloc(sizeof(start_job_t));
    if (j) {
        j->gen = s_gen;
        ui_work(start_job, start_done, j);
    }
    s_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
    return s_scr;
}
