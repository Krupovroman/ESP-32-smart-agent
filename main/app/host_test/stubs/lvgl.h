#pragma once
// The few LVGL types and calls the app layer sees through ui.h.
#include <stdint.h>
typedef struct lv_obj_t lv_obj_t;
typedef struct lv_display_t lv_display_t;
typedef struct lv_draw_buf_t lv_draw_buf_t;
typedef struct lv_indev_t lv_indev_t;
typedef struct lv_font_t lv_font_t;
lv_indev_t *lv_indev_active(void);
void lv_indev_wait_release(lv_indev_t *in);
uint32_t lv_display_get_inactive_time(const lv_display_t *d);
