idf_component_register(
    SRCS "wrist_gesture.c"
    INCLUDE_DIRS "include"
    REQUIRES
        qmi8658
        esp_driver_i2c
        esp_common
        watch_power
)
