/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wrist_gesture.h"

#include <math.h>
#include <stddef.h>
#include <stdatomic.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "qmi8658.h"
#include "watch_power.h"

static const char *TAG = "WristGesture";

#define WRIST_TASK_PERIOD_MS    50
#define WRIST_TASK_STACK_BYTES  3072
// Rotation around the sensor X axis (verified on the board: flipping the face
// towards the wearer swings ax from +9.6 to -10 and pitchX from +81 to -90).
// Positive pitch opens the screen; horizontal or negative pitch closes it.
#define WRIST_OPEN_ANGLE_DEG    45.0F
#define WRIST_CLOSE_ANGLE_DEG   25.0F
#define WRIST_SETTLE_SAMPLES    4
// Do not blank the panel right after boot: the board may be resting at an odd
// angle while the user is still looking at the startup screen.
#define WRIST_BOOT_GRACE_MS     4000
#define WRIST_I2C_ADDRESS       QMI8658_ADDRESS_HIGH

// Verified against the board: positive pitchX is the "towards the wearer" flip.
#define WRIST_OPEN_SIGN         (1.0F)

#define WRIST_CONTROL_BACKLIGHT 1
#define WRIST_LOG_EVERY_SAMPLES 40

#define RAD_TO_DEG              57.29578F

typedef struct {
    wrist_listener_t callback;
    void *user_data;
} wrist_listener_entry_t;

static qmi8658_dev_t s_imu;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_listeners_mutex;
static SemaphoreHandle_t s_sensor_mutex;
static bool s_app_active;
static int s_sensor_profile=-1;
static wrist_listener_entry_t s_listeners[WRIST_MAX_LISTENERS];
static volatile bool s_initialized;
static volatile bool s_enabled = true;
static volatile bool s_screen_up = true;
static atomic_bool s_publish_state;

static esp_err_t apply_sensor_profile_locked(void)
{
    const bool screen_on=watch_power_is_screen_on();
    const int profile=s_app_active && screen_on?2:(!s_enabled?0:(screen_on?1:3));
    if(profile==s_sensor_profile)return ESP_OK;
    s_sensor_profile=-1;
    esp_err_t result=qmi8658_enable_sensors(&s_imu,QMI8658_DISABLE_ALL);
    if(result!=ESP_OK)return result;
    if(profile!=0) {
        result=qmi8658_set_accel_range(&s_imu,QMI8658_ACCEL_RANGE_4G);
        if(result!=ESP_OK)return result;
        const qmi8658_accel_odr_t odr=profile==2?QMI8658_ACCEL_ODR_500HZ:
            (profile==1?QMI8658_ACCEL_ODR_125HZ:QMI8658_ACCEL_ODR_LOWPOWER_21HZ);
        result=qmi8658_set_accel_odr(&s_imu,odr);
        if(result!=ESP_OK)return result;
        // Both wrist orientation and Gravitysphere only consume acceleration.
        result=qmi8658_enable_sensors(&s_imu,QMI8658_ENABLE_ACCEL);
        if(result!=ESP_OK)return result;
    }
    s_sensor_profile=profile;
    ESP_LOGI(TAG,"IMU: %s, gyro off",profile==2?"app 500Hz":
             (profile==1?"wrist 125Hz":(profile==3?"low-power 21Hz":"standby")));
    return ESP_OK;
}

esp_err_t wrist_gesture_read_accel(float *x,float *y,float *z)
{
    if(!s_initialized || !s_sensor_mutex)return ESP_ERR_INVALID_STATE;
    if(!x || !y || !z)return ESP_ERR_INVALID_ARG;
    if(xSemaphoreTake(s_sensor_mutex,pdMS_TO_TICKS(100))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t result=apply_sensor_profile_locked();
    if(result==ESP_OK)result=s_sensor_profile==0?ESP_ERR_INVALID_STATE:
        qmi8658_read_accel(&s_imu,x,y,z);
    xSemaphoreGive(s_sensor_mutex);
    return result;
}

esp_err_t wrist_gesture_set_app_active(bool active)
{
    if(!s_initialized || !s_sensor_mutex)return ESP_ERR_INVALID_STATE;
    if(xSemaphoreTake(s_sensor_mutex,pdMS_TO_TICKS(100))!=pdTRUE)return ESP_ERR_TIMEOUT;
    s_app_active=active;
    esp_err_t result=apply_sensor_profile_locked();
    if(result!=ESP_OK && active) {
        s_app_active=false;
        (void)apply_sensor_profile_locked();
    }
    xSemaphoreGive(s_sensor_mutex);
    return result;
}

static void dispatch(bool screen_up)
{
    wrist_listener_t callbacks[WRIST_MAX_LISTENERS] = {};
    void *contexts[WRIST_MAX_LISTENERS] = {};
    size_t count = 0;

    if (s_listeners_mutex != nullptr &&
            xSemaphoreTake(s_listeners_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (size_t i = 0; i < WRIST_MAX_LISTENERS; ++i) {
            if (s_listeners[i].callback != nullptr) {
                callbacks[count] = s_listeners[i].callback;
                contexts[count] = s_listeners[i].user_data;
                ++count;
            }
        }
        xSemaphoreGive(s_listeners_mutex);
    }

    for (size_t i = 0; i < count; ++i) {
        callbacks[i](screen_up, contexts[i]);
    }
}

static void wrist_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(WRIST_TASK_PERIOD_MS);
    const TickType_t started = xTaskGetTickCount();
    int settle = 0;
    bool candidate = s_screen_up;
    #if WATCH_DEBUG_ENABLED
    int log_counter = 0;
    #endif

    while (true) {
        qmi8658_data_t data = {};
        const esp_err_t sensor_result=wrist_gesture_read_accel(&data.accelX,&data.accelY,&data.accelZ);
        if (s_enabled && sensor_result == ESP_OK) {
            const float magnitude = sqrtf(
                data.accelX * data.accelX +
                data.accelY * data.accelY +
                data.accelZ * data.accelZ
            );

            if (magnitude > 1.0F) {
                // Pitch around the sensor X axis: flat face-up is ~0°, flipping
                // the face towards the wearer swings it towards +90°.
                const float pitch_deg =
                    atan2f(data.accelX, -data.accelZ) * RAD_TO_DEG * WRIST_OPEN_SIGN;
                bool target = s_screen_up;

                #if WATCH_DEBUG_ENABLED
                if (++log_counter >= WRIST_LOG_EVERY_SAMPLES) {
                    log_counter = 0;
                    ESP_LOGI(
                        TAG,
                        "ax=%+.2f ay=%+.2f az=%+.2f | pitchX=%+.1f | %s",
                        data.accelX, data.accelY, data.accelZ, pitch_deg,
                        s_screen_up ? "on" : "off"
                    );
                }
                #endif

                if (pitch_deg > WRIST_OPEN_ANGLE_DEG) {
                    target = true;
                } else if (pitch_deg < WRIST_CLOSE_ANGLE_DEG) {
                    target = false;
                } else {
                    // Between the angles the panel keeps its current state.
                    settle = 0;
                }

                // The panel starts ON. Keep that reported state until a real
                // debounced transition is dispatched after the startup grace.
                // Silently seeding it to face-down would leave the panel on.
                if ((xTaskGetTickCount() - started) < pdMS_TO_TICKS(WRIST_BOOT_GRACE_MS)) {
                    candidate = s_screen_up;
                    settle = 0;
                } else if (target != candidate) {
                    candidate = target;
                    settle = 0;
                } else if (candidate != s_screen_up && ++settle >= WRIST_SETTLE_SAMPLES) {
                    s_screen_up = candidate;
                    settle = 0;
                    ESP_LOGI(TAG, "Panel %s", s_screen_up ? "up" : "down");
#if WRIST_CONTROL_BACKLIGHT
                    dispatch(s_screen_up);
#endif
                }
                // A listener can attach after an earlier transition (e.g.
                // while assets load). Publish from this task to preserve order.
                if(atomic_exchange(&s_publish_state,false))dispatch(s_screen_up);
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t wrist_gesture_init(i2c_master_bus_handle_t bus)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_ARG, TAG, "Invalid I2C bus");

    s_listeners_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_listeners_mutex != nullptr, ESP_ERR_NO_MEM, TAG, "Create mutex failed");

    s_sensor_mutex=xSemaphoreCreateMutex();
    if(!s_sensor_mutex) {
        vSemaphoreDelete(s_listeners_mutex);s_listeners_mutex=nullptr;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = qmi8658_init(&s_imu, bus, WRIST_I2C_ADDRESS);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Initialize QMI8658 failed: %s", esp_err_to_name(result));
        goto error;
    }
    qmi8658_set_accel_unit_mps2(&s_imu, true);
    result=apply_sensor_profile_locked();
    if(result!=ESP_OK)goto error;

    s_initialized = true;
    // I2C sampling does no cache-disabled work. Keep only the task control
    // block in scarce internal SRAM, using PSRAM for the unchanged 3KiB stack.
    if (xTaskCreatePinnedToCoreWithCaps(wrist_task, "wrist_gesture", WRIST_TASK_STACK_BYTES,
            nullptr, 3, &s_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_initialized=false;
        ESP_LOGE(TAG,"Create wrist task failed: internal free=%u largest=%u PSRAM free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        result=ESP_ERR_NO_MEM;
        goto error;
    }

    ESP_LOGI(TAG, "QMI8658 wrist monitor is running (stack=%u in PSRAM)",WRIST_TASK_STACK_BYTES);
    return ESP_OK;

error:
    if(s_imu.dev_handle) {
        (void)qmi8658_enable_sensors(&s_imu,QMI8658_DISABLE_ALL);
        (void)i2c_master_bus_rm_device(s_imu.dev_handle);
        s_imu.dev_handle=nullptr;
    }
    s_sensor_profile=-1;
    vSemaphoreDelete(s_sensor_mutex);s_sensor_mutex=nullptr;
    vSemaphoreDelete(s_listeners_mutex);s_listeners_mutex=nullptr;
    return result;
}

esp_err_t wrist_gesture_add_listener(wrist_listener_t callback, void *user_data)
{
    if (callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_listeners_mutex, pdMS_TO_TICKS(100)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Lock listeners failed");

    esp_err_t result = ESP_ERR_NO_MEM;
    for (size_t i = 0; i < WRIST_MAX_LISTENERS; ++i) {
        if (s_listeners[i].callback == nullptr) {
            s_listeners[i].callback = callback;
            s_listeners[i].user_data = user_data;
            result = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_listeners_mutex);
    if(result==ESP_OK)atomic_store(&s_publish_state,true);
    return result;
}

esp_err_t wrist_gesture_remove_listener(wrist_listener_t callback, void *user_data)
{
    if (callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_listeners_mutex, pdMS_TO_TICKS(100)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Lock listeners failed");

    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < WRIST_MAX_LISTENERS; ++i) {
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

bool wrist_gesture_is_screen_up(void)
{
    return s_screen_up;
}

void wrist_gesture_set_enabled(bool enabled)
{
    s_enabled = enabled;
}
