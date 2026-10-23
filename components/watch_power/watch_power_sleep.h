#pragma once
#include <stdbool.h>

/* Measurement only: leaves sleep entry decisions and return values intact. */
void watch_power_sleep_set_screen_on(bool on);
void watch_power_sleep_poll(void);
