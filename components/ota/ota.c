#include "ota.h"

#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "ota";
static const char *NS = "sys";

#define OTA_TASK_STACK 8192      // internal: TLS handshake plus flash writes
#define OTA_TASK_PRIO  4

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static volatile bool s_streaming;
static volatile bool s_https_busy;
static ota_progress_cb_t s_cb;
static ota_selftest_t s_selftest = OTA_ST_NOT_RUN;
static char s_selftest_reason[64];
static uint32_t s_boot_cnt;

// ---------------------------------------------------------------- NVS "sys"

static void sys_get_str(const char *key, char *out, size_t len)
{
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l = len;
        if (nvs_get_str(h, key, out, &l) != ESP_OK) {
            out[0] = 0;
        }
        nvs_close(h);
    }
}

static esp_err_t sys_set_str(const char *key, const char *v)
{
    char cur[64];
    sys_get_str(key, cur, sizeof(cur));
    if (!strcmp(cur, v)) {
        return ESP_OK;          // unchanged: no flash write on every boot
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_str(h, key, v);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static uint16_t sys_get_u16(const char *key)
{
    uint16_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, key, &v);
        nvs_close(h);
    }
    return v;
}

esp_err_t ota_boot_count_bump(uint32_t *out)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    uint32_t v = 0, back = 0;
    nvs_get_u32(h, "boot_cnt", &v);
    esp_err_t err = nvs_set_u32(h, "boot_cnt", v + 1);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = nvs_get_u32(h, "boot_cnt", &back);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "boot_cnt");
    ESP_RETURN_ON_FALSE(back == v + 1, ESP_ERR_INVALID_CRC, TAG, "boot_cnt read back %u != %u",
                        (unsigned)back, (unsigned)(v + 1));
    s_boot_cnt = back;
    if (out) {
        *out = back;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- status

const char *ota_state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

const char *ota_selftest_name(ota_selftest_t s)
{
    switch (s) {
    case OTA_ST_OK: return "ok";
    case OTA_ST_FAIL: return "fail";
    case OTA_ST_SKIPPED: return "skipped";
    default: return "not_run";
    }
}

bool ota_pending_verify(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_get_info(ota_info_t *o)
{
    memset(o, 0, sizeof(*o));
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (run) {
        strlcpy(o->running, run->label, sizeof(o->running));
        o->state_known = esp_ota_get_state_partition(run, &o->state) == ESP_OK;
    }
    if (!o->state_known) {
        o->state = ESP_OTA_IMG_UNDEFINED;
    }
    if (next) {
        strlcpy(o->next, next->label, sizeof(o->next));
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(next, &d) == ESP_OK) {
            strlcpy(o->next_ver, d.version, sizeof(o->next_ver));
        }
    }
    if (boot) {
        strlcpy(o->boot, boot->label, sizeof(o->boot));
    }
    strlcpy(o->ver, esp_app_get_description()->version, sizeof(o->ver));
    sys_get_str("ok_ver", o->ok_ver, sizeof(o->ok_ver));
    sys_get_str("rb_reason", o->rb_reason, sizeof(o->rb_reason));
    o->rb_cnt = sys_get_u16("rb_cnt");
    o->boot_cnt = s_boot_cnt;
    // IDF judges rollback from the otadata entry selected for the next boot: while another
    // slot is selected (push with noreboot, ota boot) it would describe that slot, not this one
    o->rollback_possible = run && boot == run && esp_ota_check_rollback_is_possible();
    o->selftest = s_selftest;
    strlcpy(o->selftest_reason, s_selftest_reason, sizeof(o->selftest_reason));
    o->busy = s_streaming || s_https_busy;
}

// ---------------------------------------------------------------- boot confirmation

esp_err_t ota_confirm_boot(bool ok, const char *reason)
{
    s_selftest = ok ? OTA_ST_OK : OTA_ST_FAIL;
    strlcpy(s_selftest_reason, reason ? reason : "", sizeof(s_selftest_reason));
    bool pending = ota_pending_verify();
    if (ok) {
        if (pending) {
            ESP_LOGI(TAG, "self-test passed: image confirmed");
            ESP_RETURN_ON_ERROR(esp_ota_mark_app_valid_cancel_rollback(), TAG, "mark valid");
        }
        return sys_set_str("ok_ver", esp_app_get_description()->version);
    }
    sys_set_str("rb_reason", reason ? reason : "selftest");
    if (!pending) {
        ESP_LOGE(TAG, "self-test failed (%s); USB-flashed image keeps running", reason ? reason : "?");
        return ESP_FAIL;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "rb_cnt", (uint16_t)(sys_get_u16("rb_cnt") + 1));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGE(TAG, "self-test failed (%s): rolling back", reason ? reason : "?");
    vTaskDelay(pdMS_TO_TICKS(200));    // let the log reach the host
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return ESP_FAIL;                   // only reached when no previous image exists
}

void ota_selftest_skipped(const char *reason)
{
    s_selftest = OTA_ST_SKIPPED;
    strlcpy(s_selftest_reason, reason ? reason : "", sizeof(s_selftest_reason));
}

// After "idf.py flash" otadata is blank, so the running slot has no entry. An update would then
// write only the new slot's entry and leave no previous image: a failed self-test could not
// roll back (the bad image keeps running) and "ota rollback" is refused. Before the first update
// the running image therefore gets a valid entry of its own.
static esp_err_t anchor_running_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "%s has no otadata entry: recording it as the valid previous image", run->label);
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(run), TAG, "anchor %s", run->label);
    return esp_ota_mark_app_valid_cancel_rollback();   // set_boot leaves it NEW
}

// ---------------------------------------------------------------- console push

esp_err_t ota_stream_begin(size_t size)
{
    ESP_RETURN_ON_FALSE(!s_streaming && !s_https_busy, ESP_ERR_INVALID_STATE, TAG, "update already running");
    ESP_RETURN_ON_ERROR(anchor_running_slot(), TAG, "otadata");
    s_part = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(s_part, ESP_ERR_NOT_FOUND, TAG, "no OTA slot");
    ESP_RETURN_ON_FALSE(size > 0 && size <= s_part->size, ESP_ERR_INVALID_SIZE, TAG, "image %u > slot %u",
                        (unsigned)size, (unsigned)s_part->size);
    ESP_RETURN_ON_ERROR(esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle), TAG, "begin");
    s_streaming = true;
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)size, s_part->label);
    return ESP_OK;
}

esp_err_t ota_stream_write(const void *p, size_t n)
{
    ESP_RETURN_ON_FALSE(s_streaming, ESP_ERR_INVALID_STATE, TAG, "not started");
    return esp_ota_write(s_handle, p, n);
}

esp_err_t ota_stream_end(bool set_boot)
{
    ESP_RETURN_ON_FALSE(s_streaming, ESP_ERR_INVALID_STATE, TAG, "not started");
    s_streaming = false;
    ESP_RETURN_ON_ERROR(esp_ota_end(s_handle), TAG, "image check");
    if (set_boot) {
        ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(s_part), TAG, "set boot");
        ESP_LOGI(TAG, "next boot: %s", s_part->label);
    }
    return ESP_OK;
}

void ota_stream_abort(void)
{
    if (s_streaming) {
        s_streaming = false;
        esp_ota_abort(s_handle);
    }
}

// ---------------------------------------------------------------- HTTPS

void ota_set_progress_cb(ota_progress_cb_t cb)
{
    s_cb = cb;
}

static void report(int pct, size_t bytes, size_t total, bool done, esp_err_t err)
{
    if (s_cb) {
        s_cb(pct, bytes, total, done, err);
    }
}

static void ota_task(void *arg)
{
    char *url = arg;
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = "RoundAssistant/1.0 (ESP32-S3; ota)",
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
    };
    esp_https_ota_handle_t h = NULL;
    size_t done = 0, total = 0;
    int pct = 0;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err == ESP_OK) {
        int sz = esp_https_ota_get_image_size(h);
        total = sz > 0 ? (size_t)sz : 0;
        int last = -1;
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int rd = esp_https_ota_get_image_len_read(h);
            done = rd > 0 ? (size_t)rd : 0;
            pct = total ? (int)((uint64_t)done * 100 / total) : 0;
            if (pct / 5 != last / 5) {
                last = pct;
                report(pct, done, total, false, ESP_OK);
            }
        }
        if (err == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
            err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK) {
            err = esp_https_ota_finish(h);      // validates the image and selects it for boot
        } else {
            esp_https_ota_abort(h);
        }
        if (err == ESP_OK) {
            pct = 100;
            done = total;
        }
    }
    ESP_LOGI(TAG, "https update: %s", esp_err_to_name(err));
    report(pct, done, total, true, err);
    free(url);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    s_https_busy = false;
    vTaskDelete(NULL);
}

esp_err_t ota_start_https(const char *url)
{
    ESP_RETURN_ON_FALSE(url && !strncmp(url, "https://", 8), ESP_ERR_INVALID_ARG, TAG, "https:// URL required");
    ESP_RETURN_ON_FALSE(!s_streaming && !s_https_busy, ESP_ERR_INVALID_STATE, TAG, "update already running");
    ESP_RETURN_ON_ERROR(anchor_running_slot(), TAG, "otadata");
    size_t n = strlen(url) + 1;
    char *copy = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(copy, ESP_ERR_NO_MEM, TAG, "url");
    memcpy(copy, url, n);
    s_https_busy = true;
    if (xTaskCreatePinnedToCore(ota_task, "ota", OTA_TASK_STACK, copy, OTA_TASK_PRIO, NULL, 0) != pdPASS) {
        s_https_busy = false;
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- slots

esp_err_t ota_set_boot(const char *label)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    ESP_RETURN_ON_FALSE(p && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX,
                        ESP_ERR_NOT_FOUND, TAG, "no OTA slot %s", label);
    ESP_RETURN_ON_FALSE(!s_streaming && !s_https_busy, ESP_ERR_INVALID_STATE, TAG, "update running");
    // the slot switched to boots PENDING_VERIFY: give the running one an entry to fall back to
    if (p != esp_ota_get_running_partition()) {
        ESP_RETURN_ON_ERROR(anchor_running_slot(), TAG, "otadata");
    }
    return esp_ota_set_boot_partition(p);   // refuses a slot without a valid image
}

// esp_ota_mark_app_* change the otadata entry selected for the next boot, which is not the
// running image after "ota push ... noreboot" or "ota boot": refuse instead of marking the
// other slot (a pushed image marked VALID before it ever ran would skip its self-test).
static esp_err_t check_running_is_selected(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    ESP_RETURN_ON_FALSE(run && boot == run, ESP_ERR_INVALID_STATE, TAG, "%s is selected for the next boot, not %s",
                        boot ? boot->label : "?", run ? run->label : "?");
    return ESP_OK;
}

esp_err_t ota_rollback_now(void)
{
    ESP_RETURN_ON_ERROR(check_running_is_selected(), TAG, "rollback");
    ESP_RETURN_ON_FALSE(esp_ota_check_rollback_is_possible(), ESP_ERR_NOT_FOUND, TAG, "no valid previous image");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

esp_err_t ota_mark(bool valid)
{
    ESP_RETURN_ON_ERROR(check_running_is_selected(), TAG, "mark");
    if (valid) {
        // a USB-flashed image has no otadata entry yet: record it (as VALID)
        ESP_RETURN_ON_ERROR(anchor_running_slot(), TAG, "otadata");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return esp_ota_mark_app_invalid_rollback();
}
