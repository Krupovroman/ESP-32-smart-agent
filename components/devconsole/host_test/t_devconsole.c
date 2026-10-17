// Host regression test of the dev console transport (protocol v2, docs/architecture.md §7):
// framing, OK/ERR payloads, log routing, blobs, uploads, legacy "BLOB:" lines, stalls.
// devconsole.c is included to reach its statics; printf inside it feeds the device stream.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dev_printf(const char *fmt, ...);
#define printf dev_printf
#include "../devconsole.c"
#undef printf
#include "mocks.h"

// What printf does on the device: every character goes through the console's stream.
int dev_printf(const char *fmt, ...)
{
    static char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && i < (int)sizeof(buf) - 1; i++) {
        out_putc(buf[i], NULL);
    }
    return n;
}
#define P dev_printf

static void expect_str(const char *file, int line, const char *got, const char *want)
{
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_fails++;
        printf("  FAIL %s:%d\n    want: %s\n    got:  %s\n", file, line, want, got);
    }
}
#define EXPECT(got, want) expect_str(__FILE__, __LINE__, (got), (want))

// Feeds input lines and runs the console loop (as devcon_task does) until they are consumed.
static char *exec(const char *input)
{
    rx_push(input);
    g_cur_task = s_task;
    while (rx_pending() > 0 || s_rx_pos < s_rx_len) {   // port, or the console's read buffer
        char *line;
        int n = dc_read_line(&line, 50);
        if (n == -2) {
            send_err(0, "ESP_ERR_INVALID_SIZE", "line too long");
        } else if (n > 0) {
            handle_line(line, (size_t)n);
        }
    }
    g_cur_task = NULL;
    CHECK(g_lock_depth == 0);
    return tx_take();
}

static void exec_expect(const char *file, int line, const char *input, const char *want)
{
    char *got = exec(input);
    expect_str(file, line, got, want);
    free(got);
}
#define EXEC(input, want) exec_expect(__FILE__, __LINE__, (input), (want))

static char *b64(const uint8_t *p, size_t n)
{
    char *s = malloc(n / 3 * 4 + 8);
    s[dc_b64_encode(p, n, s)] = 0;
    return s;
}

// ---------------------------------------------------------------- test commands

static int c_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        P("%s\n", argv[i]);
    }
    return 0;
}

static int c_json(int argc, char **argv) { P("{\"a\":1,\"b\":\"x\"}\n"); return 0; }
static int c_mix(int argc, char **argv) { P("note one\n{\"a\":1}\nnote two\n"); return 0; }
static int c_emptyobj(int argc, char **argv) { P("{}\nx\n"); return 0; }
static int c_twojson(int argc, char **argv) { P("{\"a\":1}\n{\"b\":2}\n"); return 0; }
static int c_nonl(int argc, char **argv) { P("abc"); return 0; }
static int c_fail(int argc, char **argv) { return dc_fail("bad %s", "thing"); }
static int c_failout(int argc, char **argv) { P("partial\n"); return dc_fail("nope"); }
static int c_einval(int argc, char **argv) { return ESP_ERR_INVALID_ARG; }
static int c_esc(int argc, char **argv) { P("q\"b\\s\tz\n"); return 0; }
static int c_late(int argc, char **argv) { P("late ok\n"); return 0; }

static int c_log(int argc, char **argv)
{
    ESP_LOGW("t", "inside %d", 5);
    P("done\n");
    return 0;
}

static uint8_t s_blob[7000];
static int c_blob(int argc, char **argv)
{
    return dc_blob("bin", s_blob, sizeof(s_blob), "\"w\":1");
}

static uint8_t s_up[8192];
static esp_err_t up_sink(void *ctx, const uint8_t *data, size_t len, size_t offset)
{
    memcpy(s_up + offset, data, len);
    return ESP_OK;
}

static int c_load(int argc, char **argv)
{
    const char *b = dc_arg(argc, argv, "bytes"), *c = dc_arg(argc, argv, "crc32");
    if (!b || !c) {
        return dc_fail("usage");
    }
    esp_err_t e = dc_upload(strtoul(b, NULL, 10), strtoul(c, NULL, 16), up_sink, NULL);
    if (e != ESP_OK) {
        return e;
    }
    P("{\"got\":%s}\n", b);
    return 0;
}

static uint8_t s_lw[50];
static int c_legacy(int argc, char **argv)
{
    uint32_t crc = esp_rom_crc32_le(0, s_lw, sizeof(s_lw));
    P("BLOB:BEGIN wav %u %08x 24000 1 s16le\n", (unsigned)sizeof(s_lw), (unsigned)crc);
    char *a = b64(s_lw, 30), *b = b64(s_lw + 30, 20);
    P("BLOB:%s\nBLOB:%s\n", a, b);
    free(a);
    free(b);
    P("BLOB:END\n");
    P("done\n");
    return 0;
}

static int c_legacy_shot(int argc, char **argv)
{
    static const uint8_t px[4] = { 0x00, 0xf8, 0x1f, 0x00 };
    P("BLOB:BEGIN shot 4 %08x 2 1 RGB565LE\nBLOB:APgfAA==\nBLOB:END\n", (unsigned)esp_rom_crc32_le(0, px, 4));
    return 0;
}

static int c_flood(int argc, char **argv)
{
    for (int i = 0; i < 400; i++) {   // 400 x 100 B > the 24 KB capture buffer
        P("%099d\n", i);
    }
    return 0;
}

static int c_long(int argc, char **argv)
{
    char line[1501];
    memset(line, 'x', 1500);
    line[1500] = 0;
    P("%s\n{\"n\":1}\n", line);
    return 0;
}

// The console registers these groups in devconsole_start(); here they hold the test commands.
void cmd_sys_register(void)
{
    dc_register_early("echo", "", c_echo);
    dc_register_early("json", "", c_json);
    dc_register_early("mix", "", c_mix);
    dc_register_early("emptyobj", "", c_emptyobj);
    dc_register_early("twojson", "", c_twojson);
    dc_register_early("nonl", "", c_nonl);
    dc_register_early("fail", "", c_fail);
    dc_register_early("failout", "", c_failout);
    dc_register_early("einval", "", c_einval);
    dc_register_early("esc", "", c_esc);
    dc_register_early("log", "", c_log);
}
void cmd_io_register(void) { dc_register_early("blob", "", c_blob); }
void cmd_audio_register(void) { dc_register_early("load", "", c_load); }
void cmd_ota_register(void)
{
    dc_register_early("legacy", "", c_legacy);
    dc_register_early("legacyshot", "", c_legacy_shot);
    dc_register_early("long", "", c_long);
    dc_register_early("flood", "", c_flood);
}
void cmd_net_register(void) {}

// ---------------------------------------------------------------- tests

static void t_framing(void)
{
    EXEC("#1 echo hi there\n", "@1 OK {\"out\":[\"hi\",\"there\"]}\n");
    EXEC("#2 json\n", "@2 OK {\"a\":1,\"b\":\"x\"}\n");
    EXEC("#3 mix\n", "@3 OK {\"a\":1,\"out\":[\"note one\",\"note two\"]}\n");
    EXEC("#4 emptyobj\n", "@4 OK {\"out\":[\"x\"]}\n");
    EXEC("#5 twojson\n", "@5 OK {\"b\":2,\"out\":[\"{\\\"a\\\":1}\"]}\n");
    EXEC("#6 nonl\n", "@6 OK {\"out\":[\"abc\"]}\n");
    EXEC("#7 esc\n", "@7 OK {\"out\":[\"q\\\"b\\\\s\\u0009z\"]}\n");
    EXEC("#8 fail\n", "@8 ERR ESP_FAIL {\"msg\":\"bad thing\"}\n");
    EXEC("#9 failout\n", "@9 ERR ESP_FAIL {\"msg\":\"nope\",\"out\":[\"partial\"]}\n");
    EXEC("#10 einval\n", "@10 ERR ESP_ERR_INVALID_ARG {\"msg\":\"failed\"}\n");
    EXEC("#11 nosuch a b\n", "@11 ERR ESP_ERR_NOT_FOUND {\"msg\":\"unknown command: nosuch a b\"}\n");
    EXEC("#12 echo crlf\r\n", "@12 OK {\"out\":[\"crlf\"]}\n");
    EXEC("#13 echo hx\bi\n", "@13 OK {\"out\":[\"hi\"]}\n");
    EXEC("  #14   echo  sp  \n", "@14 OK {\"out\":[\"sp\"]}\n");
    EXEC("#65535 echo max\n", "@65535 OK {\"out\":[\"max\"]}\n");
    EXEC("#0 echo\n", "@0 ERR ESP_ERR_INVALID_ARG {\"msg\":\"bad #id\"}\n");
    EXEC("#65536 echo\n", "@0 ERR ESP_ERR_INVALID_ARG {\"msg\":\"bad #id\"}\n");
    EXEC("#5x echo\n", "@0 ERR ESP_ERR_INVALID_ARG {\"msg\":\"bad #id\"}\n");
    EXEC("#15\n", "@15 ERR ESP_ERR_INVALID_ARG {\"msg\":\"empty command\"}\n");
    EXEC("\n\r\n", "");
}

static void t_human(void)
{
    // id 0: printed lines go out as they are, results carry no payload, '@' lines are escaped
    EXEC("echo a @1\n", "a\n @1\n@0 OK {}\n");
    EXEC("fail\n", "error: bad thing\n@0 ERR ESP_FAIL {\"msg\":\"bad thing\"}\n");
    // stray upload traffic from an aborted transfer is not a command
    EXEC("D 3 QUJD\nEND\nABORT\n", "");
}

static void t_limits(void)
{
    char *big = malloc(6000);
    memset(big, 'a', 5000);
    strcpy(big + 5000, "\n");
    EXEC(big, "@0 ERR ESP_ERR_INVALID_SIZE {\"msg\":\"line too long\"}\n");
    strcpy(big, "#16 echo ");
    memset(big + 9, 'b', 1100);
    strcpy(big + 1109, "\n");
    EXEC(big, "@16 ERR ESP_ERR_INVALID_SIZE {\"msg\":\"command line too long\"}\n");
    free(big);

    // a captured line longer than the BLOB look-ahead buffer is kept whole
    char *got = exec("#17 long\n");
    char want[1600] = "@17 OK {\"n\":1,\"out\":[\"";
    size_t k = strlen(want);
    memset(want + k, 'x', 1500);
    strcpy(want + k + 1500, "\"]}\n");
    EXPECT(got, want);
    free(got);

    // more output than the capture buffer holds: cut, flagged, still one valid line
    got = exec("#18 flood\n");
    size_t len = strlen(got);
    CHECK(!strncmp(got, "@18 OK {\"out\":[\"", 16));
    CHECK(len > 20000 && len < 26000 && !strcmp(got + len - 21, "\"],\"truncated\":true}\n"));
    CHECK(strchr(got, '\n') == got + len - 1);
    free(got);
}

static void t_logs_and_events(void)
{
    char *got = exec("#20 log\n");
    const char *nl = strchr(got, '\n');
    CHECK(!strncmp(got, "W (", 3) && nl && !strncmp(nl - 12, " t: inside 5\n", 13));
    EXPECT(nl ? nl + 1 : "", "@20 OK {\"out\":[\"done\"]}\n");
    free(got);

    host_log('I', "x", "a\nb");   // one log call with a newline: two log lines, no frame
    got = tx_take();
    nl = strchr(got, '\n');
    CHECK(nl && !strncmp(nl - 4, "x: a\n", 5));
    EXPECT(nl ? nl + 1 : "", "b\n");
    free(got);

    g_now_us = 12345000;
    devcon_event("ota", "\"pct\":%d", 5);
    devcon_event("ready", NULL);
    got = tx_take();
    EXPECT(got, "@! EVT {\"t\":12345,\"ev\":\"ota\",\"pct\":5}\n@! EVT {\"t\":12345,\"ev\":\"ready\"}\n");
    free(got);
    CHECK(g_lock_depth == 0);

    // other tasks wait only briefly for the writer (the holder may be stuck on a stalled host);
    // the console's own result lines wait longer
    CHECK(g_last_lock_wait == LOCK_WAIT_LOG_MS);
    host_log('I', "x", "short wait");
    CHECK(g_last_lock_wait == LOCK_WAIT_LOG_MS);
    P("other task\n");
    CHECK(g_last_lock_wait == LOCK_WAIT_LOG_MS);
    free(tx_take());
    free(exec("#21 echo q\n"));
    CHECK(g_last_lock_wait == LOCK_WAIT_MS);
}

static void t_ready_gate(void)
{
    devconsole_register("late", "", c_late);   // after start: registered at once
    EXEC("#30 late\n", "@30 ERR ESP_ERR_INVALID_STATE {\"msg\":\"not ready\"}\n");
    g_now_us = 2000000;
    devconsole_ready(true);
    char *got = tx_take();
    EXPECT(got, "@! READY {\"ver\":\"host-1\",\"reset\":\"poweron\",\"boot\":7,\"part\":\"ota_0\",\"state\":\"valid\","
                "\"boot_ms\":2000,\"selftest\":\"ok\",\"safe_mode\":false,\"proto\":2}\n");
    free(got);
    EXEC("#31 late\n", "@31 OK {\"out\":[\"late ok\"]}\n");
    CHECK(devconsole_is_ready());
}

static void t_blob(void)
{
    for (size_t i = 0; i < sizeof(s_blob); i++) {
        s_blob[i] = (uint8_t)(i * 7 + (i >> 8));
    }
    char crc[9];
    snprintf(crc, sizeof(crc), "%08x", (unsigned)esp_rom_crc32_le(0, s_blob, sizeof(s_blob)));
    char *got = exec("#40 blob\n");
    // BEGIN, D 0..2 (3072 + 3072 + 856 bytes), END, OK
    char *lines[8] = { 0 };
    int n = 0;
    for (char *p = strtok(got, "\n"); p && n < 8; p = strtok(NULL, "\n")) {
        lines[n++] = p;
    }
    CHECK(n == 6);
    char want[160];
    snprintf(want, sizeof(want), "@40 BEGIN bin {\"w\":1,\"bytes\":7000,\"crc32\":\"%s\"}", crc);
    EXPECT(lines[0] ? lines[0] : "", want);
    uint8_t back[7000];
    size_t got_n = 0;
    for (int i = 1; i <= 3 && i < n; i++) {
        char pre[16];
        snprintf(pre, sizeof(pre), "@40 D %d ", i - 1);
        CHECK(!strncmp(lines[i], pre, strlen(pre)));
        const char *d = lines[i] + strlen(pre);
        int m = dc_b64_decode(d, strlen(d), back + got_n, sizeof(back) - got_n);
        CHECK(m == (i < 3 ? 3072 : 856));
        got_n += m > 0 ? (size_t)m : 0;
    }
    CHECK(got_n == sizeof(s_blob) && !memcmp(back, s_blob, sizeof(s_blob)));
    snprintf(want, sizeof(want), "@40 END {\"bytes\":7000,\"crc32\":\"%s\",\"lines\":3}", crc);
    EXPECT(n > 4 ? lines[4] : "", want);
    EXPECT(n > 5 ? lines[5] : "", "@40 OK {}");
    free(got);
}

static char *upload_input(int id, const uint8_t *data, size_t len, const char *crc_override, const char *tail)
{
    size_t cap = 64 + len * 2 + (tail ? strlen(tail) : 0);
    char *in = malloc(cap);
    uint32_t crc = esp_rom_crc32_le(0, data, (uint32_t)len);
    int o = snprintf(in, cap, "#%d load bytes=%u crc32=", id, (unsigned)len);
    o += crc_override ? snprintf(in + o, cap - o, "%s\n", crc_override) : snprintf(in + o, cap - o, "%08x\n",
                                                                                     (unsigned)crc);
    for (size_t off = 0, seq = 0; off < len; off += DC_UP_CHUNK, seq++) {
        size_t k = len - off < DC_UP_CHUNK ? len - off : DC_UP_CHUNK;
        char *b = b64(data + off, k);
        o += snprintf(in + o, cap - o, "D %u %s\n", (unsigned)seq, b);
        free(b);
    }
    snprintf(in + o, cap - o, "%s", tail ? tail : "END\n");
    return in;
}

static void t_upload(void)
{
    uint8_t data[2500];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 13 + 5);
    }
    char *in = upload_input(50, data, sizeof(data), NULL, NULL);
    EXEC(in, "@50 READY {\"chunk\":1024,\"window\":2}\n@50 ACK 1\n@50 OK {\"got\":2500}\n");
    CHECK(!memcmp(s_up, data, sizeof(data)));
    free(in);

    in = upload_input(51, data, sizeof(data), "deadbeef", NULL);
    char *got = exec(in);
    CHECK(strstr(got, "@51 ERR ESP_ERR_INVALID_CRC {\"msg\":\"crc mismatch: ") != NULL);
    CHECK(strstr(got, " != deadbeef\"}\n") != NULL);
    free(got);
    free(in);

    in = upload_input(52, data, 1024, NULL, "ABORT\n");
    EXEC(in, "@52 READY {\"chunk\":1024,\"window\":2}\n@52 ERR ESP_ERR_INVALID_STATE {\"msg\":\"aborted by host\"}\n");
    free(in);

    // announced 2500 but END after one chunk
    in = upload_input(53, data, 1024, NULL, "END\n");
    memcpy(strstr(in, "bytes=1024"), "bytes=2500", 10);
    got = exec(in);
    CHECK(strstr(got, "@53 ERR ESP_ERR_INVALID_SIZE {\"msg\":\"size mismatch: got 1024 of 2500\"}") != NULL);
    free(got);
    free(in);

    EXEC("#54 load bytes=10 crc32=0\nD 1 QUJD\n",
         "@54 READY {\"chunk\":1024,\"window\":2}\n"
         "@54 ERR ESP_ERR_INVALID_RESPONSE {\"msg\":\"unexpected line (want D 0)\"}\n");
    EXEC("#55 load bytes=10 crc32=0\nD 0 QU*D\n",
         "@55 READY {\"chunk\":1024,\"window\":2}\n@55 ERR ESP_ERR_INVALID_ARG {\"msg\":\"bad base64 in D 0\"}\n");

    // the host goes silent after one chunk: 5 s later the upload fails, the console lives on
    in = upload_input(56, data, 1024, NULL, "");
    memcpy(strstr(in, "bytes=1024"), "bytes=2500", 10);
    int64_t t0 = g_now_us;
    EXEC(in, "@56 READY {\"chunk\":1024,\"window\":2}\n"
             "@56 ERR ESP_ERR_TIMEOUT {\"msg\":\"upload timeout after 1024 bytes\"}\n");
    CHECK(g_now_us - t0 >= 5000000);
    free(in);
    EXEC("D 1 QUJD\nEND\n#57 echo alive\n", "@57 OK {\"out\":[\"alive\"]}\n");
}

static void t_legacy_blob(void)
{
    for (size_t i = 0; i < sizeof(s_lw); i++) {
        s_lw[i] = (uint8_t)(200 - i);
    }
    char crc[9];
    snprintf(crc, sizeof(crc), "%08x", (unsigned)esp_rom_crc32_le(0, s_lw, sizeof(s_lw)));
    char *a = b64(s_lw, 30), *b = b64(s_lw + 30, 20);
    char want[600];
    snprintf(want, sizeof(want),
             "@60 BEGIN wav {\"rate\":24000,\"ch\":1,\"fmt\":\"s16le\",\"bytes\":50,\"crc32\":\"%s\",\"legacy\":true}\n"
             "@60 D 0 %s\n@60 D 1 %s\n@60 END {\"bytes\":50,\"crc32\":\"%s\",\"lines\":2}\n"
             "@60 OK {\"out\":[\"done\"]}\n", crc, a, b, crc);
    EXEC("#60 legacy\n", want);

    static const uint8_t px[4] = { 0x00, 0xf8, 0x1f, 0x00 };
    snprintf(crc, sizeof(crc), "%08x", (unsigned)esp_rom_crc32_le(0, px, 4));
    snprintf(want, sizeof(want),
             "@61 BEGIN shot {\"args\":\"2 1 RGB565LE\",\"bytes\":4,\"crc32\":\"%s\",\"legacy\":true}\n"
             "@61 D 0 APgfAA==\n@61 END {\"bytes\":4,\"crc32\":\"%s\",\"lines\":1}\n@61 OK {}\n", crc, crc);
    EXEC("#61 legacyshot\n", want);

    // a human (id 0) still sees the v1 lines as they are
    char *got = exec("legacy\n");
    snprintf(crc, sizeof(crc), "%08x", (unsigned)esp_rom_crc32_le(0, s_lw, sizeof(s_lw)));
    snprintf(want, sizeof(want), "BLOB:BEGIN wav 50 %s 24000 1 s16le\nBLOB:%s\nBLOB:%s\nBLOB:END\ndone\n@0 OK {}\n",
             crc, a, b);
    EXPECT(got, want);
    free(got);
    free(a);
    free(b);
}

static void t_stalled_host(void)
{
    // the TX ring takes 512 more bytes, then the host stops reading in the middle of a line
    g_tx_room = 512;
    int64_t t0 = g_now_us;
    char *got = exec("long\n");                // id 0: a 1500-character line, then {"n":1} and the OK
    CHECK(strlen(got) == 512);                 // the head of the line; the rest is dropped
    CHECK(g_now_us - t0 < 3000000);            // bounded waits, no hang
    free(got);
    CHECK(s_need_nl);
    // the host reads again: the cut line is terminated before the next frame
    g_tx_room = SIZE_MAX;
    EXEC("#70 echo y\n", "\n@70 OK {\"out\":[\"y\"]}\n");
    CHECK(!s_need_nl);

    // no host at all: everything is dropped at once
    g_usb_connected = false;
    uint32_t dropped = s_dropped;
    t0 = g_now_us;
    EXEC("#71 echo z\n", "");
    CHECK(s_dropped > dropped && g_now_us - t0 < 1000000);
    g_usb_connected = true;
    EXEC("#72 echo back\n", "@72 OK {\"out\":[\"back\"]}\n");
}

int main(void)
{
    // registered before the console starts (pending list), like ai_console_register()
    devconsole_register("late0", "", c_late);
    CHECK(devconsole_start() == ESP_OK);
    char *boot = tx_take();
    CHECK(!strncmp(boot, "@! EVT {\"t\":", 12) && strstr(boot, "\"ev\":\"boot\",\"ver\":\"host-1\"") != NULL);
    free(boot);
    CHECK(dc_codec_selfcheck());
    CHECK(esp_rom_crc32_le(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(devconsole_running());

    t_framing();
    t_human();
    t_limits();
    t_logs_and_events();
    t_ready_gate();
    EXEC("#80 late0\n", "@80 OK {\"out\":[\"late ok\"]}\n");
    t_blob();
    t_upload();
    t_legacy_blob();
    t_stalled_host();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
