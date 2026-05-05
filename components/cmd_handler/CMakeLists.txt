idf_component_register(
    SRCS "cmd_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES json dtu_config dtu_log blackbox
)
