#pragma once
#include <stdbool.h>

/* Called only by the existing system-status worker. No extra periodic task. */
void watch_power_tasks_poll(bool off, bool usb);
void watch_power_tasks_report(void);
