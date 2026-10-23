idf_component_register(SRCS "watch_debug.c" INCLUDE_DIRS "include" REQUIRES log)
if(NOT WATCH_DEBUG)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=_write_r" "-Wl,--wrap=printf" "-Wl,--wrap=vprintf"
        "-Wl,--wrap=puts" "-Wl,--wrap=putchar"
        "-Wl,--wrap=esp_rom_printf" "-Wl,--wrap=ets_printf")
endif()
