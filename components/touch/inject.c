#include "inject.h"
#include "esp_lvgl_port.h"

static lv_indev_t *s_indev;
static volatile int16_t s_x, s_y;
static volatile bool s_pressed;

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = s_x;
    data->point.y = s_y;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void inject_init(lv_display_t *disp)
{
    lvgl_port_lock(0);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, read_cb);
    lv_indev_set_display(s_indev, disp);
    lvgl_port_unlock();
}

void inject_set(int x, int y, bool pressed)
{
    s_x = (int16_t)x;
    s_y = (int16_t)y;
    s_pressed = pressed;
}
