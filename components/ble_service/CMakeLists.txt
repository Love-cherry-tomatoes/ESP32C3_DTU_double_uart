idf_component_register(
    SRCS "ble_service.c" "ble_gatt_table.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash dtu_config dtu_log blackbox cmd_handler
)
