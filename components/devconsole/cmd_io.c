// Display and touch commands: shot, tap, press, move, release, swipe, bl, invert, touchlog.
#include "dc_internal.h"
#include "board.h"
#include "display.h"
#include "inject.h"
#include "touch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCREEN_W 360
#define SCREEN_H 360

// shot [x y w h]: flush-shadow capture, RGB565 little-endian, rows top to bottom, stride w*2.
static int cmd_shot(int argc, char **argv)
{
    int x = 0, y = 0, w = SCREEN_W, h = SCREEN_H;
    if (argc == 5) {
        x = atoi(argv[1]);
        y = atoi(argv[2]);
        w = atoi(argv[3]);
        h = atoi(argv[4]);
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SCREEN_W || y + h > SCREEN_H) {
            return dc_fail("region outside the 360x360 screen");
        }
    } else if (argc != 1) {
        return dc_fail("usage: shot [x y w h]");
    }
    const uint16_t *fb = display_capture();
    if (!fb) {
        return dc_fail("no shadow buffer");
    }
    char meta[96];
    snprintf(meta, sizeof(meta), "\"w\":%d,\"h\":%d,\"x\":%d,\"y\":%d,\"fmt\":\"rgb565le\"", w, h, x, y);
    if (x == 0 && y == 0 && w == SCREEN_W && h == SCREEN_H) {
        return dc_blob("shot", fb, SCREEN_W * SCREEN_H * 2, meta);
    }
    uint16_t *crop = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (!crop) {
        return dc_fail("no memory");
    }
    for (int r = 0; r < h; r++) {
        memcpy(crop + (size_t)r * w, fb + (size_t)(y + r) * SCREEN_W + x, (size_t)w * 2);
    }
    int rc = dc_blob("shot", crop, (size_t)w * h * 2, meta);
    free(crop);
    return rc;
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
        return dc_fail("usage: tap x y [hold_ms]");
    }
    int hold = argc > 3 ? atoi(argv[3]) : 120;
    inject_set(x, y, true);
    vTaskDelay(pdMS_TO_TICKS(hold));
    inject_set(x, y, false);
    vTaskDelay(pdMS_TO_TICKS(60));
    return 0;
}

static int cmd_press(int argc, char **argv)
{
    int x, y;
    if (!parse_xy(argc, argv, 1, &x, &y)) {
        return dc_fail("usage: press x y");
    }
    inject_set(x, y, true);
    return 0;
}

static int cmd_release(int argc, char **argv)
{
    int x = argc > 2 ? atoi(argv[1]) : 0, y = argc > 2 ? atoi(argv[2]) : 0;
    inject_set(x, y, false);
    return 0;
}

static int cmd_swipe(int argc, char **argv)
{
    int x1, y1, x2, y2;
    if (!parse_xy(argc, argv, 1, &x1, &y1) || !parse_xy(argc, argv, 3, &x2, &y2)) {
        return dc_fail("usage: swipe x1 y1 x2 y2 [ms]");
    }
    int ms = argc > 5 ? atoi(argv[5]) : 250;
    int steps = ms / 16 > 2 ? ms / 16 : 2;
    for (int i = 0; i <= steps; i++) {
        inject_set(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, true);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    inject_set(x2, y2, false);
    vTaskDelay(pdMS_TO_TICKS(60));
    return 0;
}

static int cmd_bl(int argc, char **argv)
{
    if (argc < 2) {
        return dc_fail("usage: bl 0-100");
    }
    board_backlight_set((uint8_t)atoi(argv[1]));
    printf("{\"bl\":%u}\n", board_backlight_get());
    return 0;
}

static int cmd_invert(int argc, char **argv)
{
    if (argc < 2) {
        return dc_fail("usage: invert 0|1");
    }
    return display_set_invert(atoi(argv[1]) != 0) == ESP_OK ? 0 : dc_fail("panel");
}

static int cmd_touchlog(int argc, char **argv)
{
    touch_set_log(argc > 1 && atoi(argv[1]) != 0);
    return 0;
}

void cmd_io_register(void)
{
    devconsole_register("shot", "shot [x y w h] - screenshot (RGB565LE blob)", cmd_shot);
    devconsole_register("tap", "tap x y [hold_ms] - inject a touch", cmd_tap);
    devconsole_register("press", "press x y - hold an injected touch", cmd_press);
    devconsole_register("move", "move x y - move the held injected touch", cmd_press);
    devconsole_register("release", "release - end the injected touch", cmd_release);
    devconsole_register("swipe", "swipe x1 y1 x2 y2 [ms]", cmd_swipe);
    devconsole_register("bl", "bl 0-100 - backlight", cmd_bl);
    devconsole_register("invert", "invert 0|1 - panel colour inversion", cmd_invert);
    devconsole_register("touchlog", "touchlog 0|1 - log raw touch points", cmd_touchlog);
}
