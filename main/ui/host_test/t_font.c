// Host test of the CJK fallback (docs/ui.md 1.3): real LVGL 9.5, the generated Inter fonts, main/ui/ui_font.c,
// main/ui/ui_str.c and components/assets/assets.c over the storage pack the build makes. It checks the pack states,
// the sanitiser, the ui_28 rule and that hanzi really reach the frame buffer, and writes 360x360 PNG screenshots
// (2x) of mixed text, translator bubbles and the reader.
//   t_font.exe <storage.bin | none> <outdir> [corrupt]
#include <windows.h>
#include <stdarg.h>
#include "esp_stubs.h"
#include "lvgl.h"
#include "ui_font.h"
#include "ui_str.h"
#include "ui_theme.h"
#include "assets.h"
#include "devconsole.h"

static int s_checks, s_fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        s_checks++;                                                      \
        if (!(c)) {                                                      \
            s_fails++;                                                   \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
        }                                                                \
    } while (0)

// ---- ESP-IDF stand-ins (esp_stubs.h)

const char *esp_err_to_name(esp_err_t e)
{
    static char b[16];
    snprintf(b, sizeof(b), "err 0x%x", e);
    return b;
}

int64_t esp_timer_get_time(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *h, BaseType_t core, uint32_t caps)
{
    (void)name, (void)stack, (void)prio, (void)h, (void)core, (void)caps;
    fn(arg);        // the CRC task runs to the end at once
    return pdPASS;
}

void vTaskDeleteWithCaps(TaskHandle_t h)
{
    (void)h;
}

typedef struct {
    volatile int given;
} sem_t;

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return calloc(1, sizeof(sem_t));
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    ((sem_t *)s)->given = 1;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    free(s);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, uint32_t ms)
{
    // one thread: run LVGL (the "taskLVGL" the console waits for) until the semaphore is given
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (!((sem_t *)s)->given && esp_timer_get_time() < end) {
        lv_timer_handler();
        Sleep(1);
    }
    int g = ((sem_t *)s)->given;
    ((sem_t *)s)->given = 0;
    return g ? pdTRUE : pdFALSE;
}

static uint8_t *s_flash;
static esp_partition_t s_part = { 0x820000, 0x7C0000, "storage" };

const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, int st, const char *label)
{
    (void)t, (void)st;
    return strcmp(label, "storage") ? NULL : &s_part;
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n)
{
    if (off + n > p->size) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(dst, s_flash + off, n);
    return ESP_OK;
}

esp_err_t esp_partition_mmap(const esp_partition_t *p, size_t off, size_t n, esp_partition_mmap_memory_t m,
                             const void **out, esp_partition_mmap_handle_t *h)
{
    (void)m, (void)h;
    if (off + n > p->size) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_flash + off;
    return ESP_OK;
}

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    static uint32_t tab[256];
    if (!tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            tab[i] = c;
        }
    }
    crc = ~crc;
    while (len--) {
        crc = tab[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

bool ui_lock(uint32_t ms)
{
    (void)ms;
    return true;
}

void ui_unlock(void)
{
}

static dc_cmd_fn s_font_cmd;

void devconsole_register(const char *name, const char *help, dc_cmd_fn fn)
{
    (void)help;
    if (!strcmp(name, "font")) {
        s_font_cmd = fn;
    }
}

int dc_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  ERR ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    return 1;
}

lv_style_t st_text28;      // ui_theme.c is not built here; the bench label needs this one

// ---- Display: full frame, flushed into s_fb

#define W 360
#define H 360
static uint16_t s_fb[W * H], s_draw[W * H];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *p = (const uint16_t *)px;
    for (int32_t y = a->y1; y <= a->y2; y++) {
        for (int32_t x = a->x1; x <= a->x2; x++) {
            s_fb[y * W + x] = *p++;
        }
    }
    lv_display_flush_ready(d);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

// PNG, 2x, RGB8, deflate "stored" blocks (no compressor needed)
static void write_png(const char *path)
{
    const int s = 2, w = W * s, h = H * s;
    size_t row = 1 + (size_t)w * 3, raw_len = row * h;
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            uint16_t c = s_fb[(y / s) * W + x / s];
            r[1 + 3 * x] = (uint8_t)((c >> 11) << 3);
            r[2 + 3 * x] = (uint8_t)(((c >> 5) & 0x3F) << 2);
            r[3 + 3 * x] = (uint8_t)((c & 0x1F) << 3);
        }
    }
    size_t nblk = (raw_len + 65534) / 65535, zlen = 2 + nblk * 5 + raw_len + 4;
    uint8_t *z = malloc(4 + zlen), *o = z + 4;
    memcpy(z, "IDAT", 4);
    *o++ = 0x78;
    *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        *o++ = (uint8_t)(i + n == raw_len);
        *o++ = (uint8_t)n;
        *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n;
        *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + i, n);
        o += n;
    }
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    *o++ = (uint8_t)(adler >> 24);
    *o++ = (uint8_t)(adler >> 16);
    *o++ = (uint8_t)(adler >> 8);
    *o++ = (uint8_t)adler;
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        uint8_t ihdr[17] = { 'I', 'H', 'D', 'R' };
        ihdr[4] = (uint8_t)(w >> 24), ihdr[5] = (uint8_t)(w >> 16), ihdr[6] = (uint8_t)(w >> 8), ihdr[7] = (uint8_t)w;
        ihdr[8] = (uint8_t)(h >> 24), ihdr[9] = (uint8_t)(h >> 16), ihdr[10] = (uint8_t)(h >> 8), ihdr[11] = (uint8_t)h;
        ihdr[12] = 8, ihdr[13] = 2;
        fwrite(sig, 1, 8, f);
        put32(f, 13);
        fwrite(ihdr, 1, 17, f);
        put32(f, esp_rom_crc32_le(0, ihdr, 17));
        put32(f, (uint32_t)zlen);
        fwrite(z, 1, 4 + zlen, f);
        put32(f, esp_rom_crc32_le(0, z, (uint32_t)(4 + zlen)));
        put32(f, 0);
        fwrite("IEND", 1, 4, f);
        put32(f, esp_rom_crc32_le(0, (const uint8_t *)"IEND", 4));
        fclose(f);
    }
    free(z);
    free(raw);
}

// ---- Scenes

static const char *s_out;

static lv_obj_t *new_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s);
    return s;
}

// a label as the chat makes it: the text goes through the sanitiser first (ui_chat.c cm_set)
static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t w, const char *s)
{
    char clean[512];
    ui_str_sanitize(s, clean, sizeof(clean), font);
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, w);
    lv_label_set_text(l, clean);
    return l;
}

static lv_obj_t *column(lv_obj_t *scr, int32_t x, int32_t y, int32_t w, bool bubble)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(c, bubble ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_COL_SURFACE1), 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_hor(c, bubble ? 12 : 0, 0);
    lv_obj_set_style_pad_ver(c, bubble ? 10 : 0, 0);
    lv_obj_set_style_pad_row(c, bubble ? 2 : 8, 0);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static double shot(const char *name)
{
    int64_t t0 = esp_timer_get_time();
    lv_refr_now(NULL);
    double ms = (double)(esp_timer_get_time() - t0) / 1000.0;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\%s.png", s_out, name);
    write_png(p);
    printf("  %s.png: frame %.2f ms on this PC\n", name, ms);
    return ms;
}

// bright pixels inside an object's box: did its glyphs reach the frame?
static int lit(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int n = 0;
    for (int32_t y = LV_MAX(a.y1, 0); y <= LV_MIN(a.y2, H - 1); y++) {
        for (int32_t x = LV_MAX(a.x1, 0); x <= LV_MIN(a.x2, W - 1); x++) {
            n += (s_fb[y * W + x] >> 11) > 16;
        }
    }
    return n;
}

static void print_line(const char *line)
{
    printf("  %s\n", line);
}

static void font_cmd(int argc, char **argv)
{
    printf("  >");
    for (int i = 0; i < argc; i++) {
        printf(" %s", argv[i]);
    }
    printf("\n");
    int rc = s_font_cmd ? s_font_cmd(argc, argv) : -1;
    printf("  rc=%d\n", rc);
}

static void scenes(bool cjk)
{
    lv_obj_t *s = new_screen();
    lv_obj_t *c = column(s, 30, 40, 300, false);
    lv_obj_t *zh = text(c, &ui_20, UI_COL_TEXT1, 300, "你好，世界！こんにちは 안녕하세요");
    text(c, &ui_16, UI_COL_TEXT2, 300, "Привет, мир! Hello 你好 — 日本語のテキスト, 한국어 텍스트.");
    lv_obj_t *big = text(c, &ui_28, UI_COL_TEXT1, 300, "谢谢你的帮助 Ag");
    lv_obj_t *lat = text(c, &ui_20, UI_COL_TEXT1, 300, "Inter only: Съешь ещё этих мягких булок");
    text(c, &ui_16, UI_COL_TEXT1, 300, "拼音 nǐ hǎo · ①②③ · 「引用」 ・ ＡＢＣ　１２３");
    shot(cjk ? "mixed" : "mixed_nopack");
    CHECK(lit(lat) > 300);
    CHECK(cjk == (lit(zh) > 300));
    CHECK(!strcmp(lv_label_get_text(big), cjk ? "谢谢你的帮助 Ag" : "Ag"));

    // translator bubbles (3.4): 236 px, 12 / 10 padding; line 1 / 2 ui_16, line 3 ui_28 or ui_20
    s = new_screen();
    c = column(s, 62, 70, 236, true);
    text(c, &ui_16, UI_COL_TEXT2, 209, "RU \xE2\x86\x92 ZH");
    text(c, &ui_16, UI_COL_TEXT2, 209, "Где находится ближайшая аптека?");
    text(c, &ui_28, UI_COL_TEXT1, 209, "请问，最近的药店在哪里？");
    c = column(s, 62, 230, 236, true);
    text(c, &ui_16, UI_COL_TEXT2, 209, "ZH \xE2\x86\x92 RU");
    text(c, &ui_16, UI_COL_TEXT2, 209, "在前面的路口右转，然后一直走大概两百米就到了。");
    text(c, &ui_20, UI_COL_TEXT1, 209, "На перекрёстке направо и прямо метров двести.");
    shot(cjk ? "bubbles" : "bubbles_nopack");

    // reader (3.6): 252 px wide, ui_28 centred, 40 characters
    s = new_screen();
    lv_obj_t *r = text(s, &ui_28, UI_COL_TEXT1, 252,
                       "今天天气很好，我们一起去公园散步吧。那里有很多花和树，还有一个漂亮的湖。风景真好");
    lv_obj_set_pos(r, 54, 76);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    shot(cjk ? "reader" : "reader_nopack");
    if (cjk) {
        lv_obj_update_layout(r);
        CHECK(lv_obj_get_height(r) == 5 * 36);     // 9 hanzi per 252 px line at 28 px: 5 lines of 36 px
    }
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) {
        printf("usage: t_font <storage.bin|none> <outdir> [corrupt]\n");
        return 2;
    }
    s_out = argv[2];
    bool corrupt = argc > 3 && !strcmp(argv[3], "corrupt");
    s_flash = malloc(s_part.size);
    memset(s_flash, 0xFF, s_part.size);
    bool pack = strcmp(argv[1], "none") != 0;
    if (pack) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) {
            printf("no %s\n", argv[1]);
            return 3;
        }
        size_t n = fread(s_flash, 1, s_part.size, f);
        fclose(f);
        if (corrupt) {
            s_flash[n / 2] ^= 0x5A;     // one byte in the font: the CRC check must refuse the pack
        }
    }
    printf("t_font: %s%s\n", pack ? argv[1] : "no pack", corrupt ? " (corrupted)" : "");

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_style_init(&st_text28);
    lv_style_set_text_font(&st_text28, &ui_28);
    lv_style_set_text_color(&st_text28, lv_color_hex(UI_COL_TEXT1));

    ui_font_probe();
    ui_font_init();
    ui_font_console_register();
    bool cjk = pack && !corrupt;
    CHECK(ui_cjk_state() == (cjk ? UI_CJK_READY : pack ? UI_CJK_BAD : UI_CJK_ABSENT));
    CHECK(ui_cjk_offered() == cjk);
    CHECK(assets_state() == (cjk ? ASSETS_OK : pack ? ASSETS_BAD : ASSETS_ABSENT));

    // the sanitiser keeps what the chain can draw: markdown, emoji and Arabic go, CJK stays with the pack
    char out[256];
    ui_str_sanitize("你好🙂，世界！**粗体** مرحبا 안녕 Привет", out, sizeof(out), &ui_20);
    printf("  sanitised: <%s>\n", out);
    CHECK(!strcmp(out, cjk ? "你好，世界！粗体 안녕 Привет" : "Привет"));
    CHECK(ui_str_cols("谢谢你 abc") == 10);
    CHECK(ui_str_cols("Привет") == 6);
    CHECK(ui_str_font_has_all("最近的药店在哪里？", &ui_28) == cjk);
    CHECK(ui_str_font_has_all("Где аптека?", &ui_28));
    // the "ui fit" test of the board: every RU / EN string in its slot (new strings included)
    CHECK(ui_str_fit_check(print_line) == 0);

    scenes(cjk);

    char *st[] = { "font", "status" };
    font_cmd(2, st);
    if (cjk) {
        char *bench[] = { "font", "bench", "40", "7" };
        font_cmd(4, bench);
        font_cmd(2, st);
    }
    printf("\n%d checks, %d failures\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
