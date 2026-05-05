idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json dtu_config wifi_mgr dtu_log blackbox net_transparent
)
