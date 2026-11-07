idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_timer
        esp_http_server
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
)