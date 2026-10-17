// System commands: ping, crc, info, heap, tasks, log, reboot.
#include "dc_internal.h"
#include "board.h"
#include "display.h"
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/rtc_cntl_reg.h"

static int cmd_ping(int argc, char **argv)
{
    // booted: READY was announced (also in safe mode); ready: all commands are available
    printf("{\"uptime_ms\":%lld,\"booted\":%s,\"ready\":%s,\"proto\":2}\n", (long long)(esp_timer_get_time() / 1000),
           dc_boot_ms() ? "true" : "false", devconsole_is_ready() ? "true" : "false");
    return 0;
}

static int cmd_crc(int argc, char **argv)
{
    if (argc < 2) {
        return dc_fail("usage: crc <text>");
    }
    // the words are CRC'd as one text joined by single spaces
    uint32_t crc = 0;
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            crc = esp_rom_crc32_le(crc, (const uint8_t *)" ", 1);
            len++;
        }
        size_t n = strlen(argv[i]);
        crc = esp_rom_crc32_le(crc, (const uint8_t *)argv[i], n);
        len += n;
    }
    printf("{\"crc32\":\"%08lx\",\"len\":%u}\n", (unsigned long)crc, (unsigned)len);
    return 0;
}

static void print_heap_members(void)
{
    printf("\"int\":{\"free\":%u,\"min\":%u,\"largest\":%u},",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("\"dma\":{\"free\":%u,\"largest\":%u},", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    printf("\"psram\":{\"free\":%u,\"min\":%u,\"largest\":%u}", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    printf(",\"lvgl\":{\"total\":%u,\"free\":%u,\"biggest\":%u,\"frag_pct\":%u}", (unsigned)m.total_size,
           (unsigned)m.free_size, (unsigned)m.free_biggest_size, (unsigned)m.frag_pct);
#endif
}

static int cmd_heap(int argc, char **argv)
{
    printf("{");
    print_heap_members();
    printf("}\n");
    return 0;
}

static bool exio_ok(void)
{
    uint32_t lvl = 0;
    esp_io_expander_handle_t x = board_expander();
    return x && esp_io_expander_get_level(x, 0xff, &lvl) == ESP_OK;
}

static int cmd_info(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const display_info_t *d = display_info();
    ota_info_t o;
    ota_get_info(&o);
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    printf("{\"ver\":\"%s\",\"project\":\"%s\",\"built\":\"%s %s\",\"idf\":\"%s\",", app->version,
           app->project_name, app->date, app->time, esp_get_idf_version());
    printf("\"part\":\"%s\",\"ota_state\":\"%s\",\"boot_cnt\":%u,\"reset\":\"%s\",\"uptime_ms\":%lld,\"boot_ms\":%u,",
           o.running, ota_state_name(o.state), (unsigned)o.boot_cnt, dc_reset_reason_name(esp_reset_reason()),
           (long long)(esp_timer_get_time() / 1000), (unsigned)dc_boot_ms());
    printf("\"panel_id\":\"%02x%02x%02x%02x\",\"panel_batch\":%d,\"pclk_mhz\":%u,", d->panel_id[0], d->panel_id[1],
           d->panel_id[2], d->panel_id[3], d->panel_batch, (unsigned)(d->pclk_hz / 1000000));
    printf("\"exio_ok\":%s,\"psram_kb\":%u,\"flash_mb\":%u,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",",
           exio_ok() ? "true" : "false", (unsigned)(esp_psram_get_size() / 1024), (unsigned)(flash >> 20), mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
    // GPIO7 is output-only: read the output latch, not the pad
    printf("\"gpio7\":%d,\"bl\":%u,\"selftest\":\"%s\",\"selftest_reason\":\"%s\",\"ready\":%s,",
           (int)((REG_READ(GPIO_OUT_REG) >> 7) & 1), board_backlight_get(), ota_selftest_name(o.selftest),
           o.selftest_reason, devconsole_is_ready() ? "true" : "false");
    print_heap_members();
    dc_info_print_notes();
    printf("}\n");
    return 0;
}

#if configUSE_TRACE_FACILITY
static const char *task_state(eTaskState s)
{
    switch (s) {
    case eRunning: return "running";
    case eReady: return "ready";
    case eBlocked: return "blocked";
    case eSuspended: return "suspended";
    case eDeleted: return "deleted";
    default: return "?";
    }
}

static int cmd_tasks(int argc, char **argv)
{
    uint32_t ms = argc > 1 ? (uint32_t)atoi(argv[1]) : 250;
    if (ms > 5000) {
        ms = 5000;
    }
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *a = heap_caps_calloc(cap, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    TaskStatus_t *b = heap_caps_calloc(cap, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (!a || !b) {
        free(a);
        free(b);
        return dc_fail("no memory");
    }
    configRUN_TIME_COUNTER_TYPE ta = 0, tb = 0;
    UBaseType_t na = uxTaskGetSystemState(a, cap, &ta);
    vTaskDelay(pdMS_TO_TICKS(ms));
    UBaseType_t nb = uxTaskGetSystemState(b, cap, &tb);
    printf("{\"window_ms\":%u,\"tasks\":[", (unsigned)ms);
    for (UBaseType_t i = 0; i < nb; i++) {
        const TaskStatus_t *t = &b[i];
        int core = -1;
#if configTASKLIST_INCLUDE_COREID
        core = t->xCoreID == tskNO_AFFINITY ? -1 : (int)t->xCoreID;
#endif
        printf("%s{\"name\":\"%s\",\"core\":%d,\"prio\":%u,\"state\":\"%s\",\"hwm_bytes\":%u,\"stack_caps\":\"%s\"",
               i ? "," : "", t->pcTaskName, core, (unsigned)t->uxCurrentPriority, task_state(t->eCurrentState),
               (unsigned)(t->usStackHighWaterMark * sizeof(StackType_t)),
               esp_ptr_external_ram(t->pxStackBase) ? "psram" : "internal");
#if configGENERATE_RUN_TIME_STATS
        // percent of one core over the window
        for (UBaseType_t k = 0; k < na; k++) {
            if (a[k].xHandle == t->xHandle && tb != ta) {   // unsigned deltas survive a wrap
                uint64_t d = (uint64_t)(t->ulRunTimeCounter - a[k].ulRunTimeCounter);
                printf(",\"cpu_pct\":%u", (unsigned)(d * 100 / (uint64_t)(tb - ta)));
                break;
            }
        }
#endif
        printf("}");
    }
    printf("]}\n");
    free(a);
    free(b);
    return 0;
}
#else
static int cmd_tasks(int argc, char **argv)
{
    return dc_fail("needs CONFIG_FREERTOS_USE_TRACE_FACILITY=y");
}
#endif

static int cmd_log(int argc, char **argv)
{
    static const char *names[] = { "none", "error", "warn", "info", "debug", "verbose" };
    if (argc < 3) {
        return dc_fail("usage: log <tag|*> <none|error|warn|info|debug|verbose>");
    }
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if (!strcmp(argv[2], names[i])) {
            esp_log_level_set(argv[1], (esp_log_level_t)i);
            // levels above CONFIG_LOG_MAXIMUM_LEVEL are compiled out: "max" tells why
            // "log wifi debug" prints nothing in this build
            printf("{\"tag\":\"%s\",\"level\":\"%s\",\"max\":\"%s\"}\n", argv[1], names[i],
                   names[CONFIG_LOG_MAXIMUM_LEVEL]);
            return 0;
        }
    }
    return dc_fail("unknown level %s", argv[2]);
}

static void do_restart(void *arg)
{
    if (arg) {
        // the ROM checks this bit after a software reset and enters USB download mode
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    }
    esp_restart();
}

// reboot [dl] [after=<s>]: after= restarts later from a timer, e.g. to unplug USB first and check
// that the battery latch (GPIO7) holds the board on through a software reset.
static int cmd_reboot(int argc, char **argv)
{
    static esp_timer_handle_t s_later;
    bool dl = false;
    int after = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "dl")) {
            dl = true;
        } else if (!strncmp(argv[i], "after=", 6)) {
            after = atoi(argv[i] + 6);
        } else {
            return dc_fail("usage: reboot [dl] [after=<s>]");
        }
    }
    if (after < 0 || after > 600) {
        return dc_fail("after=0..600 s");
    }
    void *arg = dl ? (void *)1 : NULL;
    if (after == 0) {
        printf("{\"reboot\":\"%s\"}\n", dl ? "download" : "app");
        dc_defer(do_restart, arg);
        return 0;
    }
    if (s_later) {   // a newer request replaces a pending one
        esp_timer_stop(s_later);
        esp_timer_delete(s_later);
        s_later = NULL;
    }
    const esp_timer_create_args_t ta = { .callback = do_restart, .arg = arg, .name = "reboot" };
    esp_err_t e = esp_timer_create(&ta, &s_later);
    if (e == ESP_OK) {
        e = esp_timer_start_once(s_later, (uint64_t)after * 1000000);
    }
    if (e != ESP_OK) {
        dc_fail("timer: %s", esp_err_to_name(e));
        return e;
    }
    printf("{\"reboot\":\"%s\",\"after_s\":%d}\n", dl ? "download" : "app", after);
    return 0;
}

void cmd_sys_register(void)
{
    dc_register_early("ping", "ping - uptime and readiness", cmd_ping);
    dc_register_early("crc", "crc <text> - CRC-32 of the text (crc 123456789 = cbf43926)", cmd_crc);
    dc_register_early("info", "Firmware, slot, panel, memory and boot info (JSON)", cmd_info);
    dc_register_early("heap", "Heap statistics (JSON)", cmd_heap);
    dc_register_early("tasks", "tasks [ms] - task list with stack high-water marks and CPU use", cmd_tasks);
    dc_register_early("log", "log <tag|*> <none|error|warn|info|debug|verbose>", cmd_log);
    dc_register_early("reboot", "reboot [dl] [after=<s>] - restart (dl: into the ROM download mode)", cmd_reboot);
}
