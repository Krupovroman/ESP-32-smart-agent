// CJK fallback fonts over the storage asset pack (ui_font.h, docs/ui.md 1.3).
//
// The pack's TrueType font is memory-mapped (components/assets), so lv_tiny_ttf / stb_truetype read the outlines
// straight from the flash cache: no file system and no RAM copy of the 5 MB font. Each size has its own Tiny TTF
// instance with a bounded glyph cache; the rendered A8 bitmaps go to PSRAM through LVGL's font draw-buffer
// handlers, the cache nodes and stb's scratch to the LVGL pool (also PSRAM). Internal RAM is not touched.
//
// ui_cjk_16/20/28 are the objects the generated fonts name as .fallback. They forward only code points the
// pack's coverage bitmap marks, so a code point no font has (emoji, Arabic) never reaches Tiny TTF, which would
// log an error for each miss.
#include "ui_font.h"
#include "ui.h"
#include "ui_internal.h"
#include "ui_theme.h"
#include "assets.h"
#include "devconsole.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "src/draw/lv_draw_buf_private.h"   // font draw-buffer handlers (malloc / free)

static const char *TAG = "ui_font";

#define N_SIZES     3
#define COV_BYTES   8192            // one bit per BMP code point (tools/mkpack.py)
#define BENCH_MAX   64

static bool no_glyph(const lv_font_t *f, lv_font_glyph_dsc_t *d, uint32_t letter, uint32_t next)
{
    (void)f;
    (void)d;
    (void)letter;
    (void)next;
    return false;
}

lv_font_t ui_cjk_16 = { .get_glyph_dsc = no_glyph };
lv_font_t ui_cjk_20 = { .get_glyph_dsc = no_glyph };
lv_font_t ui_cjk_28 = { .get_glyph_dsc = no_glyph };

// Sizes match the Inter fonts they back; glyph-cache entries per size (each entry is one bitmap in PSRAM, about
// 0.3 / 0.45 / 0.85 KB, plus about 260 B of cache nodes in the LVGL pool). A screen shows at most about 170
// distinct glyphs at 16 px and 50 at 28 px, so a scroll never re-renders what is on screen.
static const struct {
    lv_font_t *proxy;
    int32_t px;
    uint32_t cache;
} s_size[N_SIZES] = {
    { &ui_cjk_16, 16, 224 },
    { &ui_cjk_20, 20, 224 },
    { &ui_cjk_28, 28, 160 },
};

typedef struct {
    uint32_t lookups;       // glyph descriptors asked for (code points the pack covers)
    uint32_t bitmaps;       // bitmaps asked for (every glyph drawn)
    uint32_t rendered;      // bitmaps rasterised by stb_truetype (glyph-cache misses)
    uint64_t render_us;     // time spent in those
    uint32_t render_max_us;
} font_stat_t;

static lv_font_t *s_ttf[N_SIZES];
static font_stat_t s_stat[N_SIZES];
static const uint8_t *s_cov;
static uint32_t s_glyphs, s_cps;
static volatile ui_cjk_state_t s_state = UI_CJK_ABSENT;
static bool s_in_render;
static uint32_t s_render_allocs;
static size_t s_buf_bytes, s_buf_peak;

// ---- Font draw buffers in PSRAM (glyph cache bitmaps and the label's per-glyph scratch)

static void *font_buf_malloc(size_t size, lv_color_format_t cf)
{
    (void)cf;
    // room for LVGL's alignment of data inside the buffer, as its default handler does
    void *p = heap_caps_malloc(size + LV_DRAW_BUF_ALIGN - 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        s_buf_bytes += heap_caps_get_allocated_size(p);
        s_buf_peak = LV_MAX(s_buf_peak, s_buf_bytes);
        if (s_in_render) {
            s_render_allocs++;
        }
    }
    return p;
}

static void font_buf_free(void *p)
{
    if (p) {
        s_buf_bytes -= heap_caps_get_allocated_size(p);
        heap_caps_free(p);
    }
}

// ---- Proxy callbacks (taskLVGL)

static int size_index(const lv_font_t *proxy)
{
    for (int i = 0; i < N_SIZES; i++) {
        if (s_size[i].proxy == proxy) {
            return i;
        }
    }
    return 0;
}

static bool covered(uint32_t cp)
{
    return cp < 0x10000 && s_cov && (s_cov[cp >> 3] & (1u << (cp & 7)));
}

static bool proxy_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t letter, uint32_t next)
{
    (void)next;     // no kerning between CJK glyphs
    if (!covered(letter)) {
        return false;
    }
    const lv_font_t *t = font->user_data;
    s_stat[size_index(font)].lookups++;
    return t->get_glyph_dsc(t, out, letter, 0);
}

static const void *proxy_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
    // g->resolved_font is the proxy: it shares dsc and line_height with its Tiny TTF font, which is all the
    // Tiny TTF bitmap and release callbacks read from it
    const lv_font_t *t = g->resolved_font->user_data;
    font_stat_t *st = &s_stat[size_index(g->resolved_font)];
    int64_t t0 = esp_timer_get_time();
    s_in_render = true;
    s_render_allocs = 0;
    const void *bm = t->get_glyph_bitmap(g, buf);
    s_in_render = false;
    st->bitmaps++;
    if (s_render_allocs) {
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        st->rendered++;
        st->render_us += us;
        st->render_max_us = LV_MAX(st->render_max_us, us);
    }
    return bm;
}

static void attach(void)
{
    assets_entry_t ttf, cov;
    if (!assets_find("cjk.ttf", &ttf) || !assets_find("cjk.cov", &cov) || cov.size != COV_BYTES) {
        ESP_LOGE(TAG, "pack has no cjk.ttf / cjk.cov");
        s_state = UI_CJK_BAD;
        return;
    }
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N_SIZES; i++) {
        s_ttf[i] = lv_tiny_ttf_create_data_ex(ttf.data, ttf.size, s_size[i].px, LV_FONT_KERNING_NONE,
                                              s_size[i].cache);
        if (!s_ttf[i]) {
            ESP_LOGE(TAG, "tiny_ttf %d px failed", (int)s_size[i].px);
            for (int k = 0; k < i; k++) {
                lv_tiny_ttf_destroy(s_ttf[k]);
                s_ttf[k] = NULL;
            }
            s_state = UI_CJK_BAD;
            return;
        }
    }
    s_cov = cov.data;
    s_glyphs = ttf.aux;
    s_cps = cov.aux;
    for (int i = 0; i < N_SIZES; i++) {
        lv_font_t *p = s_size[i].proxy;
        const lv_font_t *t = s_ttf[i];
        p->dsc = t->dsc;
        p->line_height = t->line_height;
        p->base_line = t->base_line;
        p->kerning = LV_FONT_KERNING_NONE;
        p->release_glyph = t->release_glyph;
        p->get_glyph_bitmap = proxy_bitmap;
        p->user_data = (void *)t;
        p->get_glyph_dsc = proxy_dsc;       // last: the fallback chain uses the proxy from here on
    }
    s_state = UI_CJK_READY;
    ESP_LOGI(TAG, "CJK fallback ready: %u glyphs, %u code points, %d/%d/%d px in %u us", (unsigned)s_glyphs,
             (unsigned)s_cps, (int)s_size[0].px, (int)s_size[1].px, (int)s_size[2].px,
             (unsigned)(esp_timer_get_time() - t0));
}

// One-shot task with a PSRAM stack (only its TCB is internal): it reads mapped flash and never writes it. The CRC of
// the 5 MB font (estimated 50-80 ms per MB through the flash cache) runs on core 0 beside app_main; the fonts are
// then attached under the
// LVGL lock. (ui_work would hold a 6 KB internal stack through the boot self-test.)
static void verify_task(void *arg)
{
    (void)arg;
    if (assets_verify() != ESP_OK) {
        s_state = UI_CJK_BAD;
    } else {
        while (!ui_lock(1000)) {
        }
        attach();
        ui_unlock();
    }
    vTaskDeleteWithCaps(NULL);
}

void ui_font_probe(void)
{
    assets_init();
    switch (assets_state()) {
    case ASSETS_UNCHECKED: s_state = UI_CJK_CHECKING; break;
    case ASSETS_OK: s_state = UI_CJK_CHECKING; break;     // attached by ui_font_init
    case ASSETS_BAD: s_state = UI_CJK_BAD; break;
    default: s_state = UI_CJK_ABSENT; break;
    }
}

void ui_font_init(void)
{
    lv_draw_buf_handlers_t *h = lv_draw_buf_get_font_handlers();
    h->buf_malloc_cb = font_buf_malloc;
    h->buf_free_cb = font_buf_free;
    if (s_state != UI_CJK_CHECKING) {
        return;
    }
    if (assets_state() == ASSETS_OK) {
        attach();
        return;
    }
    // priority 1 shares core 0 with app_main (also 1) instead of holding up the boot
    if (xTaskCreatePinnedToCoreWithCaps(verify_task, "cjk_crc", 4096, NULL, 1, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "no verify task");
        s_state = UI_CJK_BAD;
    }
}

ui_cjk_state_t ui_cjk_state(void)
{
    return s_state;
}

bool ui_cjk_offered(void)
{
    return s_state == UI_CJK_CHECKING || s_state == UI_CJK_READY;
}

// ---- Console

static const char *state_name(ui_cjk_state_t s)
{
    static const char *const names[] = { "absent", "checking", "ready", "bad" };
    return (unsigned)s < 4 ? names[s] : "?";
}

typedef struct {
    int n, offset;
    SemaphoreHandle_t done;
    uint32_t cps[BENCH_MAX];
    // per size: cold descriptor and bitmap times of n glyphs on a fresh Tiny TTF instance
    uint32_t dsc_us[N_SIZES], dsc_max[N_SIZES], bmp_us[N_SIZES], bmp_max[N_SIZES];
    // end to end: a 252 px ui_28 label with n other glyphs, from creation to the end of the refresh that drew it
    lv_obj_t *label;
    int64_t t0, t_layout;
    uint32_t label_ms, layout_ms, label_rendered, rendered0;
    uint32_t stack_free;
    bool ok;
} bench_t;

static bench_t *s_bench;

// n covered hanzi spread over the font (and the flash), different for each offset
static int pick_hanzi(uint32_t *out, int n, int offset)
{
    int total = 0;
    for (uint32_t c = 0x4E00; c <= 0x9FFF; c++) {
        total += covered(c);
    }
    if (!total) {
        return 0;
    }
    int got = 0;
    for (int i = 0; i < n; i++) {
        int want = (int)(((int64_t)offset * 997 + (int64_t)i * 61) % total), k = 0;
        for (uint32_t c = 0x4E00; c <= 0x9FFF; c++) {
            if (covered(c) && k++ == want) {
                out[got++] = c;
                break;
            }
        }
    }
    return got;
}

static void bench_ready_cb(lv_event_t *e)
{
    bench_t *b = lv_event_get_user_data(e);
    if (b != s_bench || !b->label) {
        return;
    }
    b->label_ms = (uint32_t)((esp_timer_get_time() - b->t0 + 500) / 1000);
    b->label_rendered = s_stat[2].rendered - b->rendered0;
    b->stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    lv_display_remove_event_cb_with_user_data(lv_event_get_target(e), bench_ready_cb, b);
    lv_obj_delete_async(b->label);
    b->label = NULL;
    b->ok = true;
    xSemaphoreGive(b->done);
}

static void bench_cb(void *arg)
{
    bench_t *b = arg;
    assets_entry_t ttf;
    if (s_state != UI_CJK_READY || !assets_find("cjk.ttf", &ttf)) {
        xSemaphoreGive(b->done);
        return;
    }
    int n = pick_hanzi(b->cps, b->n, b->offset);
    for (int i = 0; i < N_SIZES; i++) {
        lv_font_t *f = lv_tiny_ttf_create_data_ex(ttf.data, ttf.size, s_size[i].px, LV_FONT_KERNING_NONE,
                                                  (size_t)n + 4);
        if (!f) {
            continue;
        }
        for (int k = 0; k < n; k++) {
            lv_font_glyph_dsc_t g;
            int64_t t0 = esp_timer_get_time();
            bool ok = lv_font_get_glyph_dsc(f, &g, b->cps[k], 0);
            int64_t t1 = esp_timer_get_time();
            if (ok) {
                lv_font_get_glyph_bitmap(&g, NULL);
            }
            int64_t t2 = esp_timer_get_time();
            lv_font_glyph_release_draw_data(&g);
            b->dsc_us[i] += (uint32_t)(t1 - t0);
            b->dsc_max[i] = LV_MAX(b->dsc_max[i], (uint32_t)(t1 - t0));
            b->bmp_us[i] += (uint32_t)(t2 - t1);
            b->bmp_max[i] = LV_MAX(b->bmp_max[i], (uint32_t)(t2 - t1));
        }
        lv_tiny_ttf_destroy(f);
    }
    b->n = n;

    // end to end with the real fallback chain: other glyphs, so they are cold in the ui_28 cache as well
    uint32_t cps2[BENCH_MAX];
    int n2 = pick_hanzi(cps2, n, b->offset + 1);
    char *text = heap_caps_malloc((size_t)n2 * 3 + 1, MALLOC_CAP_SPIRAM);
    if (!text) {
        xSemaphoreGive(b->done);
        return;
    }
    size_t o = 0;
    for (int k = 0; k < n2; k++) {
        text[o++] = (char)(0xE0 | (cps2[k] >> 12));
        text[o++] = (char)(0x80 | ((cps2[k] >> 6) & 0x3F));
        text[o++] = (char)(0x80 | (cps2[k] & 0x3F));
    }
    text[o] = 0;
    b->rendered0 = s_stat[2].rendered;
    b->t0 = esp_timer_get_time();
    // the reader's text box (ui.md 3.6): x 54-306, y 76-260, ui_28 centred, on the top layer over every screen
    lv_obj_t *l = lv_label_create(lv_layer_top());
    lv_obj_add_style(l, &st_text28, 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 54, 76);
    lv_obj_set_width(l, 252);
    lv_label_set_text(l, text);
    heap_caps_free(text);
    lv_obj_update_layout(l);
    b->layout_ms = (uint32_t)((esp_timer_get_time() - b->t0 + 500) / 1000);
    b->label = l;
    lv_display_add_event_cb(lv_display_get_default(), bench_ready_cb, LV_EVENT_REFR_READY, b);
}

static int cmd_bench(int argc, char **argv)
{
    bench_t *b = heap_caps_calloc(1, sizeof(bench_t), MALLOC_CAP_SPIRAM);
    if (!b) {
        return dc_fail("no memory");
    }
    b->n = argc > 2 ? atoi(argv[2]) : 40;
    b->n = LV_CLAMP(1, b->n, BENCH_MAX);
    b->offset = argc > 3 ? atoi(argv[3]) : (int)(esp_timer_get_time() / 1000 % 1000);
    b->done = xSemaphoreCreateBinary();
    if (!b->done || !ui_lock(1000)) {
        if (b->done) {
            vSemaphoreDelete(b->done);
        }
        heap_caps_free(b);
        return dc_fail("lvgl busy");
    }
    s_bench = b;
    lv_async_call(bench_cb, b);
    ui_unlock();
    bool got = xSemaphoreTake(b->done, pdMS_TO_TICKS(5000)) == pdTRUE;
    if (ui_lock(1000)) {
        // never let taskLVGL touch the job later
        lv_async_call_cancel(bench_cb, b);
        lv_display_remove_event_cb_with_user_data(lv_display_get_default(), bench_ready_cb, b);
        if (b->label) {
            lv_obj_delete(b->label);
        }
        s_bench = NULL;
        ui_unlock();
    }
    int rc = 0;
    if (s_state != UI_CJK_READY) {
        rc = dc_fail("CJK fonts not ready (%s)", state_name(s_state));
    } else if (!got || !b->ok) {
        rc = dc_fail("bench did not finish");
    } else {
        char sizes[3][160];
        for (int i = 0; i < N_SIZES; i++) {
            snprintf(sizes[i], sizeof(sizes[i]),
                     "{\"px\":%d,\"dsc_us\":%u,\"dsc_max_us\":%u,\"bitmap_us\":%u,\"bitmap_max_us\":%u,"
                     "\"total_ms\":%u}",
                     (int)s_size[i].px, (unsigned)(b->dsc_us[i] / b->n), (unsigned)b->dsc_max[i],
                     (unsigned)(b->bmp_us[i] / b->n), (unsigned)b->bmp_max[i],
                     (unsigned)((b->dsc_us[i] + b->bmp_us[i] + 500) / 1000));
        }
        printf("{\"glyphs\":%d,\"offset\":%d,\"cold\":[%s,%s,%s],\"label28\":{\"glyphs\":%d,\"rendered\":%u,"
               "\"layout_ms\":%u,\"shown_ms\":%u},\"lvgl_stack_free\":%u}\n",
               b->n, b->offset, sizes[0], sizes[1], sizes[2], b->n, (unsigned)b->label_rendered,
               (unsigned)b->layout_ms, (unsigned)b->label_ms, (unsigned)b->stack_free);
    }
    vSemaphoreDelete(b->done);
    heap_caps_free(b);
    return rc;
}

static int cmd_status(void)
{
    font_stat_t st[N_SIZES];
    size_t buf, peak;
    if (!ui_lock(1000)) {
        return dc_fail("lvgl busy");
    }
    memcpy(st, s_stat, sizeof(st));
    buf = s_buf_bytes;
    peak = s_buf_peak;
    ui_unlock();
    char sizes[3][192];
    for (int i = 0; i < N_SIZES; i++) {
        snprintf(sizes[i], sizeof(sizes[i]),
                 "{\"px\":%d,\"cache\":%u,\"lookups\":%u,\"bitmaps\":%u,\"rendered\":%u,\"hits\":%u,"
                 "\"render_avg_us\":%u,\"render_max_us\":%u}",
                 (int)s_size[i].px, (unsigned)s_size[i].cache, (unsigned)st[i].lookups, (unsigned)st[i].bitmaps,
                 (unsigned)st[i].rendered, (unsigned)(st[i].bitmaps - st[i].rendered),
                 (unsigned)(st[i].rendered ? st[i].render_us / st[i].rendered : 0), (unsigned)st[i].render_max_us);
    }
    printf("{\"cjk\":\"%s\",\"pack\":\"%s\",\"label\":\"%s\",\"bytes\":%u,\"verify_ms\":%u,\"glyphs\":%u,\"cps\":%u,"
           "\"sizes\":[%s,%s,%s],\"font_buf_bytes\":%u,\"font_buf_peak\":%u}\n",
           state_name(s_state), assets_state_name(assets_state()), assets_label(), (unsigned)assets_size(),
           (unsigned)assets_verify_ms(), (unsigned)s_glyphs, (unsigned)s_cps, sizes[0], sizes[1], sizes[2],
           (unsigned)buf, (unsigned)peak);
    return 0;
}

static int cmd_font(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "status")) {
        return cmd_status();
    }
    if (!strcmp(sub, "bench")) {
        return cmd_bench(argc, argv);
    }
    return dc_fail("usage: font [status] | font bench [glyphs 1-64] [offset]");
}

void ui_font_console_register(void)
{
    devconsole_register("font", "CJK font pack: font [status] | font bench [n] [offset]", cmd_font);
}
