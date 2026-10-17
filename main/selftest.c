// Boot self-test before an OTA image is confirmed (docs/architecture.md §8.1, §8.2), and the
// RTC crash counter that selects safe mode after repeated crashes during boot.
#include "selftest.h"
#include "board.h"
#include "devconsole.h"
#include "display.h"
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "selftest";

#define CRASH_MAGIC       0x52414331u   // "RAC1": RTC words are valid
#define SAFE_MODE_CRASHES 3
#define GUARD_MS          10000
#define LVGL_WINDOW_MS    500
// heap gates at this point of the boot (§8.2; tighten after measuring)
#define GATE_INT_FREE     (40 * 1024)
#define GATE_INT_LARGEST  (16 * 1024)
#define GATE_DMA_FREE     (12 * 1024)
#define GATE_PSRAM_FREE   (3 * 1024 * 1024)

static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_crashes;
static RTC_NOINIT_ATTR uint32_t s_rtc_reached_ready;

static bool s_safe;
static volatile uint32_t s_ticks, s_frames;

bool selftest_boot_begin(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    if (s_rtc_magic != CRASH_MAGIC || r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT) {
        s_rtc_magic = CRASH_MAGIC;
        s_rtc_crashes = 0;
        s_rtc_reached_ready = 1;
    }
    bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (crash && !s_rtc_reached_ready) {
        s_rtc_crashes++;
    }
    s_rtc_reached_ready = 0;
    // a pending OTA image that crashes is rolled back by the bootloader instead
    s_safe = s_rtc_crashes >= SAFE_MODE_CRASHES && !ota_pending_verify();
    char v[12];
    snprintf(v, sizeof(v), "%u", (unsigned)s_rtc_crashes);
    devconsole_info_set("boot_crashes", v);
    devconsole_info_set("safe_mode", s_safe ? "true" : "false");
    if (s_safe) {
        ESP_LOGE(TAG, "%u crashes during boot: SAFE MODE (no Wi-Fi, no audio)", (unsigned)s_rtc_crashes);
    }
    return s_safe;
}

static void guard_cb(void *arg)
{
    ESP_LOGE(TAG, "self-test hung for %d ms: restarting (rollback)", GUARD_MS);
    esp_restart();
}

static void tick_cb(lv_timer_t *t)
{
    s_ticks++;
}

static void refr_cb(lv_event_t *e)
{
    s_frames++;
}

// LVGL is alive when a full refresh completes and a 20 ms timer keeps firing.
static bool lvgl_alive(void)
{
    lv_display_t *d = display_get();
    if (!d || !lvgl_port_lock(1000)) {
        return false;
    }
    s_ticks = 0;
    s_frames = 0;
    lv_timer_t *t = lv_timer_create(tick_cb, 20, NULL);
    lv_display_add_event_cb(d, refr_cb, LV_EVENT_REFR_READY, NULL);
    lv_obj_invalidate(lv_screen_active());
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(LVGL_WINDOW_MS));
    bool ok = s_frames >= 1 && s_ticks >= 10;
    if (lvgl_port_lock(1000)) {
        lv_timer_delete(t);
        lv_display_remove_event_cb_with_user_data(d, refr_cb, NULL);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "lvgl: %u frames, %u ticks in %d ms", (unsigned)s_frames, (unsigned)s_ticks, LVGL_WINDOW_MS);
    return ok;
}

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
// The LVGL heap must be the full pool, placed in PSRAM.
static bool lvgl_pool_ok(void)
{
    if (!lvgl_port_lock(1000)) {
        return false;
    }
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    void *p = lv_malloc(64);
    bool ext = p && esp_ptr_external_ram(p);
    lv_free(p);
    lvgl_port_unlock();
    return m.total_size + 4096 >= LV_MEM_SIZE && ext;
}
#endif

static void add(char *list, size_t len, const char *what)
{
    if (list[0]) {
        strlcat(list, ",", len);
    }
    strlcat(list, what, len);
}

esp_err_t selftest_run(const selftest_inputs_t *in)
{
    int64_t t0 = esp_timer_get_time();
    bool pending = ota_pending_verify();
    esp_timer_handle_t guard = NULL;
    if (pending) {
        const esp_timer_create_args_t a = { .callback = guard_cb, .name = "st_guard" };
        if (esp_timer_create(&a, &guard) == ESP_OK) {
            esp_timer_start_once(guard, (uint64_t)GUARD_MS * 1000);
        }
    }
    char fail[64] = "", warn[64] = "";
#define CHECK(cond, what)                    \
    do {                                     \
        if (!(cond)) {                       \
            add(fail, sizeof(fail), (what)); \
        }                                    \
    } while (0)
#define WARN(cond, what)                     \
    do {                                     \
        if (!(cond)) {                       \
            add(warn, sizeof(warn), (what)); \
        }                                    \
    } while (0)

    uint32_t boots = 0;
    CHECK(ota_boot_count_bump(&boots) == ESP_OK, "nvs");
    uint32_t lvl = 0;
    CHECK(in->board == ESP_OK && board_expander() &&
          esp_io_expander_get_level(board_expander(), 0xff, &lvl) == ESP_OK, "tca9554");
    CHECK(in->display == ESP_OK, "display");
    WARN(display_info()->panel_batch != 0, "panel_id");
    CHECK(in->display == ESP_OK && lvgl_alive(), "lvgl");
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    CHECK(lvgl_pool_ok(), "lvgl_pool");
#endif
    CHECK(in->audio_in == ESP_OK && in->audio_out == ESP_OK, "i2s");
    WARN(in->touch == ESP_OK, "touch");
    CHECK(in->wifi == ESP_OK, "wifi_init");
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    CHECK(int_free >= GATE_INT_FREE && int_big >= GATE_INT_LARGEST, "int_heap");
    CHECK(dma_free >= GATE_DMA_FREE, "dma_heap");
    CHECK(ps_free >= GATE_PSRAM_FREE, "psram");
    CHECK(devconsole_running(), "devconsole");
#if CONFIG_RA_SELFTEST_FORCE_PANIC
    ESP_LOGE(TAG, "CONFIG_RA_SELFTEST_FORCE_PANIC: crashing before the image is confirmed");
    vTaskDelay(pdMS_TO_TICKS(200));
    abort();
#endif
#if CONFIG_RA_SELFTEST_FORCE_FAIL
    CHECK(false, "forced");
#endif
#undef CHECK
#undef WARN

    if (guard) {
        esp_timer_stop(guard);
        esp_timer_delete(guard);
    }
    bool ok = fail[0] == 0;
    unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "%s in %u ms (pending=%d) fail=[%s] warn=[%s] int=%u/%u dma=%u psram=%u", ok ? "PASS" : "FAIL",
             ms, pending, fail, warn, (unsigned)int_free, (unsigned)int_big, (unsigned)dma_free, (unsigned)ps_free);
    devcon_event("selftest", "\"ok\":%s,\"ms\":%u,\"pending\":%s,\"fail\":\"%s\",\"warn\":\"%s\",\"boot\":%u",
                 ok ? "true" : "false", ms, pending ? "true" : "false", fail, warn, (unsigned)boots);
    esp_err_t e = ota_confirm_boot(ok, ok ? warn : fail);   // a failed pending image reboots here
    return ok ? e : ESP_FAIL;
}

void selftest_ready(void)
{
    devconsole_ready(!s_safe);
    s_rtc_reached_ready = 1;
    s_rtc_crashes = 0;
}
