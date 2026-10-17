idf_component_register(
    SRCS "touch.c" "inject.c"
    INCLUDE_DIRS "."
    REQUIRES board esp_lcd espressif__esp_lvgl_port lvgl__lvgl
    PRIV_REQUIRES espressif__esp_lcd_touch_cst816s espressif__esp_lcd_touch esp_timer
)
