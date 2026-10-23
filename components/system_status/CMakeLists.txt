idf_component_register(
    SRCS "system_status.cpp" "time_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES
        brookesia_core
        waveshare__esp32_s3_touch_amoled_2_06
        esp_driver_i2c
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
        cube32esp__xpowerslib
        lwip
        esp_timer
        pcf85063a
        watch_power
        weather_service
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
