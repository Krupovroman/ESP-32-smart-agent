idf_component_register(
    SRCS "httpx.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client
    PRIV_REQUIRES esp-tls mbedtls esp_timer
)
