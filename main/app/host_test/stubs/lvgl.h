#pragma once
// The few LVGL types and calls the app layer sees through ui.h.
#include <stdint.h>
typedef struct lv_obj_t lv_obj_t;
typedef struct lv_display_t lv_display_t;
typedef struct lv_draw_buf_t lv_draw_buf_t;
typedef struct lv_indev_t lv_indev_t;
typedef struct lv_font_t lv_font_t;
typedef enum { LV_INDEV_TYPE_NONE, LV_INDEV_TYPE_POINTER } lv_indev_type_t;
typedef enum { LV_INDEV_STATE_RELEASED, LV_INDEV_STATE_PRESSED } lv_indev_state_t;
lv_indev_t *lv_indev_active(void);
void lv_indev_wait_release(lv_indev_t *in);
lv_indev_t *lv_indev_get_next(lv_indev_t *in);
lv_indev_type_t lv_indev_get_type(const lv_indev_t *in);
lv_indev_state_t lv_indev_get_state(const lv_indev_t *in);
uint32_t lv_display_get_inactive_time(const lv_display_t *d);
