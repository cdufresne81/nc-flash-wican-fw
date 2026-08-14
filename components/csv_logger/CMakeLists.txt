idf_component_register(
    SRCS "csv_logger.c" "csv_bringup_logic.c"
    INCLUDE_DIRS "."
    REQUIRES main esp_timer json esp_http_server event_log
)
