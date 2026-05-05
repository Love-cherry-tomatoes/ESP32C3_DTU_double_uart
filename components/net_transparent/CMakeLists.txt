idf_component_register(
    SRCS "net_mqtt.c" "net_http.c" "net_udp.c" "net_transparent.c" "net_tcp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip esp_http_client mqtt esp-tls dtu_config dtu_log blackbox cmd_handler rs485_uart
)
