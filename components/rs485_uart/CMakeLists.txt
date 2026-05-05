idf_component_register(
    SRCS "rs485_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_gpio dtu_config dtu_log blackbox
)
