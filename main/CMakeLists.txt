idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_netif esp_event
             dtu_log dtu_config blackbox wifi_mgr rs485_uart
             net_transparent web_server ble_service pt1000_adc
             cmd_handler ota_update
)
