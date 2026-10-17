// Console commands for settings, Wi-Fi, the phone portal and HTTPS checks.
#include "devconsole.h"
#include "httpx.h"
#include "net.h"
#include "portal.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"

static int ok(void)
{
    printf("OK\n");
    return 0;
}

static int err(const char *why)
{
    printf("ERR %s\n", why);
    return 1;
}

static int cmd_get(int argc, char **argv)
{
    if (argc < 2) {
        return err("usage: get <key>");
    }
    char v[300];
    settings_format(argv[1], v, sizeof(v));
    printf("%s = %s\n", argv[1], v);
    return ok();
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 2) {
        return err("usage: set <key> [value]");
    }
    const setting_desc_t *d = settings_find(argv[1]);
    if (!d) {
        return err("unknown key");
    }
    esp_err_t e = settings_set_from_string(argv[1], argc > 2 ? argv[2] : "");
    if (e != ESP_OK) {
        return err(esp_err_to_name(e));
    }
    char v[300];
    settings_format(argv[1], v, sizeof(v));
    printf("%s = %s\n", argv[1], v);
    return ok();
}

static int cmd_settings(int argc, char **argv)
{
    size_t n;
    const setting_desc_t *t = settings_table(&n);
    for (size_t i = 0; i < n; i++) {
        char v[300];
        settings_format(t[i].key, v, sizeof(v));
        printf("%-12s %s\n", t[i].key, v);
    }
    if (argc > 1 && !strcmp(argv[1], "reset")) {
        settings_reset();
        printf("settings reset to defaults\n");
    }
    return ok();
}

static const char *auth_name(uint8_t a)
{
    return a == 0 ? "open" : "secured";
}

static int cmd_wifi(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "scan")) {
        net_ap_t aps[20];
        int n = 0;
        esp_err_t e = net_scan(6000, aps, 20, &n);
        if (e != ESP_OK) {
            return err(esp_err_to_name(e));
        }
        for (int i = 0; i < n; i++) {
            printf("%4d dBm  ch%-2u %-7s %s\n", aps[i].rssi, aps[i].ch, auth_name(aps[i].auth), aps[i].ssid);
        }
        return ok();
    }
    if (!strcmp(sub, "connect") && argc > 2) {
        esp_err_t e = net_connect(argv[2], argc > 3 ? argv[3] : "", true);
        return e == ESP_OK ? ok() : err("connect failed");
    }
    if (!strcmp(sub, "disconnect")) {
        net_disconnect();
        return ok();
    }
    if (!strcmp(sub, "forget") && argc > 2) {
        return net_forget(argv[2]) == ESP_OK ? ok() : err("not saved");
    }
    if (!strcmp(sub, "saved")) {
        char ssids[NET_MAX_SAVED][33];
        int n = net_saved_list(ssids, NET_MAX_SAVED);
        for (int i = 0; i < n; i++) {
            printf("%d %s\n", i, ssids[i]);
        }
        return ok();
    }
    if (!strcmp(sub, "status")) {
        net_status_t st;
        net_status(&st);
        printf("state=%s ssid=\"%s\" rssi=%d ip=%s saved=%d portal=%d\n", net_state_name(st.state), st.ssid, st.rssi,
               st.ip, st.saved, portal_active());
        return ok();
    }
    return err("usage: wifi scan | connect <ssid> [pass] | disconnect | forget <ssid> | saved | status");
}

static int cmd_portal(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "start")) {
        if (portal_start() != ESP_OK) {
            return err("start failed");
        }
    } else if (!strcmp(sub, "stop")) {
        portal_stop();
    }
    char ssid[33], pass[12];
    portal_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    printf("portal active=%d ssid=%s pass=%s\n", portal_active(), ssid, pass);
    return ok();
}

static int cmd_net(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "get")) {
        return err("usage: net get <https-url> [auth]");
    }
    char key[210] = "";
    if (argc > 3 && !strcmp(argv[3], "auth")) {
        settings_get_str("ai_key", key, sizeof(key));
    }
    int status, ms;
    size_t bytes;
    char snip[241];
    esp_err_t e = httpx_probe(argv[2], key, &status, &bytes, &ms, snip, sizeof(snip));
    memset(key, 0, sizeof(key));
    printf("net get status=%d bytes=%u ms=%d err=%s\n", status, (unsigned)bytes, ms, esp_err_to_name(e));
    for (char *c = snip; *c; c++) {
        if (*c == '\n' || *c == '\r') {
            *c = ' ';
        }
    }
    printf("body: %s\n", snip);
    return e == ESP_OK ? ok() : err(esp_err_to_name(e));
}

static void reg(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = { .command = name, .help = help, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void cmd_net_register(void)
{
    reg("get", "get <key> - read a setting (secrets masked)", cmd_get);
    reg("set", "set <key> [value] - write a setting", cmd_set);
    reg("settings", "settings [reset] - list all settings", cmd_settings);
    reg("wifi", "wifi scan | connect <ssid> [pass] | disconnect | forget <ssid> | saved | status", cmd_wifi);
    reg("portal", "portal start | stop | status - phone setup network", cmd_portal);
    reg("net", "net get <https-url> [auth] - HTTPS check", cmd_net);
}
