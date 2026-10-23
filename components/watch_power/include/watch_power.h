#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define WATCH_POWER_WIFI_LISTEN_INTERVAL 10U
#ifdef __cplusplus
extern "C" {
#endif
/* Task-context API. Active screen/audio retains full performance and disables
 * auto sleep. Screen-off idle permits a 20MHz minimum and auto Light-sleep.
 * Wi-Fi PS follows audio/screen: NONE / MIN_MODEM / MAX_MODEM respectively. */
esp_err_t watch_power_init(void);
void watch_power_set_screen_on(bool on);
bool watch_power_is_screen_on(void);
typedef struct {
    bool on;
    int64_t since_us;
} watch_power_screen_state_t;
/* Coherent screen edge/time pair for deferred work on normal worker tasks. */
watch_power_screen_state_t watch_power_get_screen_state(void);
void watch_power_audio_begin(void);
void watch_power_audio_end(void);
/* Called by the existing status worker. Reports cached battery standby
 * measurements after USB reconnect without adding a task or periodic timer. */
void watch_power_poll(void);
#ifdef __cplusplus
}
#endif
