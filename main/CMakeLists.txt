idf_component_register(
    SRC_DIRS "." "ui"
    INCLUDE_DIRS "." "ui"
    PRIV_REQUIRES board display touch audio settings net orhttp devconsole ota ai
                  app_update esp_timer lvgl__lvgl espressif__esp_lvgl_port
)
