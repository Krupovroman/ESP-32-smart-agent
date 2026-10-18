// D "О системе" (docs/ui.md 3.7) and the licences list: firmware and slot, OTA / rollback state, versions,
// panel id, network, uptime, free memory, last turn; touch test, hints, restart and factory reset. The values
// are collected in the UI worker (OTA data and NVS reads, Wi-Fi driver calls).
#include "ui_internal.h"
#include "ui_list.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_work.h"
#include "ai_client.h"
#include "display.h"
#include "net.h"
#include "ota.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint32_t gen;
    ota_info_t o;
    net_status_t st;
    uint8_t mac[6];
    size_t sram, psram;
    int64_t up_us;
    bool has_last;
    ai_last_t last;
} about_job_t;

enum { I_FW, I_STATE, I_VERSIONS, I_PANEL, I_MAC, I_IP, I_WIFI, I_UPTIME, I_MEM, I_LATENCY, I_ERROR, I__N };

static lv_obj_t *s_scr, *s_info[I__N];
static uint32_t s_gen;

static void about_job(void *arg)
{
    about_job_t *j = arg;
    ota_get_info(&j->o);
    net_status(&j->st);
    esp_read_mac(j->mac, ESP_MAC_WIFI_STA);
    j->sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    j->psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    j->up_us = esp_timer_get_time();
    j->has_last = ai_client_last(&j->last);
}

static void secs(uint32_t ms, char *out, size_t len)
{
    snprintf(out, len, "%u.%u", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
}

static void about_done(void *arg)
{
    about_job_t *j = arg;
    if (!s_scr || j->gen != s_gen) {
        return;
    }
    char buf[96], a[16], b[16];
    snprintf(buf, sizeof(buf), "%s " TXT_MIDDOT " %s", j->o.ver, j->o.running);
    ui_info_set(s_info[I_FW], buf);
    snprintf(buf, sizeof(buf), "%s%s%s", j->o.state_known ? ota_state_name(j->o.state) : "usb",
             j->o.rollback_possible ? " " TXT_MIDDOT " " : "", j->o.rollback_possible ? ui_s(S_A_ROLLBACK) : "");
    ui_info_set(s_info[I_STATE], buf);
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", j->mac[0], j->mac[1], j->mac[2], j->mac[3],
             j->mac[4], j->mac[5]);
    ui_info_set(s_info[I_MAC], buf);
    ui_info_set(s_info[I_IP], j->st.ip[0] ? j->st.ip : ui_s(S_A_NONE));
    if (j->st.state == NET_UP) {
        // SSIDs are long: the signal goes into the caption
        snprintf(buf, sizeof(buf), "Wi-Fi " TXT_MIDDOT " %d dBm", (int)j->st.rssi);
        ui_row_set_title(s_info[I_WIFI], buf);
        ui_info_set(s_info[I_WIFI], j->st.ssid);
    } else {
        ui_info_set(s_info[I_WIFI], ui_s(S_S_NOT_CONNECTED));
    }
    uint32_t s = (uint32_t)(j->up_us / 1000000);
    snprintf(buf, sizeof(buf), "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    ui_info_set(s_info[I_UPTIME], buf);
    // internal SRAM, then PSRAM
    snprintf(buf, sizeof(buf), "%u KB " TXT_MIDDOT " %u.%u MB", (unsigned)(j->sram / 1024),
             (unsigned)(j->psram / (1024 * 1024)), (unsigned)(j->psram % (1024 * 1024) * 10 / (1024 * 1024)));
    ui_info_set(s_info[I_MEM], buf);
    if (j->has_last && j->last.valid) {
        secs(j->last.t.upload_ms, a, sizeof(a));
        secs(j->last.t.ai_ms, b, sizeof(b));
        snprintf(buf, sizeof(buf), ui_s(S_A_LAT_FMT), a, b);
        ui_info_set(s_info[I_LATENCY], buf);
        ui_info_set(s_info[I_ERROR], j->last.ok ? ui_s(S_A_NONE) : j->last.err_msg);
    }
}

static void restart_job(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void reset_job(void *arg)
{
    (void)arg;
    settings_reset();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void reboot_confirm(void *ctx)
{
    (void)ctx;
    ui_save_flush();                    // pending settings first (the worker runs jobs in order)
    ui_work(restart_job, NULL, NULL);
}

static void reset_confirm(void *ctx)
{
    (void)ctx;
    ui_work(reset_job, NULL, NULL);
}

static void act_cb(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0: ui_nav_push(UI_SCR_TOUCH, 0); break;
    case 1:
        ui_set(g_s_hints, 3);           // "Показать подсказки" (5.9)
        ui_emit(UI_IN_HINTS, 3);        // the app's counter is the one that counts down
        ui_toast(ui_s(S_A_HINTS_ON));
        break;
    case 2: ui_nav_push(UI_SCR_LICENSES, 0); break;
    case 3:
        ui_dialog_open(&(ui_dialog_t){ .title = ui_s(S_A_REBOOT_Q), .confirm = ui_s(S_A_YES),
                                       .on_confirm = reboot_confirm });
        break;
    case 4:
        ui_dialog_open(&(ui_dialog_t){ .title = ui_s(S_A_RESET_Q), .body = ui_s(S_A_RESET_B),
                                       .confirm = ui_s(S_A_RESET_BTN), .danger = true, .on_confirm = reset_confirm });
        break;
    default: break;
    }
}

static void loaded_cb(lv_event_t *e)
{
    (void)e;
    about_job_t *j = ui_work_alloc(sizeof(about_job_t));
    if (j) {
        j->gen = s_gen;
        ui_work(about_job, about_done, j);
    }
}

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == s_scr) {
        s_scr = NULL;
        s_gen++;
        memset(s_info, 0, sizeof(s_info));
    }
}

lv_obj_t *scr_about_build(uint32_t arg)
{
    (void)arg;
    static const struct {
        const char *name;
        ui_sid_t cap;
        const char *fixed;
    } infos[I__N] = {
        [I_FW] = { "a_fw", S_A_FW, NULL }, [I_STATE] = { "a_state", S_N_STATUS, NULL },
        [I_VERSIONS] = { "a_versions", S__N, "ESP-IDF " TXT_MIDDOT " LVGL" }, [I_PANEL] = { "a_panel", S_A_PANEL, NULL },
        [I_MAC] = { "a_mac", S__N, "MAC" }, [I_IP] = { "a_ip", S_N_IP, NULL },
        [I_WIFI] = { "a_wifi", S__N, "Wi-Fi" }, [I_UPTIME] = { "a_uptime", S_A_UPTIME, NULL },
        [I_MEM] = { "a_mem", S_A_MEM, NULL }, [I_LATENCY] = { "a_latency", S_A_LATENCY, NULL },
        [I_ERROR] = { "a_error", S_A_LAST_ERR, NULL },
    };
    lv_obj_t *list;
    s_gen++;
    s_scr = ui_list_screen("about", ui_s(S_T_ABOUT), &list);
    lv_obj_add_event_cb(s_scr, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, delete_cb, LV_EVENT_DELETE, NULL);
    for (int i = 0; i < I__N; i++) {
        s_info[i] = ui_list_info(list, infos[i].name, infos[i].fixed ? infos[i].fixed : ui_s(infos[i].cap),
                                 ui_s(S_A_NONE));
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%s " TXT_MIDDOT " %d.%d.%d", esp_get_idf_version(), LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    ui_info_set(s_info[I_VERSIONS], buf);
    const display_info_t *d = display_info();
    if (d) {
        snprintf(buf, sizeof(buf), "%02X %02X %02X %02X " TXT_MIDDOT " %d", d->panel_id[0], d->panel_id[1],
                 d->panel_id[2], d->panel_id[3], d->panel_batch);
        ui_info_set(s_info[I_PANEL], buf);
    }
    ui_list_row(list, "a_touch", SYM_MOBILE, ui_s(S_A_TOUCH), NULL, UI_ROW_CHEVRON, act_cb, (void *)0);
    ui_list_row(list, "a_hints", SYM_INFO, ui_s(S_A_HINTS), ui_s(S_A_HINTS_SUB), UI_ROW_NONE, act_cb, (void *)1);
    ui_list_row(list, "a_licenses", SYM_INFO, ui_s(S_T_LICENSES), NULL, UI_ROW_CHEVRON, act_cb, (void *)2);
    ui_list_row(list, "a_reboot", SYM_POWER, ui_s(S_A_REBOOT), NULL, UI_ROW_NONE, act_cb, (void *)3);
    lv_obj_t *r = ui_list_row(list, "a_reset", SYM_TRASH, ui_s(S_A_RESET), NULL, UI_ROW_NONE, act_cb, (void *)4);
    ui_row_set_danger(r);
    ui_list_refresh(list);
    return s_scr;
}

lv_obj_t *scr_licenses_build(uint32_t arg)
{
    (void)arg;
    static const char *const lic[][2] = {
        { "LVGL", "MIT" },
        { "Inter", "SIL OFL 1.1" },
        { "Font Awesome 5 Free", "OFL 1.1 " TXT_MIDDOT " CC BY 4.0" },
        { "xiaozhi-esp32", "MIT" },
        { "ESP-IDF", "Apache 2.0" },
        { "esp_lvgl_port", "Apache 2.0" },
        { "cJSON", "MIT" },
    };
    lv_obj_t *list;
    lv_obj_t *scr = ui_list_screen("licenses", ui_s(S_T_LICENSES), &list);
    for (size_t i = 0; i < sizeof(lic) / sizeof(lic[0]); i++) {
        char name[16];
        snprintf(name, sizeof(name), "lic_%u", (unsigned)i);
        ui_list_info(list, name, lic[i][0], lic[i][1]);
    }
    ui_list_refresh(list);
    return scr;
}
