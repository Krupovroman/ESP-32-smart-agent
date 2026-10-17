#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

// CST816 on I2C1 as an LVGL pointer device. Reads only while the chip signals a touch
// (the CST816 does not answer I2C while asleep), and treats read errors as "released".
esp_err_t touch_init(lv_display_t *disp);

// When enabled, every raw touch point is logged (for orientation checks).
void touch_set_log(bool on);
