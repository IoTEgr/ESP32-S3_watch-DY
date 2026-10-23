#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Set Shanghai / UTC+8 before any clock UI or network client is created. */
void time_sync_init(void);
/* RTC/network work runs on the resident status task, never the LVGL task. */
void time_sync_poll(bool wifi_connected);
#ifdef __cplusplus
}
#endif
