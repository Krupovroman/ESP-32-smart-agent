idf_component_register(
    SRCS "devconsole.c" "upload.c" "cmd_sys.c" "cmd_io.c" "cmd_audio.c" "cmd_ota.c" "cmd_net.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES console esp_driver_usb_serial_jtag esp_app_format app_update esp_timer esp_psram spi_flash
                  board display touch audio settings net orhttp ota
)
