#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    uint8_t panel_id[4];   // ST77916 register 0x04
    int panel_batch;       // 1 = driver default init, 2 = "new" vendor init, 0 = unknown id
    uint32_t pclk_hz;
} display_info_t;

// Brings up the QSPI panel and the LVGL port (LVGL task runs on core 1).
// The backlight stays off; turn it on once the first screen is built.
esp_err_t display_init(void);

lv_display_t *display_get(void);
const display_info_t *display_info(void);

// Must not be called with the LVGL lock held.
esp_err_t display_set_invert(bool on);

// Re-renders the whole screen (in the LVGL task) and returns a 360x360 RGB565 copy (PSRAM,
// native byte order), or NULL after a 3 s timeout. It is
// exactly what is sent to the panel, including top and system layers.
// Must not be called with the LVGL lock held. The buffer is reused by the next call.
const uint16_t *display_capture(void);
