/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Physical keys of the ESP32-S3-Touch-AMOLED-2.06 board.
 *
 * BOOT is GPIO0 (strapping pin, active low, internal pull-up).
 * PWR  is GPIO10 (SYS_OUT through an inverter, active high, external pull-up,
 *      so internal pulls must stay disabled).
 *
 * The keys are scanned by one task instead of GPIO interrupts: Gravitysphere
 * still calls gpio_config() on GPIO0 when it opens, and an interrupt-based
 * driver would silently lose its configuration at that point.
 */
typedef enum {
    BOARD_KEY_BOOT = 0,
    BOARD_KEY_PWR,
    BOARD_KEY_MAX,
} board_key_t;

typedef enum {
    BOARD_KEY_SHORT_PRESS = 0,
    BOARD_KEY_LONG_PRESS,
} board_key_event_t;

typedef void (*board_key_callback_t)(board_key_t key, board_key_event_t event, void *user_data);

#define BOARD_KEYS_MAX_LISTENERS 4

/** Configure both keys and start the scanner task. Idempotent. */
esp_err_t board_keys_init(void);

/** Register a listener. Callbacks run in the key task, never in an ISR. */
esp_err_t board_keys_add_listener(board_key_callback_t callback, void *user_data);

/** Remove a previously registered listener. */
esp_err_t board_keys_remove_listener(board_key_callback_t callback, void *user_data);

/** Current debounced level of a key. */
bool board_keys_is_pressed(board_key_t key);

/** Enable or disable the short haptic pulse emitted on every recognised press. */
void board_keys_set_haptic_enabled(bool enabled);
bool board_keys_haptic_enabled(void);
/** Schedule touch feedback without blocking the LVGL task. */
void board_keys_haptic_request(uint32_t duration_ms);

/**
 * Fire one haptic pulse from any task.
 *
 * A coin motor needs roughly 30-50 ms to become perceptible, so callers should
 * stay within 20-60 ms. Values are clamped to 200 ms.
 */
void board_keys_haptic_pulse(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
