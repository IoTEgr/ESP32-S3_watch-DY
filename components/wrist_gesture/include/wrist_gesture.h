/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wrist-orientation monitor built on the board's QMI8658 accelerometer.
 *
 * The screen is considered "up" when the panel normal points against gravity.
 * Rotation is debounced over several samples so a swinging arm does not flicker
 * the backlight.
 */
typedef void (*wrist_listener_t)(bool screen_up, void *user_data);

#define WRIST_MAX_LISTENERS 4

esp_err_t wrist_gesture_init(i2c_master_bus_handle_t bus);

esp_err_t wrist_gesture_add_listener(wrist_listener_t callback, void *user_data);
esp_err_t wrist_gesture_remove_listener(wrist_listener_t callback, void *user_data);

bool wrist_gesture_is_screen_up(void);

/** Pause orientation handling (for example while another app owns the IMU). */
void wrist_gesture_set_enabled(bool enabled);

/** Shared accelerometer access in m/s²; serializes sensor mode changes. */
esp_err_t wrist_gesture_read_accel(float *x, float *y, float *z);
/** Request the foreground motion application's faster sample rate. */
esp_err_t wrist_gesture_set_app_active(bool active);

#ifdef __cplusplus
}
#endif
