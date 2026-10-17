// W (Wi-Fi networks), one network (connect / forget) and WC (connecting) of docs/ui.md 3.7 and 3.12. Scans,
// connects and forgets run in the UI worker: net_scan blocks for up to 5 s, net_connect for up to 20 s.
#include "ui_internal.h"
#include "ui_list.h"
#include "ui_orb.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_work.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"

#define SCAN_MAX    20
#define SHOW_MAX    16

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;
} ap_t;

// last scan and status as the UI knows them (taskLVGL)
static ap_t s_aps[SCAN_MAX];
static int s_n_aps;
static char s_saved[NET_MAX_SAVED][33];
static int s_n_saved;
static net_status_t s_st;

static lv_obj_t *s_w_scr, *s_w_list, *s_w_rescan;
static bool s_scanning;

// network the detail screen / password entry is about
static struct {
    char ssid[33];
    bool saved, connected, secured;
    int8_t rssi;
} s_target;

static bool is_saved(const char *ssid)
{
    for (int i = 0; i < s_n_saved; i++) {
        if (!strcmp(s_saved[i], ssid)) {
            return true;
        }
    }
    return false;
}

static bool is_connected(const char *ssid)
{
    return s_st.state == NET_UP && !strcmp(s_st.ssid, ssid);
}

// ---- Scan

typedef struct {
    net_ap_t aps[SCAN_MAX];
    int n;
    esp_err_t err;
    char saved[NET_MAX_SAVED][33];
    int n_saved;
    net_status_t st;
} scan_job_t;

static void scan_job(void *arg)
{
    scan_job_t *j = arg;
    j->err = net_scan(5000, j->aps, SCAN_MAX, &j->n);
    j->n_saved = net_saved_list(j->saved, NET_MAX_SAVED);
    net_status(&j->st);
}

static void w_fill(void);

static void scan_done(void *arg)
{
    scan_job_t *j = arg;
    if (s_scanning) {
        s_scanning = false;
        ui_busy(false);
    }
    if (j->err == ESP_OK) {
        // strongest first (the driver sorts too, but the list must not depend on it)
        s_n_aps = 0;
        for (int i = 0; i < j->n && s_n_aps < SCAN_MAX; i++) {
            ap_t a = { .rssi = j->aps[i].rssi, .auth = j->aps[i].auth };
            strlcpy(a.ssid, j->aps[i].ssid, sizeof(a.ssid));
            int k = s_n_aps++;
            while (k > 0 && s_aps[k - 1].rssi < a.rssi) {
                s_aps[k] = s_aps[k - 1];
                k--;
            }
            s_aps[k] = a;
        }
    }
    memcpy(s_saved, j->saved, sizeof(s_saved));
    s_n_saved = j->n_saved;
    s_st = j->st;
    if (s_w_scr) {
        ui_row_set_sub(s_w_rescan, j->err == ESP_OK ? NULL : ui_s(S_ERR_LOAD));
        w_fill();
    }
}

static void scan_start(void)
{
    if (s_scanning) {
        return;
    }
    scan_job_t *j = ui_work_alloc(sizeof(scan_job_t));
    if (!j) {
        return;
    }
    if (ui_work(scan_job, scan_done, j)) {
        s_scanning = true;
        ui_busy(true);
        if (s_w_scr) {
            ui_row_set_sub(s_w_rescan, ui_s(S_W_SCANNING));
        }
    }
}

// ---- W rows

static void pass_done(const char *text, bool ok, void *ctx)
{
    (void)ctx;
    if (ok) {
        ui_wifi_connect(s_target.ssid, text);
    }
}

static void ask_password(void)
{
    char prompt[64];
    snprintf(prompt, sizeof(prompt), ui_s(S_K_PASS_FMT), s_target.ssid);
    ui_text_open(&(ui_text_req_t){ .field = UI_FIELD_WIFI_PASS, .prompt = prompt, .done = pass_done });
}

static void target_set(const char *ssid, int rssi, bool secured)
{
    strlcpy(s_target.ssid, ssid, sizeof(s_target.ssid));
    s_target.rssi = (int8_t)rssi;
    s_target.secured = secured;
    s_target.saved = is_saved(ssid);
    s_target.connected = is_connected(ssid);
}

static void net_cb(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i >= 100) {
        // saved network out of range
        int k = (int)(i - 100);
        if (k < s_n_saved) {
            target_set(s_saved[k], -127, true);
            ui_nav_push(UI_SCR_WIFI_NET, 0);
        }
        return;
    }
    if (i < 0 || i >= s_n_aps) {
        return;
    }
    const ap_t *a = &s_aps[i];
    target_set(a->ssid, a->rssi, a->auth != WIFI_AUTH_OPEN);
    if (s_target.saved || s_target.connected) {
        ui_nav_push(UI_SCR_WIFI_NET, 0);
    } else if (!s_target.secured) {
        ui_wifi_connect(a->ssid, "");
    } else {
        ask_password();
    }
}

static void ssid_done(const char *text, bool ok, void *ctx)
{
    (void)ctx;
    if (ok && text[0]) {
        target_set(text, -127, true);
        ask_password();
    }
}

static void other_cb(lv_event_t *e)
{
    (void)e;
    ui_text_open(&(ui_text_req_t){ .field = UI_FIELD_SSID, .prompt = ui_s(S_K_SSID), .done = ssid_done });
}

static void phone_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_push(UI_SCR_QR, 0);
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    scan_start();
}

static void w_fill(void)
{
    ui_list_trim(s_w_list, 1);      // keep "Искать снова"
    char name[24];
    int shown = 0;
    bool conn_listed = false;
    for (int i = 0; i < s_n_aps && shown < SHOW_MAX; i++) {
        const ap_t *a = &s_aps[i];
        bool conn = is_connected(a->ssid);
        conn_listed |= conn;
        const char *sub = conn ? ui_s(S_W_CONNECTED) : is_saved(a->ssid) ? ui_s(S_W_SAVED)
                          : a->auth == WIFI_AUTH_OPEN ? ui_s(S_W_OPEN) : ui_s(S_W_SECURED);
        snprintf(name, sizeof(name), "w_net_%d", i);
        lv_obj_t *r = ui_list_row(s_w_list, name, NULL, a->ssid, sub, UI_ROW_WIFI, net_cb, (void *)(intptr_t)i);
        ui_row_set_wifi(r, a->rssi, a->auth != WIFI_AUTH_OPEN);
        shown++;
    }
    for (int k = 0; k < s_n_saved; k++) {
        bool seen = false;
        for (int i = 0; i < s_n_aps && !seen; i++) {
            seen = !strcmp(s_aps[i].ssid, s_saved[k]);
        }
        if (seen) {
            continue;
        }
        bool conn = is_connected(s_saved[k]) && !conn_listed;
        snprintf(name, sizeof(name), "w_saved_%d", k);
        ui_list_row(s_w_list, name, NULL, s_saved[k], ui_s(conn ? S_W_CONNECTED : S_W_AWAY), UI_ROW_CHEVRON,
                    net_cb, (void *)(intptr_t)(100 + k));
    }
    if (shown == 0 && s_n_saved == 0 && !s_scanning) {
        ui_list_row(s_w_list, "w_none", NULL, ui_s(S_W_NONE), NULL, UI_ROW_NONE, NULL, NULL);
    }
    ui_list_row(s_w_list, "w_other", NULL, ui_s(S_W_OTHER), NULL, UI_ROW_CHEVRON, other_cb, NULL);
    ui_list_row(s_w_list, "w_phone", SYM_QRCODE, ui_s(S_W_PHONE), NULL, UI_ROW_CHEVRON, phone_cb, NULL);
    ui_list_refresh(s_w_list);
}

static void w_loaded_cb(lv_event_t *e)
{
    (void)e;
    scan_start();       // on entry and on every return (after a connect or a forget)
}

static void w_delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s_w_scr) {
        s_w_scr = s_w_list = s_w_rescan = NULL;
    }
}

lv_obj_t *scr_wifi_build(uint32_t arg)
{
    (void)arg;
    s_w_scr = ui_list_screen("wifi", ui_s(S_T_WIFI), &s_w_list);
    lv_obj_add_event_cb(s_w_scr, w_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(s_w_scr, w_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    s_w_rescan = ui_list_row(s_w_list, "w_rescan", SYM_SYNC, ui_s(S_W_RESCAN), NULL, UI_ROW_NONE, rescan_cb, NULL);
    scan_start();       // before the rows: no "Нет сетей" flash while the first scan runs
    w_fill();           // the previous scan until the new one arrives
    return s_w_scr;
}

// ---- One network

typedef struct {
    char ssid[33];
    esp_err_t err;
} forget_job_t;

static void forget_job(void *arg)
{
    forget_job_t *j = arg;
    j->err = net_forget(j->ssid);
}

static void forget_done(void *arg)
{
    forget_job_t *j = arg;
    if (j->err == ESP_OK) {
        for (int i = 0; i < s_n_saved; i++) {
            if (!strcmp(s_saved[i], j->ssid)) {
                memmove(s_saved[i], s_saved[i + 1], sizeof(s_saved[0]) * (size_t)(s_n_saved - i - 1));
                s_n_saved--;
                break;
            }
        }
        ui_toast(ui_s(S_N_FORGOTTEN));
    } else {
        ui_toast(ui_s(S_NOT_SAVED));
    }
    if (ui_nav_current() == UI_SCR_WIFI_NET) {
        ui_nav_back();
    }
}

static void forget_confirm(void *ctx)
{
    (void)ctx;
    forget_job_t *j = ui_work_alloc(sizeof(forget_job_t));
    if (j) {
        strlcpy(j->ssid, s_target.ssid, sizeof(j->ssid));
        ui_work(forget_job, forget_done, j);
    }
}

static void forget_cb(lv_event_t *e)
{
    (void)e;
    ui_dialog_open(&(ui_dialog_t){ .title = ui_s(S_N_FORGET_Q), .body = s_target.ssid,
                                   .confirm = ui_s(S_N_FORGET_BTN), .danger = true, .on_confirm = forget_confirm });
}

static void connect_saved_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_connect(s_target.ssid, NULL);
}

static void password_cb(lv_event_t *e)
{
    (void)e;
    ask_password();
}

lv_obj_t *scr_wifi_net_build(uint32_t arg)
{
    (void)arg;
    if (!s_target.ssid[0]) {
        return NULL;        // only reachable from W
    }
    lv_obj_t *list;
    lv_obj_t *scr = ui_list_screen("wifinet", s_target.ssid, &list);
    char buf[48];
    ui_list_info(list, "n_status", ui_s(S_N_STATUS),
                 ui_s(s_target.connected ? S_W_CONNECTED : s_target.saved ? S_W_SAVED : S_W_SECURED));
    if (s_target.connected) {
        ui_list_info(list, "n_ip", ui_s(S_N_IP), s_st.ip[0] ? s_st.ip : ui_s(S_A_NONE));
        snprintf(buf, sizeof(buf), "%d dBm", (int)s_st.rssi);
        ui_list_info(list, "n_signal", ui_s(S_N_SIGNAL), buf);
    } else if (s_target.saved) {
        ui_list_row(list, "n_connect", SYM_WIFI, ui_s(S_N_CONNECT), NULL, UI_ROW_NONE, connect_saved_cb, NULL);
    }
    if (s_target.secured) {
        ui_list_row(list, "n_password", SYM_KEY, ui_s(S_N_PASSWORD), NULL, UI_ROW_CHEVRON, password_cb, NULL);
    }
    if (s_target.saved) {
        lv_obj_t *r = ui_list_row(list, "n_forget", SYM_TRASH, ui_s(S_N_FORGET), NULL, UI_ROW_NONE, forget_cb, NULL);
        ui_row_set_danger(r);
    }
    ui_list_refresh(list);
    return scr;
}

// ---- WC: connecting

static struct {
    char ssid[33];
    char *pass;         // PSRAM, NULL = saved password
    uint32_t gen;
} s_conn;

static ui_step_t s_wc;
static lv_obj_t *s_wc_scr;
static lv_timer_t *s_wc_timer;
static int s_wc_result;         // 0 connecting, 1 ok, 2 wrong password, 3 not found, 4 other failure
static volatile int s_reason;   // last WIFI_EVENT_STA_DISCONNECTED reason (sys_evt task)
static bool s_reason_hooked;

static void reason_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    s_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
}

typedef struct {
    char ssid[33];
    char pass[65];
    bool saved;
    uint32_t gen;
    esp_err_t err;
    int reason;
} conn_job_t;

static void conn_job(void *arg)
{
    conn_job_t *j = arg;
    if (!s_reason_hooked) {
        // the default event loop exists once net_init() ran; registering twice is harmless but avoided
        s_reason_hooked = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, reason_cb, NULL) ==
                          ESP_OK;
    }
    s_reason = 0;
    j->err = j->saved ? net_connect_saved(j->ssid) : net_connect(j->ssid, j->pass, true);
    j->reason = s_reason;
    memset(j->pass, 0, sizeof(j->pass));
}

static void wc_show(void);

static void conn_done(void *arg)
{
    conn_job_t *j = arg;
    if (!s_wc_scr || j->gen != s_conn.gen) {
        return;     // the user left (Отмена) or started another attempt
    }
    if (j->err == ESP_OK) {
        s_wc_result = 1;
    } else {
        int r = j->reason;
        bool bad_pass = r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                        r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_AUTH_EXPIRE ||
                        r == WIFI_REASON_MIC_FAILURE;
        bool not_found = r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
                         r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
                         r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
        s_wc_result = bad_pass ? 2 : not_found ? 3 : 4;
    }
    wc_show();
}

static void conn_start(void)
{
    conn_job_t *j = ui_work_alloc(sizeof(conn_job_t));
    if (!j) {
        return;
    }
    strlcpy(j->ssid, s_conn.ssid, sizeof(j->ssid));
    j->saved = !s_conn.pass;
    if (s_conn.pass) {
        strlcpy(j->pass, s_conn.pass, sizeof(j->pass));
    }
    j->gen = ++s_conn.gen;
    s_wc_result = 0;
    ui_work(conn_job, conn_done, j);
}

void ui_wifi_connect(const char *ssid, const char *pass)
{
    strlcpy(s_conn.ssid, ssid ? ssid : "", sizeof(s_conn.ssid));
    if (s_conn.pass) {
        memset(s_conn.pass, 0, strlen(s_conn.pass));
        heap_caps_free(s_conn.pass);
        s_conn.pass = NULL;
    }
    if (pass) {
        size_t n = strlen(pass) + 1;
        s_conn.pass = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (s_conn.pass) {
            memcpy(s_conn.pass, pass, n);
        }
    }
    ui_nav_push(UI_SCR_WIFI_CONN, 0);
}

static void wc_back_timer(lv_timer_t *t)
{
    (void)t;
    s_wc_timer = NULL;
    if (ui_nav_current() != UI_SCR_WIFI_CONN) {
        return;
    }
    if (ui_onboarding()) {
        ui_onboard_resume();
        return;
    }
    ui_nav_back();
    if (ui_nav_current() == UI_SCR_WIFI_NET) {
        ui_nav_back();      // the detail screen is stale now: straight back to the list
    }
}

static void wc_show(void)
{
    char sub[64];
    snprintf(sub, sizeof(sub), ui_s(S_WC_SUB_FMT), s_conn.ssid);
    switch (s_wc_result) {
    case 0:
        ui_orb_set_state(s_wc.orb, UI_ST_THINK);
        ui_step_set(&s_wc, ui_s(S_WC_TITLE), sub, NULL, ui_s(S_CANCEL));
        break;
    case 1:
        ui_orb_set_state(s_wc.orb, UI_ST_IDLE);
        ui_orb_set_expr(s_wc.orb, EXPR_HAPPY, 1500);
        ui_step_set(&s_wc, ui_s(S_WC_OK), sub, NULL, NULL);
        ui_toast(ui_s(S_DONE));
        if (!s_wc_timer) {
            // back after 1.2 s (3.12)
            s_wc_timer = lv_timer_create(wc_back_timer, 1200, NULL);
            lv_timer_set_repeat_count(s_wc_timer, 1);
        }
        break;
    case 2:
        ui_orb_set_state(s_wc.orb, UI_ST_IDLE);
        ui_orb_set_expr(s_wc.orb, EXPR_SOFT, 0);
        ui_step_set(&s_wc, ui_s(S_WC_BAD_PASS), sub, ui_s(S_WC_AGAIN), ui_s(S_WC_OTHER));
        break;
    case 3:
        ui_orb_set_state(s_wc.orb, UI_ST_IDLE);
        ui_orb_set_expr(s_wc.orb, EXPR_SOFT, 0);
        ui_step_set(&s_wc, ui_s(S_WC_NOT_FOUND), sub, ui_s(S_W_RESCAN), ui_s(S_CANCEL));
        break;
    default:
        ui_orb_set_state(s_wc.orb, UI_ST_IDLE);
        ui_orb_set_expr(s_wc.orb, EXPR_SOFT, 0);
        ui_step_set(&s_wc, ui_s(S_WC_FAILED), sub, ui_s(S_C_RETRY_A), ui_s(S_WC_OTHER));
        break;
    }
}

static void wc_btn_cb(lv_event_t *e)
{
    bool primary = (intptr_t)lv_event_get_user_data(e) == 1;
    if (!primary || s_wc_result == 3) {
        // Отмена / Другая сеть / Искать снова: back to the list (it rescans when it is shown again)
        ui_nav_back();
        return;
    }
    if (s_wc_result == 2) {
        strlcpy(s_target.ssid, s_conn.ssid, sizeof(s_target.ssid));
        ui_nav_back();      // K replaces WC: W -> K -> WC
        ask_password();
        return;
    }
    if (s_wc_result == 4) {
        conn_start();
        wc_show();
    }
}

static void wc_delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != s_wc_scr) {
        return;
    }
    s_wc_scr = NULL;
    s_conn.gen++;           // a late result is ignored
    if (s_wc_timer) {
        lv_timer_delete(s_wc_timer);
        s_wc_timer = NULL;
    }
}

lv_obj_t *scr_wifi_conn_build(uint32_t arg)
{
    (void)arg;
    if (!s_conn.ssid[0]) {
        return NULL;        // needs ui_wifi_connect()
    }
    s_wc_scr = ui_step_screen("wificonn", &s_wc, 0, wc_btn_cb);
    lv_obj_add_event_cb(s_wc_scr, wc_delete_cb, LV_EVENT_DELETE, NULL);
    conn_start();
    wc_show();
    return s_wc_scr;
}
