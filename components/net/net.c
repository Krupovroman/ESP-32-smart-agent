#include "net.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "net";
static const char *NS = "wifi";

#define BIT_UP      BIT0
#define BIT_FAIL    BIT1
#define BIT_SCANNED BIT2

typedef struct {
    char ssid[33];
    char pass[65];
} saved_t;

static esp_netif_t *s_sta;
static EventGroupHandle_t s_evt;
static SemaphoreHandle_t s_lock;
static saved_t s_saved[NET_MAX_SAVED];
static int s_n_saved;
static volatile net_state_t s_state = NET_OFF;
static char s_cur_ssid[33];
static char s_ip[16];
static bool s_auto = false;          // reconnect automatically after drops
static int s_backoff_s = 1;
static TimerHandle_t s_retry_timer;
static TaskHandle_t s_retry_task;
static bool s_sntp_started;

// Scan + connect need ~1.5 KB of stack and block up to 4 s: they run in this task, not in the
// 2 KB FreeRTOS timer service (overflowed on every Wi-Fi drop) and not in the event loop.
#define RETRY_STACK 4096
#define RETRY_PRIO  4

const char *net_state_name(net_state_t s)
{
    switch (s) {
    case NET_OFF: return "off";
    case NET_CONNECTING: return "connecting";
    case NET_UP: return "up";
    case NET_FAILED: return "failed";
    }
    return "?";
}

static void load_saved(void)
{
    nvs_handle_t h;
    s_n_saved = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    for (int i = 0; i < n && i < NET_MAX_SAVED; i++) {
        char k[16];
        size_t len = sizeof(s_saved[0].ssid);
        snprintf(k, sizeof(k), "ssid%d", i);
        if (nvs_get_str(h, k, s_saved[s_n_saved].ssid, &len) != ESP_OK) {
            continue;
        }
        len = sizeof(s_saved[0].pass);
        snprintf(k, sizeof(k), "pass%d", i);
        if (nvs_get_str(h, k, s_saved[s_n_saved].pass, &len) != ESP_OK) {
            s_saved[s_n_saved].pass[0] = 0;
        }
        s_n_saved++;
    }
    nvs_close(h);
}

static esp_err_t store_saved(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    nvs_erase_all(h);
    nvs_set_u8(h, "n", (uint8_t)s_n_saved);
    for (int i = 0; i < s_n_saved; i++) {
        char k[16];
        snprintf(k, sizeof(k), "ssid%d", i);
        nvs_set_str(h, k, s_saved[i].ssid);
        snprintf(k, sizeof(k), "pass%d", i);
        nvs_set_str(h, k, s_saved[i].pass);
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void remember(const char *ssid, const char *pass)
{
    saved_t e = {0};
    strlcpy(e.ssid, ssid, sizeof(e.ssid));
    strlcpy(e.pass, pass ? pass : "", sizeof(e.pass));
    int keep = 0;
    saved_t tmp[NET_MAX_SAVED];
    tmp[keep++] = e;
    for (int i = 0; i < s_n_saved && keep < NET_MAX_SAVED; i++) {
        if (strcmp(s_saved[i].ssid, ssid)) {
            tmp[keep++] = s_saved[i];
        }
    }
    memcpy(s_saved, tmp, sizeof(saved_t) * keep);
    s_n_saved = keep;
    store_saved();
    memset(tmp, 0, sizeof(tmp));
}

static void start_sta(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    s_state = NET_CONNECTING;
    xEventGroupClearBits(s_evt, BIT_UP | BIT_FAIL);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    memset(&cfg, 0, sizeof(cfg));
}

// Picks the strongest visible saved network, or the most recent one when the scan finds none.
static void connect_best_saved(void)
{
    if (s_n_saved == 0) {
        s_state = NET_OFF;
        return;
    }
    int best = 0;
    net_ap_t aps[16];
    int n = 0;
    esp_err_t scanned = net_scan(4000, aps, 16, &n);
    if (!s_auto) {
        return;              // net_connect() / net_disconnect() took over during the scan
    }
    if (scanned == ESP_OK) {
        int best_rssi = -127;
        for (int i = 0; i < s_n_saved; i++) {
            for (int k = 0; k < n; k++) {
                if (!strcmp(aps[k].ssid, s_saved[i].ssid) && aps[k].rssi > best_rssi) {
                    best_rssi = aps[k].rssi;
                    best = i;
                }
            }
        }
    }
    ESP_LOGI(TAG, "connecting to \"%s\"", s_saved[best].ssid);
    start_sta(s_saved[best].ssid, s_saved[best].pass);
}

static void retry_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        // checked under the lock: net_connect() clears s_auto before it waits for the lock
        if (s_auto && s_state != NET_UP) {
            connect_best_saved();
        }
        xSemaphoreGive(s_lock);
    }
}

// Timer service context (2 KB stack): only wake the retry task.
static void retry_cb(TimerHandle_t t)
{
    if (s_retry_task) {
        xTaskNotifyGive(s_retry_task);
    }
}

// Created with the first automatic connection, after the boot self-test measured the heap.
// Caller holds s_lock.
static void ensure_retry_task(void)
{
    if (!s_retry_task && xTaskCreate(retry_task, "net_retry", RETRY_STACK, NULL, RETRY_PRIO, &s_retry_task) != pdPASS) {
        s_retry_task = NULL;
        ESP_LOGE(TAG, "no memory for the reconnect task: no automatic reconnects");
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (d->reason == WIFI_REASON_ASSOC_LEAVE && s_state == NET_CONNECTING) {
            // start_sta() left the previous network itself (esp_wifi_disconnect); this event arrives after the
            // bits were cleared and is not the result of the attempt just started, which reports its own
            s_ip[0] = 0;
            xEventGroupClearBits(s_evt, BIT_UP);
            ESP_LOGI(TAG, "left the previous network for \"%s\"", s_cur_ssid);
            return;
        }
        bool was_up = s_state == NET_UP;
        s_state = NET_FAILED;
        s_ip[0] = 0;
        xEventGroupClearBits(s_evt, BIT_UP);
        xEventGroupSetBits(s_evt, BIT_FAIL);
        ESP_LOGW(TAG, "disconnected from \"%s\" (reason %d)", s_cur_ssid, d->reason);
        if (s_auto) {
            s_backoff_s = was_up ? 1 : (s_backoff_s * 2 > 60 ? 60 : s_backoff_s * 2);
            xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_backoff_s * 1000), 0);
            xTimerStart(s_retry_timer, 0);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_evt, BIT_SCANNED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_state = NET_UP;
        s_backoff_s = 1;
        xEventGroupSetBits(s_evt, BIT_UP);
        ESP_LOGI(TAG, "up: \"%s\" ip %s", s_cur_ssid, s_ip);
        if (!s_sntp_started) {
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sc);
            s_sntp_started = true;
        }
    }
}

esp_err_t net_init(void)
{
    s_evt = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    s_retry_timer = xTimerCreate("net_retry", pdMS_TO_TICKS(1000), pdFALSE, NULL, retry_cb);
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, "round-assistant");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    load_saved();
    ESP_LOGI(TAG, "wifi ready, %d saved network(s)", s_n_saved);
    return ESP_OK;
}

esp_err_t net_start(void)
{
    s_auto = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ensure_retry_task();
    connect_best_saved();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t net_connect(const char *ssid, const char *pass, bool save)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0] && strlen(ssid) <= 32, ESP_ERR_INVALID_ARG, TAG, "ssid");
    ESP_RETURN_ON_FALSE(!pass || strlen(pass) <= 64, ESP_ERR_INVALID_ARG, TAG, "pass");
    bool was_auto = s_auto;
    s_auto = false;              // a manual attempt must not be raced by the retry timer
    xTimerStop(s_retry_timer, 0);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ensure_retry_task();
    start_sta(ssid, pass);
    xSemaphoreGive(s_lock);
    EventBits_t b = xEventGroupWaitBits(s_evt, BIT_UP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    bool up = b & BIT_UP;
    if (up && save) {
        remember(ssid, pass);
    }
    s_auto = was_auto || up;
    if (!up) {
        ESP_LOGW(TAG, "connect to \"%s\" failed", ssid);
        if (s_auto) {
            xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(3000), 0);
            xTimerStart(s_retry_timer, 0);
        }
    }
    return up ? ESP_OK : ESP_FAIL;
}

esp_err_t net_connect_saved(const char *ssid)
{
    char pass[65] = "";
    bool found = false;
    for (int i = 0; i < s_n_saved && !found; i++) {
        if (!strcmp(s_saved[i].ssid, ssid)) {
            strlcpy(pass, s_saved[i].pass, sizeof(pass));
            found = true;
        }
    }
    ESP_RETURN_ON_FALSE(found, ESP_ERR_NOT_FOUND, TAG, "not saved");
    esp_err_t err = net_connect(ssid, pass, true);
    memset(pass, 0, sizeof(pass));
    return err;
}

esp_err_t net_disconnect(void)
{
    s_auto = false;
    xTimerStop(s_retry_timer, 0);
    s_state = NET_OFF;
    return esp_wifi_disconnect();
}

esp_err_t net_forget(const char *ssid)
{
    int keep = 0;
    for (int i = 0; i < s_n_saved; i++) {
        if (strcmp(s_saved[i].ssid, ssid)) {
            s_saved[keep++] = s_saved[i];
        }
    }
    ESP_RETURN_ON_FALSE(keep != s_n_saved, ESP_ERR_NOT_FOUND, TAG, "not saved");
    s_n_saved = keep;
    return store_saved();
}

esp_err_t net_scan(uint32_t timeout_ms, net_ap_t *out, int max, int *n)
{
    *n = 0;
    xEventGroupClearBits(s_evt, BIT_SCANNED);
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    ESP_RETURN_ON_ERROR(err, TAG, "scan start");
    if (!(xEventGroupWaitBits(s_evt, BIT_SCANNED, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & BIT_SCANNED)) {
        esp_wifi_scan_stop();
        return ESP_ERR_TIMEOUT;
    }
    uint16_t cnt = 20;
    // 1.8 KB: on the heap, so callers with small stacks (the retry task) can scan
    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * cnt);
    if (!recs) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&cnt, recs);
    if (err != ESP_OK) {
        free(recs);
        ESP_LOGE(TAG, "records: %s", esp_err_to_name(err));
        return err;
    }
    for (int i = 0; i < cnt && *n < max; i++) {
        if (!recs[i].ssid[0]) {
            continue;
        }
        bool dup = false;    // same SSID from several access points: keep the strongest (list is sorted)
        for (int k = 0; k < *n; k++) {
            if (!strcmp(out[k].ssid, (char *)recs[i].ssid)) {
                dup = true;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(out[*n].ssid, (char *)recs[i].ssid, sizeof(out[0].ssid));
        out[*n].rssi = recs[i].rssi;
        out[*n].auth = recs[i].authmode;
        out[*n].ch = recs[i].primary;
        (*n)++;
    }
    free(recs);
    return ESP_OK;
}

void net_status(net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    strlcpy(out->ssid, s_cur_ssid, sizeof(out->ssid));
    strlcpy(out->ip, s_ip, sizeof(out->ip));
    out->saved = s_n_saved;
    if (s_state == NET_UP) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

bool net_wait_up(uint32_t timeout_ms)
{
    return xEventGroupWaitBits(s_evt, BIT_UP, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & BIT_UP;
}

int net_saved_list(char ssids[][33], int max)
{
    int n = 0;
    for (; n < s_n_saved && n < max; n++) {
        strlcpy(ssids[n], s_saved[n].ssid, 33);
    }
    return n;
}
