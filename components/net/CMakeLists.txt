idf_component_register(
    SRCS "net.c" "portal.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event settings
    PRIV_REQUIRES esp_http_server dns_server lwip
    EMBED_TXTFILES "portal.html"
)
