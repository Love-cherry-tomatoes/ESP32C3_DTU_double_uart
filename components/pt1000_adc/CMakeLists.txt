idf_component_register(
    SRCS "pt1000_adc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc esp_driver_gpio nvs_flash dtu_config dtu_log blackbox
)
