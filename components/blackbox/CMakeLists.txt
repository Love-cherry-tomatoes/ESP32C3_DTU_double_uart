idf_component_register(
    SRCS "blackbox.c"
    INCLUDE_DIRS "include"
    REQUIRES spi_flash nvs_flash
)
