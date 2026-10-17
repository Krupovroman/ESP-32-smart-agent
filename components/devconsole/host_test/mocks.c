// Host stand-ins: libc gaps, FreeRTOS, esp_console, logging, the USB-Serial-JTAG driver and OTA.
#include "mocks.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota.h"

int g_checks, g_fails;
int host_fail(const char *file, int line, const char *what)
{
    g_fails++;
    printf("  FAIL %s:%d: %s\n", file, line, what);
    return 0;
}

// ---- libc ----
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t d = strlen(dst);
    return d + strlcpy(dst + d, src, size > d ? size - d : 0);
}

static FILE s_dummy_file;
FILE *g_host_stdout = &s_dummy_file, *g_host_stderr = &s_dummy_file;
int host_fflush(FILE *f)
{
    (void)f;
    return 0;
}

// ---- esp_err, esp_system, app description, heap, CRC, clock ----
const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    default: return "UNKNOWN ERROR";
    }
}

esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
void esp_restart(void) { abort(); }

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = { "host-1", "round_assistant", "12:00:00", "Oct 18 2026", "v6.1" };
    return &d;
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 100000;
}

uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

int64_t g_now_us = 1000000;
int64_t esp_timer_get_time(void) { return g_now_us; }

// ---- logging ----
static vprintf_like_t s_log_hook;
vprintf_like_t esp_log_set_vprintf(vprintf_like_t f)
{
    vprintf_like_t old = s_log_hook;
    s_log_hook = f;
    return old;
}

static void call_hook(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    s_log_hook(fmt, ap);
    va_end(ap);
}

void host_log(char level, const char *tag, const char *fmt, ...)
{
    char msg[3000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (s_log_hook) {
        call_hook("%c (%lld) %s: %s\n", level, (long long)(g_now_us / 1000), tag, msg);
    } else {
        printf("%c %s: %s\n", level, tag, msg);
    }
}

// ---- FreeRTOS ----
int g_lock_depth;
void *g_cur_task;
static int s_task_dummy;
void *g_created_task = &s_task_dummy;

bool xPortInIsrContext(void) { return false; }
BaseType_t xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    static int m;
    return &m;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t)
{
    (void)s;
    (void)t;
    g_lock_depth++;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
    (void)s;
    g_lock_depth--;
    return pdTRUE;
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *h, BaseType_t core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)core;
    *h = g_created_task;       // the test drives the task loop itself
    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return g_cur_task; }
eTaskState eTaskGetState(TaskHandle_t t) { (void)t; return eBlocked; }
void vTaskDelay(TickType_t t) { g_now_us += (int64_t)t * 1000; }

// ---- esp_console ----
#define MAX_CMDS 80
static esp_console_cmd_t s_cmds[MAX_CMDS];
static int s_n_cmds;

esp_err_t esp_console_init(const esp_console_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t esp_console_register_help_command(void) { return ESP_OK; }

esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd)
{
    if (s_n_cmds >= MAX_CMDS) {
        return ESP_ERR_NO_MEM;
    }
    s_cmds[s_n_cmds++] = *cmd;
    return ESP_OK;
}

esp_err_t esp_console_run(const char *cmdline, int *cmd_ret)
{
    static char buf[1200];
    char *argv[16];
    int argc = 0;
    strlcpy(buf, cmdline, sizeof(buf));
    for (char *p = strtok(buf, " "); p && argc < 16; p = strtok(NULL, " ")) {
        argv[argc++] = p;
    }
    if (argc == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_n_cmds; i++) {
        if (!strcmp(s_cmds[i].command, argv[0])) {
            *cmd_ret = s_cmds[i].func_w_context ? s_cmds[i].func_w_context(s_cmds[i].context, argc, argv)
                                                : s_cmds[i].func(argc, argv);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

// ---- USB-Serial-JTAG ----
bool g_usb_connected = true;
size_t g_tx_room = SIZE_MAX;
static char *s_tx;
static size_t s_tx_len, s_tx_cap;
static char *s_rx;
static size_t s_rx_len, s_rx_pos;

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg) { (void)cfg; return ESP_OK; }
void usb_serial_jtag_vfs_use_driver(void) {}
bool usb_serial_jtag_is_connected(void) { return g_usb_connected; }
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks) { (void)ticks; return ESP_OK; }

int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t ticks)
{
    if (size > g_tx_room) {           // the ring takes a write whole or not at all
        g_now_us += (int64_t)ticks * 1000;
        return 0;
    }
    if (g_tx_room != SIZE_MAX) {
        g_tx_room -= size;
    }
    if (s_tx_len + size + 1 > s_tx_cap) {
        s_tx_cap = (s_tx_len + size + 1) * 2;
        s_tx = realloc(s_tx, s_tx_cap);
    }
    memcpy(s_tx + s_tx_len, src, size);
    s_tx_len += size;
    return (int)size;
}

int usb_serial_jtag_read_bytes(void *buf, uint32_t length, TickType_t ticks)
{
    size_t left = s_rx_len - s_rx_pos;
    if (left == 0) {
        g_now_us += (int64_t)ticks * 1000;   // nothing arrives within the timeout
        return 0;
    }
    size_t n = left < length ? left : length;
    memcpy(buf, s_rx + s_rx_pos, n);
    s_rx_pos += n;
    return (int)n;
}

void rx_push(const char *s)
{
    size_t n = strlen(s);
    char *nb = malloc(s_rx_len - s_rx_pos + n + 1);
    memcpy(nb, s_rx + s_rx_pos, s_rx_len - s_rx_pos);
    memcpy(nb + s_rx_len - s_rx_pos, s, n);
    s_rx_len = s_rx_len - s_rx_pos + n;
    s_rx_pos = 0;
    free(s_rx);
    s_rx = nb;
}

size_t rx_pending(void)
{
    return s_rx_len - s_rx_pos;
}

char *tx_take(void)
{
    char *out = malloc(s_tx_len + 1);
    memcpy(out, s_tx ? s_tx : "", s_tx_len);
    out[s_tx_len] = 0;
    s_tx_len = 0;
    return out;
}

// ---- OTA ----
void ota_get_info(ota_info_t *o)
{
    memset(o, 0, sizeof(*o));
    strlcpy(o->running, "ota_0", sizeof(o->running));
    o->boot_cnt = 7;
    o->selftest = OTA_ST_OK;
}

const char *ota_state_name(esp_ota_img_states_t s) { (void)s; return "valid"; }
const char *ota_selftest_name(ota_selftest_t s) { return s == OTA_ST_OK ? "ok" : "not_run"; }
