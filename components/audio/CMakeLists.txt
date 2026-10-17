idf_component_register(
    SRCS "audio_in.c" "audio_out.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s board
)
