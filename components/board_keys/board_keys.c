/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board_keys.h"

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "BoardKeys";

#define BOOT_GPIO           GPIO_NUM_0
#define PWR_GPIO            GPIO_NUM_10
#define HAPTIC_GPIO         GPIO_NUM_18

#define KEY_SCAN_PERIOD_MS  10
// Three identical samples at 10 ms keep a 30 ms guard against contact bounce.
#define KEY_DEBOUNCE_SAMPLES 3
// A press shorter than this is treated as bounce, not as a tap.
#define KEY_SHORT_MIN_MS    40
// PWR retains its existing short-press limit and long-press timing.
#define KEY_SHORT_MAX_MS    900
#define KEY_LONG_MS         1400
#define BOOT_LONG_MS        900
// A coin motor needs tens of milliseconds to spin up; 12 ms was inaudible and
// imperceptible on the real board.
#define HAPTIC_PULSE_MS     40
#define HAPTIC_MAX_PULSE_MS 200
// The motor driver on this board is an active-high transistor. Flip to 1 if a
// board revision ever drives it through an inverting stage.
#define HAPTIC_ACTIVE_LOW   0

typedef struct {
    gpio_num_t gpio;
    bool active_low;
    bool stable_pressed;
    bool candidate_pressed;
    uint8_t candidate_samples;
    int64_t pressed_at_us;
    bool long_fired;
} key_state_t;

typedef struct {
    board_key_callback_t callback;
    void *user_data;
} key_listener_t;

static key_state_t s_keys[BOARD_KEY_MAX] = {
    [BOARD_KEY_BOOT] = { .gpio = BOOT_GPIO, .active_low = true },
    [BOARD_KEY_PWR] = { .gpio = PWR_GPIO, .active_low = false },
};

static key_listener_t s_listeners[BOARD_KEYS_MAX_LISTENERS];
static SemaphoreHandle_t s_listeners_mutex;
static TaskHandle_t s_task;
static atomic_bool s_haptic_enabled = true;
static atomic_uint s_haptic_request_ms = 0;
static bool s_initialized;
static bool s_interrupt_ready;

static void IRAM_ATTR key_isr(void *arg)
{
    // A held level must not keep firing. The task rearms after debounced release.
    gpio_intr_disable((gpio_num_t)(uintptr_t)arg);
    BaseType_t higher_priority_woken=pdFALSE;
    if(s_task)vTaskNotifyGiveFromISR(s_task,&higher_priority_woken);
    if(higher_priority_woken)portYIELD_FROM_ISR();
}

static void set_key_wakeup(bool enabled)
{
    for(int i=0;i<BOARD_KEY_MAX;++i) {
        key_state_t *key=&s_keys[i];
        if(!enabled) {
            gpio_intr_disable(key->gpio);
            gpio_wakeup_disable(key->gpio);
        } else {
            esp_err_t ret=gpio_wakeup_enable(key->gpio,
                key->active_low?GPIO_INTR_LOW_LEVEL:GPIO_INTR_HIGH_LEVEL);
            if(ret==ESP_OK)ret=gpio_intr_enable(key->gpio);
            if(ret!=ESP_OK) {
                s_interrupt_ready=false;
                set_key_wakeup(false);
                ESP_LOGW(TAG,"GPIO wake setup failed; using polling: %s",esp_err_to_name(ret));
                return;
            }
        }
    }
}

static bool read_pressed(const key_state_t *key)
{
    const int level = gpio_get_level(key->gpio);
    return key->active_low ? (level == 0) : (level != 0);
}

static void drive_motor(bool on)
{
    const int level = HAPTIC_ACTIVE_LOW ? (on ? 0 : 1) : (on ? 1 : 0);
    gpio_set_level(HAPTIC_GPIO, level);
}

void board_keys_haptic_pulse(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return;
    }
    if (duration_ms > HAPTIC_MAX_PULSE_MS) {
        duration_ms = HAPTIC_MAX_PULSE_MS;
    }

    drive_motor(true);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    drive_motor(false);
}

static void pulse_haptic(void)
{
    if (!s_haptic_enabled) {
        return;
    }
    board_keys_haptic_pulse(HAPTIC_PULSE_MS);
}

static void dispatch_event(board_key_t key, board_key_event_t event)
{
    board_key_callback_t callbacks[BOARD_KEYS_MAX_LISTENERS] = {};
    void *contexts[BOARD_KEYS_MAX_LISTENERS] = {};
    size_t count = 0;

    if (s_listeners_mutex != nullptr &&
            xSemaphoreTake(s_listeners_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (size_t i = 0; i < BOARD_KEYS_MAX_LISTENERS; ++i) {
            if (s_listeners[i].callback != nullptr) {
                callbacks[count] = s_listeners[i].callback;
                contexts[count] = s_listeners[i].user_data;
                ++count;
            }
        }
        xSemaphoreGive(s_listeners_mutex);
    }

    for (size_t i = 0; i < count; ++i) {
        callbacks[i](key, event, contexts[i]);
    }
}

static void scan_key(key_state_t *key, board_key_t id, bool pressed, int64_t now_us)
{
    const int long_ms = id == BOARD_KEY_BOOT ? BOOT_LONG_MS : KEY_LONG_MS;
    if (pressed != key->candidate_pressed) {
        key->candidate_pressed = pressed;
        key->candidate_samples = 0;
    } else if (key->candidate_samples < KEY_DEBOUNCE_SAMPLES) {
        ++key->candidate_samples;
    }

    if (key->candidate_samples >= KEY_DEBOUNCE_SAMPLES &&
            key->stable_pressed != key->candidate_pressed) {
        key->stable_pressed = key->candidate_pressed;
        if (key->stable_pressed) {
            key->pressed_at_us = now_us;
            key->long_fired = false;
        } else {
            const int64_t held_ms = (now_us - key->pressed_at_us) / 1000;
            if (!key->long_fired && held_ms >= KEY_SHORT_MIN_MS) {
                // Catch a release on the threshold even if the scanner never
                // observed a pressed sample after it. Never emit both events.
                if (held_ms >= long_ms) {
                    pulse_haptic();
                    dispatch_event(id, BOARD_KEY_LONG_PRESS);
                } else if (id == BOARD_KEY_BOOT || held_ms < KEY_SHORT_MAX_MS) {
                    pulse_haptic();
                    dispatch_event(id, BOARD_KEY_SHORT_PRESS);
                }
            }
            key->long_fired = false;
        }
    }
    if (key->stable_pressed && !key->long_fired &&
            (now_us - key->pressed_at_us) / 1000 >= long_ms) {
        key->long_fired = true;
        pulse_haptic();
        dispatch_event(id, BOARD_KEY_LONG_PRESS);
    }
}

static void key_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS);
    while (true) {
        const unsigned haptic_ms = atomic_exchange(&s_haptic_request_ms, 0);
        if (haptic_ms && s_haptic_enabled) board_keys_haptic_pulse(haptic_ms);
        const int64_t now_us = esp_timer_get_time();

        bool settled_idle=true;
        for (int index = 0; index < BOARD_KEY_MAX; ++index) {
            key_state_t *key = &s_keys[index];
            scan_key(key, (board_key_t)index, read_pressed(key), now_us);
            if(key->stable_pressed || key->candidate_pressed ||
               key->candidate_samples<KEY_DEBOUNCE_SAMPLES)settled_idle=false;
        }

        if(s_interrupt_ready && settled_idle) {
            // Level wake also covers a press arriving between the last sample
            // and arming, or while the CPU is in automatic Light-sleep.
            set_key_wakeup(true);
            ulTaskNotifyTake(pdTRUE,s_interrupt_ready?portMAX_DELAY:period);
            set_key_wakeup(false);
        } else {
            // During a press/debounce only, retain the original 10ms scanner.
            vTaskDelay(period);
        }
    }
}

static esp_err_t configure_input(gpio_num_t gpio, bool pull_up, bool pull_down)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

esp_err_t board_keys_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_listeners_mutex = xSemaphoreCreateMutex();
    if (s_listeners_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // BOOT carries the board's pull-up. PWR is driven by SYS_OUT through T1,
    // so both internal pulls stay off to avoid fighting the external circuit.
    ESP_RETURN_ON_ERROR(configure_input(BOOT_GPIO, true, false), TAG, "Configure BOOT failed");
    ESP_RETURN_ON_ERROR(configure_input(PWR_GPIO, false, false), TAG, "Configure PWR failed");

    const gpio_config_t haptic_config = {
        .pin_bit_mask = 1ULL << HAPTIC_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&haptic_config), TAG, "Configure haptic failed");
    drive_motor(false);

    // Seed the debounce state so a key held during boot is not reported twice.
    for (int index = 0; index < BOARD_KEY_MAX; ++index) {
        key_state_t *key = &s_keys[index];
        key->stable_pressed = read_pressed(key);
        key->candidate_pressed = key->stable_pressed;
        key->candidate_samples = KEY_DEBOUNCE_SAMPLES;
        key->pressed_at_us = esp_timer_get_time();
        key->long_fired = false;
    }

    esp_err_t irq_result=gpio_install_isr_service(0);
    if(irq_result==ESP_ERR_INVALID_STATE)irq_result=ESP_OK; // Shared with touch/TE.
    if(irq_result==ESP_OK)irq_result=gpio_isr_handler_add(BOOT_GPIO,key_isr,(void *)(uintptr_t)BOOT_GPIO);
    if(irq_result==ESP_OK)irq_result=gpio_isr_handler_add(PWR_GPIO,key_isr,(void *)(uintptr_t)PWR_GPIO);
    if(irq_result==ESP_OK)irq_result=esp_sleep_enable_gpio_wakeup();
    s_interrupt_ready=irq_result==ESP_OK;
    if(!s_interrupt_ready) {
        gpio_isr_handler_remove(BOOT_GPIO);
        gpio_isr_handler_remove(PWR_GPIO);
        ESP_LOGW(TAG,"Key interrupts unavailable; using polling: %s",esp_err_to_name(irq_result));
    }

    if (xTaskCreatePinnedToCore(key_task, "board_keys", 3072, nullptr, 4, &s_task, 1) != pdPASS) {
        if(s_interrupt_ready) {
            gpio_isr_handler_remove(BOOT_GPIO);gpio_isr_handler_remove(PWR_GPIO);
            s_interrupt_ready=false;
        }
        vSemaphoreDelete(s_listeners_mutex);
        s_listeners_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "BOOT(GPIO%d) and PWR(GPIO%d) keys are ready (%s)", BOOT_GPIO, PWR_GPIO,
             s_interrupt_ready?"interrupt + Light-sleep wake":"polling fallback");
    return ESP_OK;
}

esp_err_t board_keys_add_listener(board_key_callback_t callback, void *user_data)
{
    if (callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_listeners_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_listeners_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_NO_MEM;
    for (size_t i = 0; i < BOARD_KEYS_MAX_LISTENERS; ++i) {
        if (s_listeners[i].callback == nullptr) {
            s_listeners[i].callback = callback;
            s_listeners[i].user_data = user_data;
            result = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_listeners_mutex);
    return result;
}

esp_err_t board_keys_remove_listener(board_key_callback_t callback, void *user_data)
{
    if (callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_listeners_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_listeners_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < BOARD_KEYS_MAX_LISTENERS; ++i) {
        if (s_listeners[i].callback == callback && s_listeners[i].user_data == user_data) {
            s_listeners[i].callback = nullptr;
            s_listeners[i].user_data = nullptr;
            result = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_listeners_mutex);
    return result;
}

bool board_keys_is_pressed(board_key_t key)
{
    if (key >= BOARD_KEY_MAX) {
        return false;
    }
    return s_keys[key].stable_pressed;
}

void board_keys_set_haptic_enabled(bool enabled)
{
    s_haptic_enabled = enabled;
}

bool board_keys_haptic_enabled(void)
{
    return s_haptic_enabled;
}

void board_keys_haptic_request(uint32_t duration_ms)
{
    if (duration_ms > HAPTIC_MAX_PULSE_MS) duration_ms = HAPTIC_MAX_PULSE_MS;
    if (s_initialized && s_haptic_enabled) {
        atomic_store(&s_haptic_request_ms, duration_ms);
        if(s_task)xTaskNotifyGive(s_task);
    }
}
