// "ui" dev-console command: drives the UI without the app (docs/ui.md 7.9). Commands post through the
// UI queue and wait until it is pumped, so scripted sequences see the result of the previous line.
#include "ui.h"
#include "ui_internal.h"
#include "ui_kb.h"
#include "ui_list.h"
#include "ui_orb.h"
#include "ui_rim.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "devconsole.h"
#include "inject.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LOCK_MS 1000
#define PUMP_WAIT_MS 500

static const char *const s_state_names[UI_ST__N] = {
    "idle", "listening", "latched", "cancel", "uploading", "thinking", "speaking", "answered", "error", "offline",
};
static const char *const s_mood_names[] = { "neutral", "happy", "sad", "surprised", "thinking" };
static const char *const s_fault_names[UI_FAULT__N] = {
    "none", "nowifi", "badkey", "nocredit", "lowcredit", "region", "model", "expiring", "mic",
};
static const char *const s_card_names[UI_CARD__N] = {
    "generic", "nowifi", "badkey", "nocredit", "region", "model", "rate", "server", "tls", "mic", "nospeaker",
};
static const char *const s_net_names[] = { "off", "connecting", "up", "failed" };

// Index of name in a table, or a plain number; -1 if neither.
static int lookup(const char *name, const char *const *tab, int n)
{
    for (int i = 0; i < n; i++) {
        if (tab[i] && strcmp(name, tab[i]) == 0) {
            return i;
        }
    }
    char *end;
    long v = strtol(name, &end, 10);
    return (*end == 0 && v >= 0 && v < n) ? (int)v : -1;
}

// argv[first..] joined with spaces into a PSRAM string (NULL when empty)
static char *join(int argc, char **argv, int first)
{
    size_t n = 1;
    for (int i = first; i < argc; i++) {
        n += strlen(argv[i]) + 1;
    }
    if (first >= argc) {
        return NULL;
    }
    char *s = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!s) {
        return NULL;
    }
    s[0] = 0;
    for (int i = first; i < argc; i++) {
        if (i > first) {
            strcat(s, " ");
        }
        strcat(s, argv[i]);
    }
    return s;
}

static int post(ui_msg_type_t type, int32_t i, char *s)
{
    ui_post(&(ui_msg_t){ .type = type, .i = i, .s = s });
    if (!ui_wait_pumped(PUMP_WAIT_MS)) {
        return dc_fail("ui queue not drained (dropped %u)", ui_dropped());
    }
    return 0;
}

static void print_line(const char *line)
{
    printf("%s\n", line);
}

typedef struct {
    char *buf;
    size_t len, o;
    int n;
} tree_ctx_t;

// One line per named object: name, class, coords, Rmax of the visible part, C(lickable), V(isible)/H(idden)
static void tree_line(lv_obj_t *obj, void *ctx)
{
    tree_ctx_t *t = ctx;
    char name[UI_NAME_MAX];
    lv_obj_get_name_resolved(obj, name, sizeof(name));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_area_t va;
    bool vis = ui_obj_visible(obj) && ui_obj_visible_area(obj, &va);
    float rmax = vis ? ui_obj_rmax(obj, &va) : 0;
    char text[40] = "";
    if (lv_obj_check_type(obj, &lv_label_class)) {
        // first 32 bytes on one line: newlines would split the record, a cut UTF-8 sequence garbles it
        char snip[33];
        ui_str_copy(snip, sizeof(snip), lv_label_get_text(obj));
        for (char *c = snip; *c; c++) {
            if ((unsigned char)*c < 0x20) {
                *c = ' ';
            }
        }
        snprintf(text, sizeof(text), " \"%s\"", snip);
    }
    if (t->o + 160 >= t->len) {
        return;
    }
    t->o += (size_t)snprintf(t->buf + t->o, t->len - t->o, "%-18s %-11s x=%-4d y=%-4d w=%-4d h=%-4d rmax=%-6.1f %s%s%s\n",
                             name, ui_obj_type(obj), (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a),
                             (int)lv_area_get_height(&a), (double)rmax,
                             lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? "C" : "-", vis ? "V" : "H", text);
    t->n++;
}

static int cmd_tree(void)
{
    // collect under the lock, print after it: a slow USB host must never stall rendering
    tree_ctx_t t = { heap_caps_malloc(16384, MALLOC_CAP_SPIRAM), 16384, 0, 0 };
    if (!t.buf) {
        return dc_fail("no memory");
    }
    t.buf[0] = 0;
    if (!ui_lock(LOCK_MS)) {
        heap_caps_free(t.buf);
        return dc_fail("lvgl busy");
    }
    ui_walk_named(tree_line, &t);
    ui_unlock();
    printf("screen %s\n", ui_screen_name(ui_nav_current()));
    for (char *line = t.buf, *nl; *line; line = nl + 1) {
        nl = strchr(line, '\n');
        if (!nl) {
            break;
        }
        printf("%.*s\n", (int)(nl - line), line);
    }
    printf("objects %d\n", t.n);
    heap_caps_free(t.buf);
    return 0;
}

// One injected tap; never under the LVGL lock: taskLVGL must run to read the samples
static void tap_at(lv_point_t c, int hold_ms, int after_ms)
{
    inject_set(c.x, c.y, true);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    inject_set(c.x, c.y, false);
    vTaskDelay(pdMS_TO_TICKS(after_ms));
}

static int cmd_tap(int argc, char **argv)
{
    if (argc < 3) {
        return dc_fail("usage: ui tap <name> | key:<ok|bksp|space|lang|digits|symbols|letters|back|label> [hold_ms]");
    }
    int hold = argc > 3 ? atoi(argv[3]) : 120;
    if (ui_asleep() && post(UI_WAKE, 0, NULL) != 0) {
        return 1;       // a dimmed screen would swallow the tap
    }
    if (!ui_lock(LOCK_MS)) {
        return dc_fail("lvgl busy");
    }
    lv_point_t c = { 0, 0 };
    if (!strncmp(argv[2], "key:", 4)) {
        // a key of the round keyboard (keys are buttonmatrix areas, not objects)
        bool found = ui_kb_key_point(argv[2] + 4, &c);
        ui_unlock();
        if (!found) {
            return dc_fail("no key '%s' on the current layout (ui keys)", argv[2] + 4);
        }
        tap_at(c, hold > 0 ? hold : 120, 80);
        printf("tap %s at %d,%d\n", argv[2], (int)c.x, (int)c.y);
        return 0;
    }
    lv_obj_t *o = ui_find(argv[2]);
    if (o && ui_list_is_row(o)) {
        ui_list_focus(o);           // rows scroll to the centre line first (they fade towards the rim)
    }
    lv_area_t a = { 0 };
    bool vis = o && ui_obj_visible(o) && ui_obj_visible_area(o, &a);
    c = (lv_point_t){ (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 };
    if (o && lv_obj_has_flag(o, LV_OBJ_FLAG_ADV_HITTEST)) {
        ui_orb_center(o, &c);      // the orb's hit test is a circle around its centre
    }
    ui_unlock();
    if (!o) {
        return dc_fail("no object '%s' on screen %s", argv[2], ui_screen_name(ui_nav_current()));
    }
    if (!vis) {
        return dc_fail("'%s' is not visible", argv[2]);
    }
    tap_at(c, hold > 0 ? hold : 120, 80);
    printf("tap %s at %d,%d\n", argv[2], (int)c.x, (int)c.y);
    return 0;
}

static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int n = 1;
    if (c >= 0xF0 && p[1] && p[2] && p[3]) {
        c = ((c & 7u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        n = 4;
    } else if (c >= 0xE0 && p[1] && p[2]) {
        c = ((c & 0xFu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        n = 3;
    } else if (c >= 0xC0 && p[1]) {
        c = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        n = 2;
    }
    *s += n;
    return c;
}

// ui kb [-ok] <utf8 text>: types through real taps on the key areas, including group -> letter steps, page and
// language switches and the "Aa" button of the hex keypad; -ok taps ✓ at the end
static int cmd_kb(int argc, char **argv)
{
    int first = 2;
    bool ok = false;
    if (argc > 2 && !strcmp(argv[2], "-ok")) {
        ok = true;
        first = 3;
    }
    char *text = join(argc, argv, first);
    const char *p = text ? text : "";
    if (ui_asleep() && post(UI_WAKE, 0, NULL) != 0) {
        heap_caps_free(text);
        return 1;
    }
    if (!ui_lock(LOCK_MS)) {
        heap_caps_free(text);
        return dc_fail("lvgl busy");
    }
    lv_obj_t *ta = ui_find("text_field");
    // the hex field already holds the protected prefix: a full key may be typed as it is
    if (ta && !strncmp(p, UI_KEY_PREFIX, strlen(UI_KEY_PREFIX)) && ui_kb_hex() &&
        !strncmp(lv_textarea_get_text(ta), UI_KEY_PREFIX, strlen(UI_KEY_PREFIX))) {
        p += strlen(UI_KEY_PREFIX);
    }
    ui_unlock();
    if (!ta) {
        heap_caps_free(text);
        return dc_fail("no keyboard on screen %s (ui goto text)", ui_screen_name(ui_nav_current()));
    }
    int typed = 0, taps = 0;
    while (*p) {
        const char *at = p;
        uint32_t cp = next_cp(&p);
        bool done = false;
        for (int step = 0; step < 8 && !done; step++) {
            lv_point_t pt = { 0, 0 };
            if (!ui_lock(LOCK_MS)) {
                heap_caps_free(text);
                return dc_fail("lvgl busy");
            }
            ui_kb_plan_t pl = ui_kb_plan(cp, &pt);
            if (pl == UI_KB_AA) {
                lv_obj_t *aa = ui_find("text_aa");
                lv_area_t a;
                if (aa && ui_obj_visible_area(aa, &a)) {
                    pt = (lv_point_t){ (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 };
                } else {
                    pl = UI_KB_NONE;
                }
            }
            ui_unlock();
            if (pl == UI_KB_NONE) {
                int off = (int)(at - (text ? text : ""));
                heap_caps_free(text);
                return dc_fail("cannot type U+%04X (byte %d) on this field; typed %d", (unsigned)cp, off, typed);
            }
            tap_at(pt, 70, 110);
            taps++;
            done = pl == UI_KB_TYPES;
        }
        if (!done) {
            heap_caps_free(text);
            return dc_fail("no path to U+%04X; typed %d", (unsigned)cp, typed);
        }
        typed++;
    }
    heap_caps_free(text);
    if (ok) {
        lv_point_t pt;
        if (!ui_lock(LOCK_MS)) {
            return dc_fail("lvgl busy");
        }
        bool found = ui_kb_key_point("ok", &pt);
        ui_unlock();
        if (!found) {
            return dc_fail("no OK key (typed %d)", typed);
        }
        tap_at(pt, 70, 150);
        taps++;
    }
    // report what the field holds (secrets only by length)
    char shown[96] = "", esc[200];
    unsigned len = 0;
    if (ui_lock(LOCK_MS)) {
        lv_obj_t *t = ui_find("text_field");
        if (t) {
            const char *v = lv_textarea_get_text(t);
            len = (unsigned)strlen(v);
            if (!lv_textarea_get_password_mode(t)) {
                ui_str_copy(shown, sizeof(shown), v);
            }
        }
        ui_unlock();
    }
    size_t o = 0;
    for (const char *s = shown; *s && o + 7 < sizeof(esc); s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            esc[o++] = '\\';
            esc[o++] = (char)ch;
        } else if (ch < 0x20) {
            o += (size_t)snprintf(esc + o, sizeof(esc) - o, "\\u%04x", ch);
        } else {
            esc[o++] = (char)ch;
        }
    }
    esc[o] = 0;
    printf("{\"typed\":%d,\"taps\":%d,\"bytes\":%u,\"screen\":\"%s\",\"text\":\"%s\"}\n", typed, taps, len,
           ui_screen_name(ui_nav_current()), esc);
    return 0;
}

static int cmd_keys(void)
{
    char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return dc_fail("no memory");
    }
    if (!ui_lock(LOCK_MS)) {
        heap_caps_free(buf);
        return dc_fail("lvgl busy");
    }
    ui_kb_dump(buf, 2048);
    ui_unlock();
    for (char *line = buf, *nl; *line; line = nl + 1) {
        nl = strchr(line, '\n');
        if (!nl) {
            printf("%s\n", line);
            break;
        }
        printf("%.*s\n", (int)(nl - line), line);
    }
    heap_caps_free(buf);
    return 0;
}

static int cmd_audit(void)
{
    size_t len = 4096;
    char *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return dc_fail("no memory");
    }
    if (!ui_lock(LOCK_MS)) {
        heap_caps_free(buf);
        return dc_fail("lvgl busy");
    }
    int bad = ui_audit_json(buf, len);
    ui_unlock();
    printf("audit %s violations=%d %s\n", ui_screen_name(ui_nav_current()), bad, buf);
    heap_caps_free(buf);
    return bad ? 1 : 0;
}

static int cmd_fit(void)
{
    if (!ui_lock(LOCK_MS)) {
        return dc_fail("lvgl busy");
    }
    int bad = ui_str_fit_check(print_line);
    ui_unlock();
    printf("fit overflows=%d\n", bad);
    return bad ? 1 : 0;
}

static int cmd_btn(int argc, char **argv)
{
    const char *b = argc > 2 ? argv[2] : "";
    const char *ev = argc > 3 ? argv[3] : "short";
    if (!strcmp(b, "boot")) {
        printf("boot %s\n", ui_boot_down() ? "talk" : "ignored");
        return 0;
    }
    if (!strcmp(b, "pwr")) {
        if (!strcmp(ev, "down")) {
            ui_pwr_down();
        } else if (!strcmp(ev, "up")) {
            ui_pwr_up();
        } else {
            printf("pwr %s\n", ui_pwr_short() ? "back" : "at home");
        }
        return ui_wait_pumped(PUMP_WAIT_MS) ? 0 : dc_fail("ui queue not drained");
    }
    return dc_fail("usage: ui btn boot | pwr [short|down|up]");
}

static int cmd_ui(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    const char *arg = argc > 2 ? argv[2] : "";
    int v;
    if (!strcmp(sub, "screens")) {
        for (int i = 0; i < UI_SCR__N; i++) {
            printf("%s%s\n", ui_screen_name((ui_screen_t)i), i == (int)ui_nav_current() ? " *" : "");
        }
        return 0;
    }
    if (!strcmp(sub, "goto")) {
        const char *names[UI_SCR__N];
        for (int i = 0; i < UI_SCR__N; i++) {
            names[i] = ui_screen_name((ui_screen_t)i);
        }
        if (!strcmp(arg, "rd")) {
            arg = "reader";
        }
        if ((v = lookup(arg, names, UI_SCR__N)) < 0) {
            return dc_fail("unknown screen '%s' (ui screens)", arg);
        }
        // optional screen argument: adjust 1 = brightness, langpick 1 = slot B, text <field>, onboard <step>
        char *sarg = argc > 3 ? join(argc, argv, 3) : NULL;
        int rc = post(UI_GOTO, v, sarg);
        if (rc == 0 && ui_nav_current() != (ui_screen_t)v) {
            return dc_fail("screen %s did not open (reader needs a translated bubble; wifinet / wificonn are "
                           "reached from wifi)", arg);
        }
        return rc;
    }
    if (!strcmp(sub, "kb")) {
        return cmd_kb(argc, argv);
    }
    if (!strcmp(sub, "keys")) {
        return cmd_keys();
    }
    if (!strcmp(sub, "wake")) {
        return post(UI_WAKE, 0, NULL);
    }
    if (!strcmp(sub, "off")) {
        return post(UI_SCREEN_OFF, 0, NULL);
    }
    if (!strcmp(sub, "sleep")) {
        printf("{\"level\":%d,\"screen\":\"%s\"}\n", ui_sleep_level(), ui_screen_name(ui_nav_current()));
        return 0;
    }
    if (!strcmp(sub, "state")) {
        if (!strcmp(arg, "rec")) {
            arg = "listening";
        }
        if ((v = lookup(arg, s_state_names, UI_ST__N)) < 0) {
            return dc_fail("states: idle listening latched cancel uploading thinking speaking answered error offline");
        }
        return post(UI_SET_STATE, v, NULL);
    }
    if (!strcmp(sub, "mood")) {
        if ((v = lookup(arg, s_mood_names, 5)) < 0) {
            return dc_fail("moods: neutral happy sad surprised thinking");
        }
        return post(UI_SET_MOOD, v, NULL);
    }
    if (!strcmp(sub, "expr")) {
        int e = -1;
        for (int i = 0; i < EXPR__N; i++) {
            if (!strcmp(arg, ui_orb_expr_name((orb_expr_t)i))) {
                e = i;
            }
        }
        if (e < 0) {
            return dc_fail("expressions: neutral wide look happy soft sleepy");
        }
        uint32_t ms = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;
        if (!ui_lock(LOCK_MS)) {
            return dc_fail("lvgl busy");
        }
        ui_orb_set_expr(scr_home_orb(), (orb_expr_t)e, ms);
        ui_unlock();
        return 0;
    }
    if (!strcmp(sub, "intro")) {
        // replays the A31 boot splash on the Home orb
        if (!ui_lock(LOCK_MS)) {
            return dc_fail("lvgl busy");
        }
        ui_orb_intro(scr_home_orb());
        ui_unlock();
        return 0;
    }
    if (!strcmp(sub, "user")) {
        // ui user [-a|-b] <text...>   ("-" alone: waveform placeholder)
        int first = 2, side = UI_SIDE_AUTO;
        if (!strcmp(arg, "-a") || !strcmp(arg, "-b")) {
            side = arg[1] == 'a' ? UI_SIDE_A : UI_SIDE_B;
            first = 3;
        }
        if (first < argc && !strcmp(argv[first], "-")) {
            return post(UI_USER_MSG, side, NULL);
        }
        char *s = join(argc, argv, first);
        if (!s) {
            return dc_fail("usage: ui user [-a|-b] <text...> | ui user -");
        }
        return post(UI_USER_MSG, side, s);
    }
    if (!strcmp(sub, "bot") || !strcmp(sub, "append")) {
        char *s = join(argc, argv, 2);
        if (!s) {
            return dc_fail("usage: ui %s <text...>", sub);
        }
        return post(!strcmp(sub, "bot") ? UI_BOT_MSG : UI_BOT_APPEND, 0, s);
    }
    if (!strcmp(sub, "wave")) {
        // synthetic speech-like envelope for the pending waveform bubble
        uint8_t *env = heap_caps_malloc(32, MALLOC_CAP_SPIRAM);
        if (!env) {
            return dc_fail("no memory");
        }
        for (int i = 0; i < 32; i++) {
            env[i] = (uint8_t)((i * 73 + 41) % 200 + 40);
        }
        return post(UI_USER_WAVE, 0, (char *)env);
    }
    if (!strcmp(sub, "clear")) {
        return post(UI_CLEAR, 0, NULL);
    }
    if (!strcmp(sub, "divider")) {
        return post(UI_DIVIDER, 0, join(argc, argv, 2));     // ui divider [15:10]
    }
    if (!strcmp(sub, "tree")) {
        return cmd_tree();
    }
    if (!strcmp(sub, "tap")) {
        return cmd_tap(argc, argv);
    }
    if (!strcmp(sub, "audit")) {
        return cmd_audit();
    }
    if (!strcmp(sub, "fit")) {
        return cmd_fit();
    }
    if (!strcmp(sub, "btn")) {
        return cmd_btn(argc, argv);
    }
    if (!strcmp(sub, "mode")) {
        return post(UI_SET_MODE, !strcmp(arg, "translator") || !strcmp(arg, "1"), NULL);
    }
    if (!strcmp(sub, "error")) {
        if ((v = lookup(arg, s_card_names, UI_CARD__N)) < 0) {
            return dc_fail("cards: generic nowifi badkey nocredit region model rate server tls mic nospeaker");
        }
        return post(UI_ERROR, v, join(argc, argv, 3));
    }
    if (!strcmp(sub, "fault")) {
        if ((v = lookup(arg, s_fault_names, UI_FAULT__N)) < 0) {
            return dc_fail("faults: none nowifi badkey nocredit lowcredit region model expiring mic");
        }
        return post(UI_SET_FAULT, v, NULL);
    }
    if (!strcmp(sub, "net")) {
        if ((v = lookup(arg, s_net_names, 4)) < 0) {
            return dc_fail("net: off connecting up failed");
        }
        return post(UI_SET_NET, v, NULL);
    }
    if (!strcmp(sub, "toast")) {
        char *s = join(argc, argv, 2);
        return s ? post(UI_TOAST, 0, s) : dc_fail("usage: ui toast <text...>");
    }
    if (!strcmp(sub, "upload")) {
        return post(UI_UPLOAD_PCT, atoi(arg), NULL);
    }
    if (!strcmp(sub, "retry")) {
        return post(UI_RETRY, atoi(arg), NULL);
    }
    if (!strcmp(sub, "lang")) {
        return post(UI_SET_LANG, !strcmp(arg, "en"), NULL);
    }
    if (!strcmp(sub, "langs")) {
        char *s = join(argc, argv, 2);
        return s ? post(UI_SET_LANGS, 0, s) : dc_fail("usage: ui langs ru|en");
    }
    if (!strcmp(sub, "speaker")) {
        return post(UI_SET_SPEAKER, atoi(arg), NULL);
    }
    if (!strcmp(sub, "textonly")) {
        return post(UI_SET_TEXT_ONLY, atoi(arg), NULL);
    }
    if (!strcmp(sub, "hints")) {
        return post(UI_SET_HINTS, atoi(arg), NULL);
    }
    if (!strcmp(sub, "face")) {
        return post(UI_SET_FACE, atoi(arg), NULL);
    }
    if (!strcmp(sub, "dir")) {
        return post(UI_SET_TR_DIR, atoi(arg), NULL);    // 0 auto, 1 A->B, 2 B->A
    }
    if (!strcmp(sub, "back")) {
        return post(UI_BACK, 0, NULL);
    }
    if (!strcmp(sub, "fps")) {
#if LV_USE_PERF_MONITOR
        // lv_sysmon in log mode: one "LVGL" log line per second, no overlay in the screenshots
        if (!ui_lock(LOCK_MS)) {
            return dc_fail("lvgl busy");
        }
        if (!strcmp(arg, "on")) {
            lv_sysmon_performance_resume(NULL);
        } else {
            lv_sysmon_performance_pause(NULL);
        }
        ui_unlock();
        return 0;
#else
        return dc_fail("built without LV_USE_PERF_MONITOR");
#endif
    }
    if (!strcmp(sub, "busy")) {
        // rim BUSY comet (A30); the scan / portal screens that use it come later
        if (!ui_lock(LOCK_MS)) {
            return dc_fail("lvgl busy");
        }
        ui_rim_busy(atoi(arg) != 0);
        ui_unlock();
        return 0;
    }
    return dc_fail("usage: ui screens | goto <name> [arg] | kb [-ok] <text> | keys | wake | off | sleep | "
                   "state <s> | mood <m> | expr <e> [ms] | intro | "
                   "user [-a|-b] <text> | bot <text> | append <text> | wave | clear | divider [hh:mm] | tree | "
                   "tap <name>|key:<k> [ms] | audit | fit | mode 0|1 | "
                   "error <card> [body] | fault <f> | net <n> | toast <text> | upload <pct> | retry <n> | "
                   "lang ru|en | langs a|b | speaker 0|1 | textonly 0|1 | hints <n> | face 0|1 | dir 0|1|2 | "
                   "back | busy 0|1 | fps on|off | "
                   "btn boot|pwr [short|down|up]");
}

void ui_console_register(void)
{
    devconsole_register("ui", "UI test hooks: ui screens | goto | kb | keys | state | mood | user | bot | tree | tap ...",
                        cmd_ui);
}
