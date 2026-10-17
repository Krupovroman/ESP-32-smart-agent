idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_ledc espressif__esp_io_expander_tca9554 espressif__esp_io_expander
)
