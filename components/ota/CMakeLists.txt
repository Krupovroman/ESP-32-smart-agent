idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_partition
    PRIV_REQUIRES esp_https_ota esp_http_client esp_app_format mbedtls nvs_flash
)
