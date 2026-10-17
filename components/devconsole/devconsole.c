#include "devconsole.h"
#include "board.h"
#include "display.h"
#include "inject.h"
#include "touch.h"
#include "audio_in.h"
#include "audio_out.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCREEN_W 360
#define SCREEN_H 360

// Commands print their own lines and return 0/1; the trampoline prints the final OK/ERR.
static int ok(void)
{
    return 0;
}

static int err(const char *why)
{
    return dc_fail("%s", why);
}

int dc_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("error: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    return 1;
}

static int trampoline(void *ctx, int argc, char **argv)
{
    int rc = ((dc_cmd_fn)ctx)(argc, argv);
    printf(rc == 0 ? "OK\n" : "ERR %d\n", rc);
    fflush(stdout);
    return rc;
}

#define MAX_PENDING 48
typedef struct {
    const char *name, *help;
    dc_cmd_fn fn;
} pending_t;
static pending_t s_pending[MAX_PENDING];
static int s_n_pending;
static bool s_started;

static void register_now(const char *name, const char *help, dc_cmd_fn fn)
{
    const esp_console_cmd_t cmd = {
        .command = name, .help = help, .func_w_context = trampoline, .context = (void *)fn,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void devconsole_register(const char *name, const char *help, dc_cmd_fn fn)
{
    if (s_started) {
        register_now(name, help, fn);
    } else if (s_n_pending < MAX_PENDING) {
        s_pending[s_n_pending++] = (pending_t){ name, help, fn };
    }
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    default: return "other";
    }
}

static void print_heap(void)
{
    printf("heap internal free=%u min=%u largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("heap dma free=%u largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    printf("heap psram free=%u min=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

static int cmd_info(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const display_info_t *d = display_info();
    const esp_partition_t *run = esp_ota_get_running_partition();
    printf("app %s %s (%s %s) idf %s\n", app->project_name, app->version, app->date, app->time, esp_get_idf_version());
    printf("partition %s @0x%06x\n", run ? run->label : "?", run ? (unsigned)run->address : 0);
    printf("reset %s uptime_ms %lld\n", reset_reason_str(esp_reset_reason()), esp_timer_get_time() / 1000);
    printf("panel id %02x %02x %02x %02x batch %d pclk %u\n", d->panel_id[0], d->panel_id[1], d->panel_id[2],
           d->panel_id[3], d->panel_batch, (unsigned)d->pclk_hz);
    printf("backlight %u\n", board_backlight_get());
    print_heap();
    return ok();
}

static int cmd_heap(int argc, char **argv)
{
    print_heap();
    return ok();
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void print_b64_line(const uint8_t *p, size_t n)
{
    char line[8 + 4 * 32 + 2];
    size_t o = 0;
    memcpy(line, "BLOB:", 5);
    o = 5;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        line[o++] = B64[(v >> 18) & 63];
        line[o++] = B64[(v >> 12) & 63];
        line[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        line[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    line[o++] = '\n';
    line[o] = 0;
    fputs(line, stdout);
}

// Binary payloads go out as base64 lines between BLOB:BEGIN and BLOB:END, with a CRC32.
static void dump_blob(const char *kind, const uint8_t *p, size_t len, const char *meta)
{
    uint32_t crc = esp_rom_crc32_le(0, p, len);
    printf("BLOB:BEGIN %s %u %08lx %s\n", kind, (unsigned)len, (unsigned long)crc, meta);
    for (size_t i = 0; i < len; i += 96) {
        print_b64_line(p + i, len - i < 96 ? len - i : 96);
    }
    printf("BLOB:END\n");
    fflush(stdout);
}

static int cmd_shot(int argc, char **argv)
{
    const uint16_t *fb = display_capture();
    if (!fb) {
        return err("no shadow buffer");
    }
    dump_blob("shot", (const uint8_t *)fb, SCREEN_W * SCREEN_H * 2, "360 360 RGB565LE");
    return ok();
}

static void print_clip(const audio_clip_t *c)
{
    printf("clip ms=%u samples=%u peak=%d rms_dbfs=%.1f\n", (unsigned)c->ms, (unsigned)c->samples, c->peak,
           c->rms_dbfs_x10 / 10.0);
}

static int cmd_mic(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "probe")) {
        audio_probe_t pr;
        uint32_t ms = argc > 2 ? (uint32_t)atoi(argv[2]) : 600;
        if (audio_in_probe(ms, &pr) != ESP_OK) {
            return err("busy");
        }
        printf("right rms_dbfs=%.1f zero=%.3f\n", pr.rms_dbfs_x10[0] / 10.0, pr.zero_permille[0] / 1000.0);
        printf("left  rms_dbfs=%.1f zero=%.3f\n", pr.rms_dbfs_x10[1] / 10.0, pr.zero_permille[1] / 1000.0);
        return ok();
    }
    if (!strcmp(sub, "rec")) {
        uint32_t ms = argc > 2 ? (uint32_t)atoi(argv[2]) : 3000;
        audio_in_start(ms);
        while (audio_in_recording()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        print_clip(audio_in_last());
        return ok();
    }
    if (!strcmp(sub, "dump")) {
        const audio_clip_t *c = audio_in_last();
        if (!c) {
            return err("no clip");
        }
        size_t len = 0;
        uint8_t *wav = audio_clip_to_wav(c, &len);
        if (!wav) {
            return err("no memory");
        }
        char meta[32];
        snprintf(meta, sizeof(meta), "%u 1 s16le", (unsigned)c->rate);
        dump_blob("wav", wav, len, meta);
        free(wav);
        return ok();
    }
    if (!strcmp(sub, "rate")) {
        if (argc > 2) {
            audio_in_set_rate((uint32_t)atoi(argv[2]));
        }
        printf("rate %u\n", (unsigned)audio_in_get_rate());
        return ok();
    }
    if (!strcmp(sub, "level")) {
        printf("level %d\n", audio_in_level());
        return ok();
    }
    if (!strcmp(sub, "fmt") && argc > 3) {
        audio_in_set_format(!strcmp(argv[2], "right"), atoi(argv[3]));
        return ok();
    }
    return err("usage: mic probe [ms] | rec <ms> | dump | level | fmt right|left <shift>");
}

static int cmd_spk(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "tone")) {
        uint32_t hz = argc > 2 ? (uint32_t)atoi(argv[2]) : 440;
        uint32_t ms = argc > 3 ? (uint32_t)atoi(argv[3]) : 500;
        uint8_t amp = argc > 4 ? (uint8_t)atoi(argv[4]) : 50;
        audio_out_tone(hz, ms, amp);
        return ok();
    }
    if (!strcmp(sub, "vol") && argc > 2) {
        audio_out_set_volume((uint8_t)atoi(argv[2]));
        return ok();
    }
    if (!strcmp(sub, "stat")) {
        audio_out_stats_t st;
        audio_out_get_stats(&st);
        printf("rate=%u played=%llu queued=%u write_errors=%u volume=%u busy=%d\n", (unsigned)st.rate,
               (unsigned long long)st.bytes_played, (unsigned)st.queued_bytes, (unsigned)st.write_errors, st.volume,
               audio_out_busy());
        return ok();
    }
    return err("usage: spk tone <hz> <ms> [amp] | vol <0-100> | stat");
}

static bool parse_xy(int argc, char **argv, int first, int *x, int *y)
{
    if (argc < first + 2) {
        return false;
    }
    *x = atoi(argv[first]);
    *y = atoi(argv[first + 1]);
    return *x >= 0 && *x < SCREEN_W && *y >= 0 && *y < SCREEN_H;
}

static int cmd_tap(int argc, char **argv)
{
    int x, y;
    if (!parse_xy(argc, argv, 1, &x, &y)) {
        return err("usage: tap x y [hold_ms]");
    }
    int hold = argc > 3 ? atoi(argv[3]) : 120;
    inject_set(x, y, true);
    vTaskDelay(pdMS_TO_TICKS(hold));
    inject_set(x, y, false);
    vTaskDelay(pdMS_TO_TICKS(60));
    return ok();
}

static int cmd_press(int argc, char **argv)
{
    int x, y;
    if (!parse_xy(argc, argv, 1, &x, &y)) {
        return err("usage: press x y");
    }
    inject_set(x, y, true);
    return ok();
}

static int cmd_release(int argc, char **argv)
{
    int x = argc > 2 ? atoi(argv[1]) : 0, y = argc > 2 ? atoi(argv[2]) : 0;
    inject_set(x, y, false);
    return ok();
}

static int cmd_swipe(int argc, char **argv)
{
    int x1, y1, x2, y2;
    if (!parse_xy(argc, argv, 1, &x1, &y1) || !parse_xy(argc, argv, 3, &x2, &y2)) {
        return err("usage: swipe x1 y1 x2 y2 [ms]");
    }
    int ms = argc > 5 ? atoi(argv[5]) : 250;
    int steps = ms / 16 > 2 ? ms / 16 : 2;
    for (int i = 0; i <= steps; i++) {
        inject_set(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, true);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    inject_set(x2, y2, false);
    vTaskDelay(pdMS_TO_TICKS(60));
    return ok();
}

static int cmd_bl(int argc, char **argv)
{
    if (argc < 2) {
        return err("usage: bl 0-100");
    }
    board_backlight_set((uint8_t)atoi(argv[1]));
    return ok();
}

static int cmd_invert(int argc, char **argv)
{
    if (argc < 2) {
        return err("usage: invert 0|1");
    }
    return display_set_invert(atoi(argv[1]) != 0) == ESP_OK ? ok() : err("panel");
}

static int cmd_touchlog(int argc, char **argv)
{
    touch_set_log(argc > 1 && atoi(argv[1]) != 0);
    return ok();
}

static int cmd_reboot(int argc, char **argv)
{
    printf("OK\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static void reg(const char *name, const char *help, dc_cmd_fn fn)
{
    devconsole_register(name, help, fn);
}

esp_err_t devconsole_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "ra>";
    repl_cfg.max_cmdline_length = 512;
    repl_cfg.task_stack_size = 6144;
    esp_console_dev_usb_serial_jtag_config_t hw_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();
    s_started = true;
    for (int i = 0; i < s_n_pending; i++) {
        register_now(s_pending[i].name, s_pending[i].help, s_pending[i].fn);
    }
    reg("info", "Firmware, panel and memory info", cmd_info);
    reg("heap", "Heap statistics", cmd_heap);
    reg("shot", "Screenshot as base64 RGB565 (SHOT: lines)", cmd_shot);
    reg("tap", "tap x y [hold_ms] - inject a touch", cmd_tap);
    reg("press", "press x y - hold an injected touch", cmd_press);
    reg("move", "move x y - move the held injected touch", cmd_press);
    reg("release", "release - end the injected touch", cmd_release);
    reg("swipe", "swipe x1 y1 x2 y2 [ms]", cmd_swipe);
    reg("bl", "bl 0-100 - backlight", cmd_bl);
    reg("invert", "invert 0|1 - panel colour inversion", cmd_invert);
    reg("touchlog", "touchlog 0|1 - log raw touch points", cmd_touchlog);
    reg("mic", "mic probe [ms] | rec <ms> | dump | level | fmt right|left <shift>", cmd_mic);
    reg("spk", "spk tone <hz> <ms> [amp] | vol <0-100> | stat", cmd_spk);
    reg("reboot", "Restart the chip", cmd_reboot);
    cmd_net_register();
    return esp_console_start_repl(repl);
}
