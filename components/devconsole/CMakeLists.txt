idf_component_register(
    SRCS "devconsole.c" "cmd_net.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES console esp_driver_usb_serial_jtag esp_app_format app_update esp_timer
                  board display touch audio settings net orhttp
)
