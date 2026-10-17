// OTA commands (docs/architecture.md §8): ota info|push|url|boot|rollback|mark.
#include "dc_internal.h"
#include "ota.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_system.h"

static void print_info(void)
{
    ota_info_t o;
    ota_get_info(&o);
    printf("{\"running\":\"%s\",\"boot\":\"%s\",\"next\":\"%s\",\"state\":\"%s\",\"ver\":\"%s\",\"next_ver\":\"%s\","
           "\"ok_ver\":\"%s\",\"rb_reason\":\"%s\",\"rb_cnt\":%u,\"boot_cnt\":%u,\"rollback_possible\":%s,"
           "\"selftest\":\"%s\",\"busy\":%s}\n",
           o.running, o.boot, o.next, ota_state_name(o.state), o.ver, o.next_ver, o.ok_ver, o.rb_reason,
           o.rb_cnt, (unsigned)o.boot_cnt, o.rollback_possible ? "true" : "false", ota_selftest_name(o.selftest),
           o.busy ? "true" : "false");
}

static esp_err_t push_sink(void *ctx, const uint8_t *data, size_t len, size_t offset)
{
    return ota_stream_write(data, len);
}

static void restart_cb(void *arg)
{
    esp_restart();
}

// ota push bytes=N crc32=X [noreboot]: the image goes to the inactive slot and boots in
// PENDING_VERIFY, so the self-test decides whether it stays.
static int ota_push(int argc, char **argv)
{
    const char *b = dc_arg(argc, argv, "bytes"), *c = dc_arg(argc, argv, "crc32");
    if (!b || !c) {
        return dc_fail("usage: ota push bytes=<n> crc32=<hex> [noreboot]");
    }
    bool reboot = true;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "noreboot")) {
            reboot = false;
        }
    }
    size_t bytes = strtoul(b, NULL, 10);
    uint32_t crc = strtoul(c, NULL, 16);
    esp_err_t e = ota_stream_begin(bytes);
    if (e != ESP_OK) {
        dc_fail("cannot start: %s", esp_err_to_name(e));
        return e;
    }
    e = dc_upload(bytes, crc, push_sink, NULL);
    if (e != ESP_OK) {
        ota_stream_abort();
        return e;
    }
    e = ota_stream_end(true);
    if (e != ESP_OK) {
        dc_fail("image rejected: %s", esp_err_to_name(e));
        return e;
    }
    ota_info_t o;
    ota_get_info(&o);
    printf("{\"bytes\":%u,\"boot\":\"%s\",\"ver\":\"%s\",\"reboot\":%s}\n", (unsigned)bytes, o.boot, o.next_ver,
           reboot ? "true" : "false");
    if (reboot) {
        dc_defer(restart_cb, NULL);
    }
    return 0;
}

static void progress(int pct, size_t bytes, size_t total, bool done, esp_err_t err)
{
    if (done) {
        devcon_event("ota", "\"pct\":%d,\"bytes\":%u,\"total\":%u,\"done\":true,\"err\":\"%s\"", pct,
                     (unsigned)bytes, (unsigned)total, esp_err_to_name(err));
    } else {
        devcon_event("ota", "\"pct\":%d,\"bytes\":%u,\"total\":%u", pct, (unsigned)bytes, (unsigned)total);
    }
}

// mark/rollback act on the slot selected for the next boot: refuse while that is not this one
static bool selected_is_running(const ota_info_t *o)
{
    if (strcmp(o->boot, o->running)) {
        dc_fail("%s is selected for the next boot: reboot first, or ota boot %s", o->boot, o->running);
        return false;
    }
    return true;
}

static int cmd_ota(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "info";
    if (!strcmp(sub, "info")) {
        print_info();
        return 0;
    }
    if (!strcmp(sub, "push")) {
        return ota_push(argc, argv);
    }
    if (!strcmp(sub, "url")) {
        // a typed URL is passed on as it is (ota_start_https copies it), so it is never cut
        char saved[204] = "";
        if (argc <= 2) {
            settings_get_str("ota_url", saved, sizeof(saved));
        }
        const char *url = argc > 2 ? argv[2] : saved;
        if (!url[0]) {
            return dc_fail("usage: ota url <https-url> (or set ota_url)");
        }
        ota_set_progress_cb(progress);
        esp_err_t e = ota_start_https(url);
        if (e != ESP_OK) {
            dc_fail("cannot start: %s", esp_err_to_name(e));
            return e;
        }
        printf("{\"started\":true}\n");
        return 0;
    }
    if (!strcmp(sub, "boot") && argc > 2) {
        esp_err_t e = ota_set_boot(argv[2]);
        if (e != ESP_OK) {
            dc_fail("cannot boot %s: %s", argv[2], esp_err_to_name(e));
            return e;
        }
        printf("{\"boot\":\"%s\",\"reboot\":true}\n", argv[2]);
        dc_defer(restart_cb, NULL);
        return 0;
    }
    if (!strcmp(sub, "rollback")) {
        ota_info_t o;
        ota_get_info(&o);
        if (!selected_is_running(&o)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!o.rollback_possible) {
            return dc_fail("no valid previous image");
        }
        // mark now and restart after the OK line, so a refused mark is reported as ERR
        esp_err_t e = ota_mark(false);
        if (e != ESP_OK) {
            dc_fail("cannot mark %s invalid: %s", o.running, esp_err_to_name(e));
            return e;
        }
        printf("{\"rollback\":true,\"to\":\"%s\"}\n", o.next);
        dc_defer(restart_cb, NULL);
        return 0;
    }
    if (!strcmp(sub, "mark") && argc > 2) {
        bool valid = !strcmp(argv[2], "valid");
        if (!valid && strcmp(argv[2], "invalid")) {
            return dc_fail("usage: ota mark valid|invalid");
        }
        ota_info_t o;
        ota_get_info(&o);
        if (!selected_is_running(&o)) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t e = ota_mark(valid);
        if (e != ESP_OK) {
            dc_fail("mark %s: %s", argv[2], esp_err_to_name(e));
            return e;
        }
        print_info();
        return 0;
    }
    return dc_fail("usage: ota info | push bytes=N crc32=X [noreboot] | url [https-url] | boot ota_0|ota_1 | "
                   "rollback | mark valid|invalid");
}

void cmd_ota_register(void)
{
    dc_register_early("ota", "ota info | push bytes=N crc32=X [noreboot] | url [https-url] | boot ota_0|ota_1 | "
                      "rollback | mark valid|invalid", cmd_ota);
}
