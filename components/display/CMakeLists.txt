idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "."
    REQUIRES board esp_lcd esp_driver_spi espressif__esp_lvgl_port lvgl__lvgl
    PRIV_REQUIRES espressif__esp_lcd_st77916
)
