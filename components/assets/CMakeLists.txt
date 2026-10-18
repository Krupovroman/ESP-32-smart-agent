idf_component_register(
    SRCS "assets.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition
    PRIV_REQUIRES esp_rom esp_timer
)
