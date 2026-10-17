idf_component_register(
    SRC_DIRS "." "ui"
    INCLUDE_DIRS "." "ui"
    PRIV_REQUIRES board display touch audio settings net orhttp devconsole
                  app_update lvgl__lvgl espressif__esp_lvgl_port
)
