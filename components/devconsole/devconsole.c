// Dev console transport, protocol v2 (docs/architecture.md §7): line reader on the
// USB-Serial-JTAG driver (no linenoise, no echo), "#id" framing, OK/ERR result lines,
// one writer for printf, ESP_LOG and events, and drop-instead-of-block when no host reads.
#include "dc_internal.h"
#include "ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "devcon";

#define TASK_STACK      6144     // internal RAM: commands write NVS and flash (OTA)
#define TASK_PRIO       3
#define MAX_CMDS        64
#define CAP_BYTES       (24 * 1024)
#define OUT_LINE_BYTES  2048
#define USB_CHUNK       512
#define WAIT_CMD_MS     1000     // command output: the host is reading, allow it to lag
#define WAIT_LOG_MS     100
#define LOCK_WAIT_MS    2000

// ---------------------------------------------------------------- state

static TaskHandle_t s_task;
static SemaphoreHandle_t s_wr;               // recursive: a log from inside the writer must not deadlock
static volatile bool s_stalled;              // host stopped reading; drop logs until the TX ring drains
static bool s_cmd_stalled;                   // the running command's output already timed out once
static volatile uint32_t s_dropped;
static bool s_drv;
static volatile bool s_ready;
static uint32_t s_boot_ms;

// the line being written (only touched while s_wr is held)
static char s_stage[USB_CHUNK];
static size_t s_stage_len;
static bool s_line_important;
static char s_fmt[640];
static int s_wr_depth;                       // >0 while this task is inside a write

// command being executed (devcon task only)
static uint16_t s_cur_id;
static bool s_capture;                       // stdout of the devcon task goes to s_cap
static char *s_cap;                          // PSRAM, '\n'-separated lines
static size_t s_cap_len;
static bool s_cap_trunc;
static char s_fail_msg[200];
static void (*s_defer_fn)(void *);
static void *s_defer_arg;

// stdout lines of other tasks (and of id-0 commands)
static char *s_ob;
static size_t s_ob_len;

// input
static uint8_t s_rx[256];
static size_t s_rx_len, s_rx_pos;
static char *s_line;                         // PSRAM, DC_LINE_MAX + 1
static size_t s_line_len;
static bool s_line_over;

typedef struct {
    dc_cmd_fn fn;
    bool early;
} dc_entry_t;
static dc_entry_t s_entries[MAX_CMDS];
static int s_n_entries;

typedef struct {
    const char *name, *help;
    dc_entry_t *e;
} pending_t;
static pending_t s_pending[MAX_CMDS];
static int s_n_pending;
static bool s_started;

// ---------------------------------------------------------------- USB output

static bool in_isr_or_no_sched(void)
{
    return xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING;
}

// Pushes bytes into the driver's TX ring. Never waits forever: without a host the bytes are
// dropped at once, and after one timeout logs are dropped until the host reads again. Command
// output (a host just sent the command) still gets one bounded wait per command, because the
// ring may hold stale logs from before the port was opened.
static void usb_out(const char *p, size_t n)
{
    if (!s_drv || n == 0) {
        return;
    }
    if (!usb_serial_jtag_is_connected()) {
        s_dropped++;
        return;
    }
    const bool can_wait = !in_isr_or_no_sched() && (s_line_important ? !s_cmd_stalled : !s_stalled);
    while (n > 0) {
        size_t k = n > USB_CHUNK ? USB_CHUNK : n;
        int w = usb_serial_jtag_write_bytes(p, k, 0);
        if (w <= 0) {
            if (!can_wait) {
                s_dropped++;
                return;
            }
            w = usb_serial_jtag_write_bytes(p, k, pdMS_TO_TICKS(s_line_important ? WAIT_CMD_MS : WAIT_LOG_MS));
            if (w <= 0) {
                s_stalled = true;
                if (s_line_important) {
                    s_cmd_stalled = true;
                }
                s_dropped++;
                return;
            }
        }
        s_stalled = false;
        p += k;
        n -= k;
    }
}

static void stage_flush(void)
{
    usb_out(s_stage, s_stage_len);
    s_stage_len = 0;
}

static bool wr_lock(void)
{
    if (!s_wr || xPortInIsrContext()) {
        return false;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return true;                // single-threaded: nothing to lock against
    }
    if (xSemaphoreTakeRecursive(s_wr, pdMS_TO_TICKS(LOCK_WAIT_MS)) != pdTRUE) {
        s_dropped++;
        return false;
    }
    return true;
}

static void wr_unlock(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreGiveRecursive(s_wr);
    }
}

bool dc_line_begin(bool important)
{
    if (!wr_lock()) {
        return false;
    }
    if (s_wr_depth++ == 0) {
        s_stage_len = 0;
        s_line_important = important;
    }
    return true;
}

void dc_line_put(const char *s, size_t n)
{
    while (n > 0) {
        size_t room = sizeof(s_stage) - s_stage_len;
        size_t k = n < room ? n : room;
        memcpy(s_stage + s_stage_len, s, k);
        s_stage_len += k;
        s += k;
        n -= k;
        if (s_stage_len == sizeof(s_stage)) {
            stage_flush();
        }
    }
}

void dc_line_puts(const char *s)
{
    dc_line_put(s, strlen(s));
}

void dc_line_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_fmt, sizeof(s_fmt), fmt, ap);
    va_end(ap);
    if (n > 0) {
        dc_line_put(s_fmt, (size_t)n < sizeof(s_fmt) ? (size_t)n : sizeof(s_fmt) - 1);
    }
}

void dc_line_json_str(const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    dc_line_put("\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[2] = { '\\', (char)c };
            dc_line_put(e, 2);
        } else if (c < 0x20) {
            char e[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
            dc_line_put(e, 6);
        } else {
            dc_line_put((const char *)&s[i], 1);
        }
    }
    dc_line_put("\"", 1);
}

void dc_line_end(void)
{
    if (s_wr_depth <= 0) {
        return;
    }
    if (--s_wr_depth == 0) {
        dc_line_put("\n", 1);
        stage_flush();
    }
    wr_unlock();
}

// A line that did not come from the protocol layer. It must not look like a frame.
static void emit_raw(const char *s, size_t n, bool important)
{
    if (!dc_line_begin(important)) {
        return;
    }
    if (n > 0 && s[0] == '@') {
        dc_line_put(" ", 1);
    }
    dc_line_put(s, n);
    dc_line_end();
}

// ---------------------------------------------------------------- ESP_LOG and stdout hooks

static char s_logbuf[512];
static bool s_log_busy;

static int log_vprintf(const char *fmt, va_list ap)
{
    if (!wr_lock()) {
        return 0;
    }
    if (s_log_busy || s_wr_depth > 0) {   // raised by the writer itself (mid-line): drop it
        wr_unlock();
        return 0;
    }
    s_log_busy = true;
    int n = vsnprintf(s_logbuf, sizeof(s_logbuf), fmt, ap);
    size_t len = n < 0 ? 0 : ((size_t)n < sizeof(s_logbuf) ? (size_t)n : sizeof(s_logbuf) - 1);
    // one log call is one line; split embedded newlines so every line stays a log line
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s_logbuf[i] == '\n' || s_logbuf[i] == '\r') {
            if (i > start) {
                emit_raw(s_logbuf + start, i - start, false);
            }
            start = i + 1;
        }
    }
    s_log_busy = false;
    wr_unlock();
    return n;
}

static void cap_putc(char c)
{
    if (s_cap_len + 1 < CAP_BYTES) {
        s_cap[s_cap_len++] = c;
    } else {
        s_cap_trunc = true;
    }
}

static int out_putc(char c, FILE *f)
{
    (void)f;
    if (c == '\r') {
        return 0;
    }
    if (s_capture && s_cur_id != 0 && xTaskGetCurrentTaskHandle() == s_task) {
        cap_putc(c);
        return 0;
    }
    if (c == '\n' || s_ob_len >= OUT_LINE_BYTES) {
        bool mine = xTaskGetCurrentTaskHandle() == s_task;
        emit_raw(s_ob, s_ob_len, mine);
        s_ob_len = 0;
        if (c == '\n') {
            return 0;
        }
    }
    s_ob[s_ob_len++] = c;
    return 0;
}

static int out_flush(FILE *f)
{
    (void)f;
    return 0;
}

static FILE s_out = FDEV_SETUP_STREAM(out_putc, NULL, out_flush, _FDEV_SETUP_WRITE);

// The console component prints through a per-task stdout (help output).
#if CONFIG_LIBC_PICOLIBC && !CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
#define tls_stdout linenoise_stdout
#endif
#if CONFIG_LIBC_PICOLIBC
extern __thread FILE *tls_stdout;
#endif

// ---------------------------------------------------------------- commands

int dc_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (xTaskGetCurrentTaskHandle() == s_task) {
        vsnprintf(s_fail_msg, sizeof(s_fail_msg), fmt, ap);
        if (s_cur_id == 0) {
            printf("error: %s\n", s_fail_msg);
        }
    } else {
        char msg[120];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        ESP_LOGW(TAG, "dc_fail outside a command: %s", msg);
    }
    va_end(ap);
    return 1;
}

const char *dc_arg(int argc, char **argv, const char *key)
{
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], key, kl) && argv[i][kl] == '=') {
            return argv[i] + kl + 1;
        }
    }
    return NULL;
}

void dc_defer(void (*fn)(void *arg), void *arg)
{
    s_defer_fn = fn;
    s_defer_arg = arg;
}

uint16_t dc_cur_id(void)
{
    return s_cur_id;
}

static int trampoline(void *ctx, int argc, char **argv)
{
    const dc_entry_t *e = ctx;
    if (!e->early && !s_ready) {
        dc_fail("not ready");
        return ESP_ERR_INVALID_STATE;
    }
    return e->fn(argc, argv);
}

static void register_now(const char *name, const char *help, dc_entry_t *e)
{
    const esp_console_cmd_t cmd = {
        .command = name, .help = help, .func_w_context = trampoline, .context = e,
    };
    if (esp_console_cmd_register(&cmd) != ESP_OK) {
        ESP_LOGE(TAG, "cannot register %s", name);
    }
}

static void register_cmd(const char *name, const char *help, dc_cmd_fn fn, bool early)
{
    if (s_n_entries >= MAX_CMDS) {
        ESP_LOGE(TAG, "too many commands, %s dropped", name);
        return;
    }
    dc_entry_t *e = &s_entries[s_n_entries++];
    e->fn = fn;
    e->early = early;
    if (s_started) {
        register_now(name, help, e);
    } else {
        s_pending[s_n_pending++] = (pending_t){ name, help, e };
    }
}

void devconsole_register(const char *name, const char *help, dc_cmd_fn fn)
{
    register_cmd(name, help, fn, false);
}

void dc_register_early(const char *name, const char *help, dc_cmd_fn fn)
{
    register_cmd(name, help, fn, true);
}

static bool is_json_object(const char *s, size_t n)
{
    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }
    return n >= 2 && s[0] == '{' && s[n - 1] == '}';
}

// Writes ,"out":[...] or "out":[...] with every captured line except skip.
static void put_out_array(const char *skip)
{
    dc_line_puts("\"out\":[");
    bool first = true;
    size_t i = 0;
    while (i < s_cap_len) {
        const char *l = s_cap + i;
        const char *nl = memchr(l, '\n', s_cap_len - i);
        size_t n = nl ? (size_t)(nl - l) : s_cap_len - i;
        if (l != skip) {
            if (!first) {
                dc_line_put(",", 1);
            }
            dc_line_json_str(l, n);
            first = false;
        }
        i += n + 1;
    }
    dc_line_puts("]");
    if (s_cap_trunc) {
        dc_line_puts(",\"truncated\":true");
    }
}

static bool has_lines_except(const char *skip)
{
    size_t i = 0;
    while (i < s_cap_len) {
        const char *l = s_cap + i;
        const char *nl = memchr(l, '\n', s_cap_len - i);
        size_t n = nl ? (size_t)(nl - l) : s_cap_len - i;
        if (l != skip) {
            return true;
        }
        i += n + 1;
    }
    return false;
}

static const char *err_name(int rc)
{
    if (rc == 1 || rc == -1) {
        return "ESP_FAIL";
    }
    const char *n = esp_err_to_name((esp_err_t)rc);
    return (!n || !strncmp(n, "UNKNOWN", 7)) ? "ESP_FAIL" : n;
}

static void send_ok(uint16_t id)
{
    if (!dc_line_begin(true)) {
        return;
    }
    dc_line_printf("@%u OK ", id);
    if (id == 0) {
        dc_line_puts("{}");
        dc_line_end();
        return;
    }
    // the last captured line that is a JSON object becomes the payload
    const char *obj = NULL;
    size_t obj_n = 0;
    size_t i = 0;
    while (i < s_cap_len) {
        const char *l = s_cap + i;
        const char *nl = memchr(l, '\n', s_cap_len - i);
        size_t n = nl ? (size_t)(nl - l) : s_cap_len - i;
        if (is_json_object(l, n)) {
            obj = l;
            obj_n = n;
        }
        i += n + 1;
    }
    if (!obj) {
        dc_line_puts("{");
        if (s_cap_len > 0) {
            put_out_array(NULL);
        }
        dc_line_puts("}");
    } else if (!has_lines_except(obj)) {
        dc_line_put(obj, obj_n);
    } else {
        while (obj[obj_n - 1] != '}') {
            obj_n--;
        }
        size_t inner = obj_n - 1;    // drop the closing brace
        bool empty = true;
        for (size_t k = 1; k < inner; k++) {
            if (obj[k] != ' ') {
                empty = false;
            }
        }
        dc_line_put(obj, inner);
        if (!empty) {
            dc_line_put(",", 1);
        }
        put_out_array(obj);
        dc_line_puts("}");
    }
    dc_line_end();
}

static void send_err(uint16_t id, const char *name, const char *msg)
{
    if (!dc_line_begin(true)) {
        return;
    }
    dc_line_printf("@%u ERR %s {\"msg\":", id, name);
    dc_line_json_str(msg, strlen(msg));
    if (id != 0 && s_cap_len > 0) {
        dc_line_put(",", 1);
        put_out_array(NULL);
    }
    dc_line_puts("}");
    dc_line_end();
}

static void run_command(uint16_t id, const char *cmdline)
{
    s_cur_id = id;
    s_cmd_stalled = false;
    s_cap_len = 0;
    s_cap_trunc = false;
    s_fail_msg[0] = 0;
    s_defer_fn = NULL;
    s_capture = true;
    int ret = 0;
    esp_err_t e = esp_console_run(cmdline, &ret);
    fflush(stdout);
    s_capture = false;
    if (e == ESP_ERR_NOT_FOUND) {
        char msg[64];
        snprintf(msg, sizeof(msg), "unknown command: %.40s", cmdline);
        send_err(id, "ESP_ERR_NOT_FOUND", msg);
    } else if (e != ESP_OK) {
        send_err(id, esp_err_to_name(e), "bad command line");
    } else if (ret != 0) {
        send_err(id, err_name(ret), s_fail_msg[0] ? s_fail_msg : "failed");
    } else {
        send_ok(id);
    }
    s_cur_id = 0;
    void (*fn)(void *) = s_defer_fn;
    s_defer_fn = NULL;
    if (fn && e == ESP_OK && ret == 0) {
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(300));   // let the OK line out first
        fn(s_defer_arg);
    }
}

// Upload traffic that arrives after an aborted upload is not a command.
static bool is_stale_upload_line(const char *p)
{
    return (p[0] == 'D' && p[1] == ' ') || !strcmp(p, "END") || !strcmp(p, "ABORT");
}

static void handle_line(char *p, size_t len)
{
    uint16_t id = 0;
    while (*p == ' ' || *p == '\t') {
        p++;
        len--;
    }
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
        p[--len] = 0;
    }
    if (p[0] == '#') {
        char *end = NULL;
        unsigned long v = strtoul(p + 1, &end, 10);
        if (end == p + 1 || v == 0 || v > 65535 || (*end != ' ' && *end != 0)) {
            send_err(0, "ESP_ERR_INVALID_ARG", "bad #id");
            return;
        }
        id = (uint16_t)v;
        p = end;
        while (*p == ' ') {
            p++;
        }
        len = strlen(p);
    }
    if (len == 0) {
        return;
    }
    if (id == 0 && is_stale_upload_line(p)) {
        return;
    }
    if (len > DC_CMD_MAX) {
        send_err(id, "ESP_ERR_INVALID_SIZE", "command line too long");
        return;
    }
    run_command(id, p);
}

// ---------------------------------------------------------------- input

int dc_read_line(char **line, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        while (s_rx_pos < s_rx_len) {
            char c = (char)s_rx[s_rx_pos++];
            if (c == '\n' || c == '\r') {
                if (s_line_len == 0 && !s_line_over) {
                    continue;          // empty line, or the LF of a CR LF
                }
                bool over = s_line_over;
                size_t n = s_line_len;
                s_line[n] = 0;
                s_line_len = 0;
                s_line_over = false;
                *line = s_line;
                return over ? -2 : (int)n;
            }
            if (s_line_len < DC_LINE_MAX) {
                s_line[s_line_len++] = c;
            } else {
                s_line_over = true;
            }
        }
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            return -1;
        }
        uint32_t wait_ms = left_us / 1000 > 100 ? 100 : (uint32_t)(left_us / 1000) + 1;
        int n = usb_serial_jtag_read_bytes(s_rx, sizeof(s_rx), pdMS_TO_TICKS(wait_ms));
        s_rx_len = n > 0 ? (size_t)n : 0;
        s_rx_pos = 0;
    }
}

static void devcon_task(void *arg)
{
#if CONFIG_LIBC_PICOLIBC
    tls_stdout = &s_out;
#endif
    for (;;) {
        char *line;
        int n = dc_read_line(&line, 1000);
        if (n == -2) {
            send_err(0, "ESP_ERR_INVALID_SIZE", "line too long");
        } else if (n > 0) {
            handle_line(line, (size_t)n);
        }
    }
}

// ---------------------------------------------------------------- events, boot status

void devcon_event(const char *ev, const char *fmt, ...)
{
    if (!dc_line_begin(false)) {
        return;
    }
    dc_line_printf("@! EVT {\"t\":%lld,\"ev\":\"%s\"", (long long)(esp_timer_get_time() / 1000), ev);
    if (fmt && fmt[0]) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(s_fmt, sizeof(s_fmt), fmt, ap);
        va_end(ap);
        if (n > 0) {
            dc_line_put(",", 1);
            dc_line_put(s_fmt, (size_t)n < sizeof(s_fmt) ? (size_t)n : sizeof(s_fmt) - 1);
        }
    }
    dc_line_puts("}");
    dc_line_end();
}

typedef struct {
    char name[20];
    uint32_t free;
} stage_t;
static stage_t s_stages[8];
static int s_n_stages;

typedef struct {
    char key[20];
    char val[48];
} note_t;
static note_t s_notes[8];
static int s_n_notes;

void devconsole_heap_stage(const char *name)
{
    if (s_n_stages < (int)(sizeof(s_stages) / sizeof(s_stages[0]))) {
        strlcpy(s_stages[s_n_stages].name, name, sizeof(s_stages[0].name));
        s_stages[s_n_stages].free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        s_n_stages++;
    }
}

void devconsole_info_set(const char *key, const char *json_value)
{
    int i = 0;
    while (i < s_n_notes && strcmp(s_notes[i].key, key)) {
        i++;
    }
    if (i == s_n_notes) {
        if (s_n_notes >= (int)(sizeof(s_notes) / sizeof(s_notes[0]))) {
            return;
        }
        s_n_notes++;
    }
    strlcpy(s_notes[i].key, key, sizeof(s_notes[0].key));
    strlcpy(s_notes[i].val, json_value, sizeof(s_notes[0].val));
}

void dc_info_print_notes(void)
{
    printf(",\"heap_stages\":{");
    for (int i = 0; i < s_n_stages; i++) {
        printf("%s\"%s\":%u", i ? "," : "", s_stages[i].name, (unsigned)s_stages[i].free);
    }
    printf("}");
    for (int i = 0; i < s_n_notes; i++) {
        printf(",\"%s\":%s", s_notes[i].key, s_notes[i].val);
    }
}

uint32_t dc_boot_ms(void)
{
    return s_boot_ms;
}

const char *dc_reset_reason_name(int r)
{
    switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "other";
    }
}

void devconsole_ready(bool unlock)
{
    ota_info_t o;
    ota_get_info(&o);
    s_boot_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_ready = unlock;
    const esp_app_desc_t *app = esp_app_get_description();
    if (!dc_line_begin(false)) {
        return;
    }
    dc_line_printf("@! READY {\"ver\":\"%s\",\"reset\":\"%s\",\"boot\":%u,\"part\":\"%s\",\"state\":\"%s\","
                   "\"boot_ms\":%u,\"selftest\":\"%s\",\"safe_mode\":%s,\"proto\":2}",
                   app->version, dc_reset_reason_name(esp_reset_reason()), (unsigned)o.boot_cnt, o.running,
                   ota_state_name(o.state), (unsigned)s_boot_ms, ota_selftest_name(o.selftest),
                   unlock ? "false" : "true");
    dc_line_end();
}

bool devconsole_is_ready(void)
{
    return s_ready;
}

bool devconsole_running(void)
{
    return s_task != NULL && eTaskGetState(s_task) != eDeleted;
}

// ---------------------------------------------------------------- start

esp_err_t devconsole_start(void)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_line = heap_caps_malloc(DC_LINE_MAX + 1, MALLOC_CAP_SPIRAM);
    s_cap = heap_caps_malloc(CAP_BYTES, MALLOC_CAP_SPIRAM);
    s_ob = heap_caps_malloc(OUT_LINE_BYTES + 1, MALLOC_CAP_SPIRAM);
    s_wr = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_line && s_cap && s_ob && s_wr, ESP_ERR_NO_MEM, TAG, "buffers");

    usb_serial_jtag_driver_config_t usb = {
        .tx_buffer_size = DC_USB_TX_RING,
        .rx_buffer_size = DC_USB_RX_RING,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb), TAG, "usb driver");
    // stray writes to fd 1 (code that bypasses stdout) also go through the driver's ring
    usb_serial_jtag_vfs_use_driver();
    s_drv = true;

    fflush(stdout);
    stdout = &s_out;
    stderr = &s_out;
    esp_log_set_vprintf(log_vprintf);

    esp_console_config_t cc = ESP_CONSOLE_CONFIG_DEFAULT();
    cc.max_cmdline_length = DC_CMD_MAX + 1;
    cc.max_cmdline_args = 16;
    cc.heap_alloc_caps = MALLOC_CAP_SPIRAM;
    ESP_RETURN_ON_ERROR(esp_console_init(&cc), TAG, "console");
    esp_console_register_help_command();
    s_started = true;
    for (int i = 0; i < s_n_pending; i++) {
        register_now(s_pending[i].name, s_pending[i].help, s_pending[i].e);
    }
    s_n_pending = 0;
    cmd_sys_register();
    cmd_io_register();
    cmd_audio_register();
    cmd_ota_register();
    cmd_net_register();

    dc_codec_selfcheck();
    BaseType_t ok = xTaskCreatePinnedToCore(devcon_task, "devcon", TASK_STACK, NULL, TASK_PRIO, &s_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");

    const esp_app_desc_t *app = esp_app_get_description();
    devcon_event("boot", "\"ver\":\"%s\",\"reset\":\"%s\"", app->version,
                 dc_reset_reason_name(esp_reset_reason()));
    return ESP_OK;
}
