idf_component_register(
    SRCS "wifi_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_wifi esp_timer log esp_http_server event_log
)
