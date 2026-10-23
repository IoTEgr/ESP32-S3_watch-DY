set(watch_power_sources "watch_power.c")
if(WATCH_DEBUG)
    list(APPEND watch_power_sources "watch_power_sleep.c" "watch_power_tasks.c" "watch_power_gate.c")
endif()
idf_component_register(SRCS ${watch_power_sources} INCLUDE_DIRS "include"
                       REQUIRES esp_pm freertos esp_common esp_timer esp_driver_usb_serial_jtag esp_hw_support esp_wifi
                       PRIV_REQUIRES heap)
if(WATCH_DEBUG)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_light_sleep_start"
                    "-Wl,--wrap=vApplicationSleep"
                    "-Wl,--wrap=esp_wifi_internal_is_tsf_active")
endif()
