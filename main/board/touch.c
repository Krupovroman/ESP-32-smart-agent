#include "touch.h"
#include "board.h"
#include "board_pins.h"

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

static const char *TAG = "touch";

static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_indev;
static volatile bool s_irq;
static bool s_pressed;
static int16_t s_last_x, s_last_y;
static int64_t s_last_irq_us;
static bool s_log;

static void IRAM_ATTR touch_isr(esp_lcd_touch_handle_t tp)
{
    s_irq = true;
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int64_t now = esp_timer_get_time();
    bool irq = s_irq;
    s_irq = false;
    if (irq) {
        s_last_irq_us = now;
    }
    // Poll while the finger is down (INT keeps pulsing); stop ~60 ms after the last pulse.
    if (irq || (s_pressed && now - s_last_irq_us < 60000)) {
        esp_lcd_touch_point_data_t pt[1];
        uint8_t cnt = 0;
        if (esp_lcd_touch_read_data(s_tp) == ESP_OK &&
            esp_lcd_touch_get_data(s_tp, pt, &cnt, 1) == ESP_OK && cnt > 0) {
            s_pressed = true;
            s_last_x = pt[0].x;
            s_last_y = pt[0].y;
            if (s_log) {
                ESP_LOGI(TAG, "raw %d,%d", s_last_x, s_last_y);
            }
        } else {
            s_pressed = false;
        }
    } else {
        s_pressed = false;
    }
    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t touch_init(lv_display_t *disp)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(board_i2c1(), &io_cfg, &tp_io), TAG, "io");

    esp_lcd_touch_config_t cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,     // reset is on the IO expander (pulsed in board_init)
        .int_gpio_num = BOARD_PIN_TP_INT,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(tp_io, &cfg, &s_tp), TAG, "cst816s");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_register_interrupt_callback(s_tp, touch_isr), TAG, "isr");

    lvgl_port_lock(0);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, read_cb);
    lv_indev_set_display(s_indev, disp);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "CST816 ready");
    return ESP_OK;
}

void touch_set_log(bool on) { s_log = on; }
