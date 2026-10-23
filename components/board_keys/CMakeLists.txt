idf_component_register(
    SRCS "board_keys.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        esp_timer
        esp_common
        esp_hw_support
)
