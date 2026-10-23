/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <cstdint>

#include "board_keys.h"
#include "lvgl.h"
#include "watch_face_view.h"
#include "watch_control_view.h"
#include "watch_launcher_view.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace esp_brookesia::systems::phone {
class Phone;
class StatusBar;
} // namespace esp_brookesia::systems::phone

namespace brookesia::watch_ui {

/** Watch face, centered AI with five surrounding apps, and an all-app grid.
 * BOOT returns from any app to its menu, then to favorites / the watch face.
 * Holding BOOT opens Xiaozhi and requests an automatic listening turn.
 * All input and drawing remain on the LVGL task. */
class WatchShell {
public:
    static WatchShell &instance();

    /** Must be called after Phone::begin() and after every app is installed. */
    bool begin(esp_brookesia::systems::phone::Phone *phone);

private:
    WatchShell() = default;

    static void keyCallback(board_key_t key, board_key_event_t event, void *user_data);
    static void timerCallback(lv_timer_t *timer);
    static void inputTimerCallback(lv_timer_t *timer);
    static void launcherCallback(void *context,int app_id);
    static void wristCallback(bool screen_up, void *user_data);
    static void renderCallback(lv_event_t *event);

    void handleKey(board_key_t key, board_key_event_t event);
    void handleWrist(bool screen_up);
    void initControlService();
    void toggleControls();
    void closeControls();
    void finishControls();
    void refreshControls();
    void controlTick();
    void controlAction(watch_control_action_t action, int value);
    void controlToast(const char *text);
    bool queueControl(watch_control_action_t action, int value);
    static void controlCallback(void *context, watch_control_action_t action, int value);
    static void controlWorker(void *context);
    void createFace();
    void createGrid();
    void showFace();
    void showGrid(bool preserve_page = false);
    void launchApp(int app_id);
    void wakeVoiceAssistant();
    void startRandomGif();
    void stopGif();
    void randomizePhrases();
    void setScreenOn(bool on);
    void setUiStandby(bool standby);
    void applyStatusBarMode();
    void refreshTime();
    void refreshStatus();
    void refreshWeather();

    esp_brookesia::systems::phone::Phone *_phone = nullptr;
    esp_brookesia::systems::phone::StatusBar *_status_bar = nullptr;

    lv_obj_t *_face = nullptr;
    watch_face_view_t _face_view = {};
    lv_obj_t *_background = nullptr;
    lv_obj_t *_gif = nullptr;
    lv_obj_t *_date_label = nullptr;
    lv_obj_t *_weekday_label = nullptr;
    lv_obj_t *_time_label = nullptr;
    lv_obj_t *_temp_label = nullptr;
    lv_obj_t *_weather_icon = nullptr;
    lv_obj_t *_battery_track = nullptr;
    lv_obj_t *_battery_fill = nullptr;
    lv_obj_t *_phrase_labels[8] = {};

    watch_launcher_view_t *_launcher_view = nullptr; // PSRAM-owned view state.
    bool _launcher_open = false;
    bool _return_to_launcher = false;

    lv_timer_t *_timer = nullptr;
    lv_timer_t *_input_timer = nullptr;
    lv_timer_t *_sleep_refr_timer = nullptr;
    lv_timer_t *_sleep_touch_timer = nullptr;

    // Sensor/key tasks publish only. The LVGL task consumes these every 20 ms;
    // it owns all UI state, GIF loading and the synchronous wake redraw.
    // Coalesce rapid wrist changes to the most recent orientation.
    std::atomic<int> _pending_wrist{-1};
    std::atomic<bool> _pending_boot{false};
    std::atomic<bool> _pending_boot_long{false};
    std::atomic<bool> _pending_pwr{false};

    watch_control_view_t _control_view = {};
    bool _controls_visible = false;
    bool _raise_to_wake = true;
    bool _keep_awake = false;
    bool _airplane = false;
    bool _airplane_restore_wifi = false;
    bool _touch_locked = false;
    unsigned _torch_mode = 0;
    int _normal_brightness = 100;
    int64_t _countdown_end_us = 0;
    bool _timer_alarm_pending = false;
    int64_t _toast_until_us = 0;
    struct ControlCommand { int action; int value; };
    QueueHandle_t _control_queue = nullptr;
    bool _control_busy = false; // Owned by LVGL; one background request at a time.
    ControlCommand _control_pending = {};
    std::atomic<bool> _control_result_ready{false};
    int _control_result = 0; // Published before result_ready; consumed on LVGL.

    // Keep only the active compressed GIF in PSRAM; lv_gif borrows the descriptor.
    uint8_t *_gif_data = nullptr;
    lv_image_dsc_t _gif_source = {};
    char _gif_path[64] = {};

    bool _started = false;
    bool _face_visible = true;
    bool _screen_on = true;
    int _last_gif = -1;
    int _last_minute = -1;
    int _last_battery = -2;
    int64_t _last_weather_ms = -1;
    uint32_t _tick = 0;
    int64_t _perf_started_us = 0;
    int64_t _render_started_us = 0;
    int64_t _render_total_us = 0;
    int64_t _render_max_us = 0;
    uint32_t _render_count = 0;
};

} // namespace brookesia::watch_ui
