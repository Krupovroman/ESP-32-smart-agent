#include "display.h"
#include "board_pins.h"

#include <string.h>
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "st77916_init_new.inc"

static const char *TAG = "display";

#define LCD_PCLK_HZ         (80 * 1000 * 1000)
#define LCD_PROBE_PCLK_HZ   (3 * 1000 * 1000)
#define LCD_OPCODE_READ_CMD 0x0B
#define DRAW_BUF_LINES      40

static lv_display_t *s_disp;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static display_info_t s_info;
static uint16_t *s_shadow;
static volatile bool s_capture;

static void probe_panel_id(void)
{
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(BOARD_PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_PROBE_PCLK_HZ;
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io) != ESP_OK) {
        ESP_LOGW(TAG, "probe io failed");
        return;
    }
    int cmd = (LCD_OPCODE_READ_CMD << 24) | (0x04 << 8);
    esp_err_t err = esp_lcd_panel_io_rx_param(io, cmd, s_info.panel_id, sizeof(s_info.panel_id));
    esp_lcd_panel_io_del(io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read 0x04 failed: %s", esp_err_to_name(err));
        return;
    }
    const uint8_t *id = s_info.panel_id;
    if (id[0] == 0x00 && id[1] == 0x7F && id[2] == 0x7F && id[3] == 0x7F) {
        s_info.panel_batch = 1;
    } else if (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        s_info.panel_batch = 2;
    }
    ESP_LOGI(TAG, "panel id %02x %02x %02x %02x -> batch %d", id[0], id[1], id[2], id[3], s_info.panel_batch);
}

// Keep flushed areas on even coordinates; QSPI panels of this family misbehave otherwise.
static void rounder_cb(lv_area_t *area)
{
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

// Copies every flushed area into the shadow frame while a capture is running.
static void flush_start_cb(lv_event_t *e)
{
    if (!s_capture || !s_shadow) {
        return;
    }
    const lv_area_t *a = lv_event_get_param(e);
    lv_draw_buf_t *buf = lv_display_get_buf_active(s_disp);
    if (!a || !buf) {
        return;
    }
    int32_t w = lv_area_get_width(a);
    for (int32_t y = a->y1; y <= a->y2; y++) {
        if (y < 0 || y >= BOARD_LCD_V_RES || a->x1 < 0 || a->x2 >= BOARD_LCD_H_RES) {
            continue;
        }
        const uint8_t *src = buf->data + (size_t)(y - a->y1) * buf->header.stride;
        memcpy(&s_shadow[y * BOARD_LCD_H_RES + a->x1], src, (size_t)w * 2);
    }
}

esp_err_t display_init(void)
{
    const spi_bus_config_t bus_cfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        BOARD_PIN_LCD_SCK, BOARD_PIN_LCD_D0, BOARD_PIN_LCD_D1, BOARD_PIN_LCD_D2, BOARD_PIN_LCD_D3,
        BOARD_LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    probe_panel_id();

    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(BOARD_PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");
    s_info.pclk_hz = LCD_PCLK_HZ;

    st77916_vendor_config_t vendor_cfg = {
        .flags = { .use_qspi_interface = 1 },
    };
    if (s_info.panel_batch == 2) {
        vendor_cfg.init_cmds = st77916_init_new;
        vendor_cfg.init_cmds_size = sizeof(st77916_init_new) / sizeof(st77916_init_new[0]);
    }
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,   // reset is on the IO expander (pulsed in board_init)
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel), TAG, "st77916");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 8192;
    port_cfg.task_affinity = 1;
    port_cfg.task_max_sleep_ms = 100;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .rounder_cb = rounder_cb,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 1,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "add disp");

    s_shadow = heap_caps_calloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    lvgl_port_lock(0);
    lv_display_add_event_cb(s_disp, flush_start_cb, LV_EVENT_FLUSH_START, NULL);
    lvgl_port_unlock();
    return ESP_OK;
}

lv_display_t *display_get(void) { return s_disp; }
const display_info_t *display_info(void) { return &s_info; }

esp_err_t display_set_invert(bool on)
{
    lvgl_port_lock(0);
    esp_err_t err = esp_lcd_panel_invert_color(s_panel, on);
    lvgl_port_unlock();
    return err;
}

const uint16_t *display_capture(void)
{
    if (!s_shadow) {
        return NULL;
    }
    lvgl_port_lock(0);
    s_capture = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_disp);
    s_capture = false;
    lvgl_port_unlock();
    return s_shadow;
}
