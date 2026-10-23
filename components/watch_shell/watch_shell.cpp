/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "watch_shell.hpp"
#include "watch_face_layout.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_random.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_brookesia.hpp"
#include "system_status.hpp"
#include "weather_service.h"
#include "wrist_gesture.h"
#include "XiaozhiApp.hpp"
#include "watch_power.h"
#include "esp_lvgl_port.h"

LV_IMAGE_DECLARE(face_bg);
LV_IMAGE_DECLARE(wx_qing);
LV_IMAGE_DECLARE(wx_yun);
LV_IMAGE_DECLARE(wx_yin);
LV_IMAGE_DECLARE(wx_yu);
LV_IMAGE_DECLARE(wx_lei);
LV_IMAGE_DECLARE(wx_xue);
LV_IMAGE_DECLARE(wx_wu);
LV_FONT_DECLARE(font_clock);
LV_FONT_DECLARE(font_meta);
LV_FONT_DECLARE(font_phrase);

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;
using namespace esp_brookesia::systems::phone;

namespace brookesia::watch_ui {

namespace {

const char *TAG = "WatchShell";

constexpr uint32_t TICK_PERIOD_MS = 250;
constexpr uint32_t INPUT_PERIOD_MS = 20;
constexpr uint32_t STANDBY_INPUT_PERIOD_MS = 50;
constexpr uint32_t STATUS_REFRESH_TICKS = 4;

constexpr int BAR_W = FACE_BATTERY_W;
constexpr int FACE_GIF_COUNT = 11;
constexpr const char *FACE_GIF_FORMAT = "A:/spiffs/watchface/face%02d.gif";

const char *const WEEKDAY_NAMES[] = {"日", "一", "二", "三", "四", "五", "六"};

const lv_image_dsc_t *weather_glyph(const char *icon)
{
    if (icon == nullptr) {
        return &wx_yin;
    }
    if (strcmp(icon, "qing") == 0) {
        return &wx_qing;
    }
    if (strcmp(icon, "yun") == 0) {
        return &wx_yun;
    }
    if (strcmp(icon, "yu") == 0) {
        return &wx_yu;
    }
    if (strcmp(icon, "lei") == 0) {
        return &wx_lei;
    }
    if (strcmp(icon, "xue") == 0) {
        return &wx_xue;
    }
    if (strcmp(icon, "wu") == 0) {
        return &wx_wu;
    }
    return &wx_yin;
}

} // namespace

WatchShell &WatchShell::instance()
{
    static WatchShell shell;
    return shell;
}

bool WatchShell::begin(Phone *phone)
{
    if (_started) {
        return true;
    }
    if (phone == nullptr) {
        return false;
    }

    initControlService();
    _phone = phone;
    _status_bar = phone->getDisplay().getStatusBar();

    ESP_LOGI(
        TAG, "PSRAM free before face: %u bytes, FS 'A' ready: %d",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<int>(lv_fs_is_ready('A'))
    );
    // Independent of LVGL: proves whether the storage image really carries the
    // animations, so a failing gif can be blamed on the right layer.
    #if WATCH_DEBUG_ENABLED
    {
        FILE *probe = std::fopen("/spiffs/watchface/face01.gif", "rb");
        ESP_LOGI(TAG, "probe face01.gif: %s", (probe != nullptr) ? "ok" : "missing");
        if (probe != nullptr) {
            std::fclose(probe);
        }
    }
    #endif

    {
        LvLockGuard gui_guard;
        createFace();
        if (_face == nullptr) {
            ESP_LOGE(TAG, "Create watch face failed");
            return false;
        }
        #if WATCH_DEBUG_ENABLED
        lv_display_t *display = lv_obj_get_display(_face);
        lv_display_add_event_cb(display, renderCallback, LV_EVENT_RENDER_START, this);
        lv_display_add_event_cb(display, renderCallback, LV_EVENT_RENDER_READY, this);
        #endif
        createGrid();
        if (_launcher_view == nullptr) {
            ESP_LOGE(TAG, "Create watch grid failed");
            return false;
        }
        refreshTime();
        refreshStatus();
        refreshWeather();
        showFace();

        // The gesture hint bar would sit on top of the watch face.
        if (Gesture *gesture = _phone->getManager().getGesture()) {
            gesture->setIndicatorBarVisible(Gesture::IndicatorBarType::BOTTOM, false);
        }

        _timer = lv_timer_create(timerCallback, TICK_PERIOD_MS, this);
        if (_timer == nullptr) {
            ESP_LOGE(TAG, "Create watch face timer failed");
            return false;
        }
        _input_timer = lv_timer_create(inputTimerCallback, INPUT_PERIOD_MS, this);
        if (_input_timer == nullptr) {
            ESP_LOGE(TAG, "Create watch input timer failed");
            lv_timer_delete(_timer);
            _timer = nullptr;
            return false;
        }
    }

    esp_err_t result = board_keys_add_listener(keyCallback, this);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Register key listener failed: %s", esp_err_to_name(result));
        return false;
    }
    result = wrist_gesture_add_listener(wristCallback, this);
    if (result != ESP_OK) {
        // Not fatal: the watch face still works from the BOOT key.
        ESP_LOGW(TAG, "Register wrist listener failed: %s", esp_err_to_name(result));
    }

    _started = true;
    ESP_LOGI(TAG, "Watch face and app grid are ready");
    return true;
}

void WatchShell::createFace()
{
    lv_obj_t *main_obj = _phone->getDisplay().getMainScreenObject();
    lv_obj_t *parent = main_obj ? lv_obj_get_parent(main_obj) : lv_screen_active();
    if (!parent || !watch_face_view_create(parent, &_face_view)) return;
    _face = _face_view.root;
    _background = _face_view.background;
    _date_label = _face_view.date;
    _weekday_label = _face_view.weekday;
    _time_label = _face_view.time;
    _temp_label = _face_view.temperature;
    _weather_icon = _face_view.weather;
    _battery_track = _face_view.battery_track;
    _battery_fill = _face_view.battery_fill;
    for (int i = 0; i < WATCH_FACE_PHRASES; ++i)
        _phrase_labels[i] = _face_view.phrases[i];
}

void WatchShell::startRandomGif()
{
    stopGif();
    if (!_face_visible || _face == nullptr) {
        return;
    }

    int index = static_cast<int>(esp_random() % FACE_GIF_COUNT);
    if (index == _last_gif) {
        index = (index + 1) % FACE_GIF_COUNT;
    }
    _last_gif = index;

    randomizePhrases();
    #if WATCH_DEBUG_ENABLED
    _perf_started_us = esp_timer_get_time();
    _render_started_us = _render_total_us = _render_max_us = 0;
    _render_count = 0;
    #endif

    std::snprintf(_gif_path, sizeof(_gif_path), FACE_GIF_FORMAT, index + 1);

    _gif = lv_gif_create(_face);
    if (_gif == nullptr) {
        ESP_LOGE(TAG, "Create gif widget failed");
        return;
    }
    // Read the prepared compressed animation only once. While asleep this
    // prepares the NEXT wake, so no SPIFFS read is needed after lighting up.
    FILE *file = std::fopen(_gif_path + 2, "rb"); // Strip LVGL's A: prefix.
    long length = 0;
    if (file && std::fseek(file, 0, SEEK_END) == 0) length = std::ftell(file);
    if (file && length > 0 && length <= 1024 * 1024 &&
            std::fseek(file, 0, SEEK_SET) == 0) {
        _gif_data = static_cast<uint8_t *>(heap_caps_malloc(
            static_cast<size_t>(length), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    const bool read_ok = _gif_data &&
        std::fread(_gif_data, 1, static_cast<size_t>(length), file) == static_cast<size_t>(length);
    if (file) std::fclose(file);
    if (!read_ok) {
        ESP_LOGE(TAG, "Cannot preload GIF: %s", _gif_path);
        stopGif();
        return;
    }
    _gif_source = {};
    _gif_source.header.magic = LV_IMAGE_HEADER_MAGIC;
    _gif_source.header.cf = LV_COLOR_FORMAT_RAW;
    _gif_source.data_size = static_cast<uint32_t>(length);
    _gif_source.data = _gif_data;
    lv_obj_remove_flag(_gif, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_gif_set_src(_gif, &_gif_source);
    lv_gif_set_loop_count(_gif, 0); // Repeat every sticker, regardless of file loop metadata.
    if (!_screen_on) lv_gif_pause(_gif); // Keep the decoded first frame ready for wake.
    watch_face_view_place_animation(_gif, index + 1);

    lv_obj_update_layout(_gif);
    if (!lv_gif_is_loaded(_gif)) {
        ESP_LOGE(TAG, "GIF failed to load: %s (PSRAM %u)", _gif_path,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    } else {
        ESP_LOGI(TAG, "%s %s (%dx%d, load=%lldms)", _screen_on ? "Playing" : "Prepared",
                 _gif_path, static_cast<int>(lv_obj_get_width(_gif)),
                 static_cast<int>(lv_obj_get_height(_gif)),
                 static_cast<long long>((esp_timer_get_time() - _perf_started_us) / 1000));
    }
}

void WatchShell::stopGif()
{
    if (_gif != nullptr) {
        lv_obj_delete(_gif);
        _gif = nullptr;
    }
    heap_caps_free(_gif_data);
    _gif_data = nullptr;
    _gif_source = {};
}

void WatchShell::randomizePhrases()
{
    // Complete reference arrangements keep long captions in suitable slots.
    // The GIF itself still comes from all eleven user-supplied animations.
    const unsigned scene = static_cast<unsigned>(esp_random() % WATCH_FACE_SCENES);
    watch_face_view_set_scene(&_face_view, scene);
}

void WatchShell::setUiStandby(bool standby)
{
    lv_indev_t *touch=bsp_display_get_input_dev();
    if(_phone) {
        if(auto *gesture=_phone->getManager().getGesture())
            gesture->setInputSuspended(standby || _launcher_open || _controls_visible);
    }
    if(standby) {
        lv_timer_t *refr=lv_display_get_refr_timer(lv_display_get_default());
        _sleep_refr_timer=refr && !lv_timer_get_paused(refr)?refr:nullptr;
        if(_sleep_refr_timer)lv_timer_pause(_sleep_refr_timer);
        lv_timer_t *read=touch?lv_indev_get_read_timer(touch):nullptr;
        _sleep_touch_timer=read && !lv_timer_get_paused(read)?read:nullptr;
        if(_sleep_touch_timer)lv_timer_pause(_sleep_touch_timer);
        if(_timer)lv_timer_pause(_timer);
    } else {
        if(_sleep_refr_timer)lv_timer_resume(_sleep_refr_timer);
        if(_sleep_touch_timer)lv_timer_resume(_sleep_touch_timer);
        _sleep_refr_timer=nullptr;_sleep_touch_timer=nullptr;
        if(_timer)lv_timer_resume(_timer);
    }
    if(touch)lv_indev_enable(touch,!standby);
    // Keep a small UI-owned input/control pump for keys, wrist and countdowns.
    // Producer tasks only notify this worker, never render from their stacks.
    if(_input_timer)lv_timer_set_period(_input_timer,standby?STANDBY_INPUT_PERIOD_MS:INPUT_PERIOD_MS);
}

void WatchShell::setScreenOn(bool on)
{
    if (_screen_on == on) return;
    if(on) {
        watch_power_set_screen_on(true);
        setUiStandby(false);
    }
    if (!on && !_torch_mode) {
        const int brightness = bsp_display_brightness_get();
        if (brightness >= 10) _normal_brightness = brightness;
    }
    _screen_on = on;
    esp_brookesia::apps::XiaozhiApp::requestInstance()->setVoiceScreenOn(on);
    if (!on) {
        // Hide emission BEFORE deleting the old character or doing file I/O.
        bsp_display_backlight_off();
        const esp_err_t panel_result=bsp_display_set_panel_on(false);
        if(panel_result!=ESP_OK)ESP_LOGW(TAG,"Panel off failed: %s",esp_err_to_name(panel_result));
        if (!_controls_visible && _face_visible) startRandomGif(); // Prepare the next character while dark.
        else if (_gif) lv_gif_pause(_gif);
        setUiStandby(true);
        watch_power_set_screen_on(false);
        return;
    }

    #if WATCH_DEBUG_ENABLED
    const int64_t started_us = esp_timer_get_time();
    _perf_started_us = started_us;
    _render_started_us = _render_total_us = _render_max_us = 0;
    _render_count = 0;
    #endif
    // Normal wake reuses the paused, decoded first frame. Failed prefetch gets
    // one retry here, still with the panel dark until a complete frame exists.
    if (_face_visible && (!_gif || !lv_gif_is_loaded(_gif))) startRandomGif();
    if (_gif) lv_gif_pause(_gif);
    refreshTime();
    refreshStatus();
    refreshWeather();
    lv_obj_t *surface = _controls_visible ? _control_view.root :
        (_face_visible ? _face : (_launcher_open ? watch_launcher_view_surface(_launcher_view) : lv_screen_active()));
    if (surface) {
        lv_obj_invalidate(surface);
        lv_refr_now(lv_obj_get_display(surface));
    }
    // tx_param drains the queued QSPI pixel transfers before sending brightness.
    // Thus the complete background AND GIF first frame are already in panel GRAM.
    esp_err_t result = bsp_display_set_panel_on(true);
    if(result==ESP_OK)result=bsp_display_brightness_set(_torch_mode ? 100 : _normal_brightness);
    if (result != ESP_OK) {
        _screen_on = false;
        esp_brookesia::apps::XiaozhiApp::requestInstance()->setVoiceScreenOn(false);
        bsp_display_backlight_off();
        bsp_display_set_panel_on(false);
        setUiStandby(true);
        watch_power_set_screen_on(false);
        ESP_LOGE(TAG, "Wake brightness failed: %s", esp_err_to_name(result));
        return;
    }
    if (_gif && !_controls_visible && _face_visible) lv_gif_resume(_gif); // Resume only a visible character.
    #if WATCH_DEBUG_ENABLED
    ESP_LOGI(TAG, "Wake: complete frame visible in %lldms (GIF prepared=%d task=%s stack_free=%u)",
             static_cast<long long>((esp_timer_get_time() - started_us) / 1000),
             _gif && lv_gif_is_loaded(_gif), pcTaskGetName(nullptr),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    #endif
}

void WatchShell::applyStatusBarMode()
{
    if (_status_bar == nullptr) {
        return;
    }

    const bool app_active = (_phone != nullptr) &&
                            (_phone->getManager().getActiveApp() != nullptr);
    if (app_active) {
        // The running app owns the status bar mode; do not fight its config.
        return;
    }

    // The launcher has no time/header widgets; hide the system bar in the shell.
    _status_bar->setVisualMode(StatusBar::VisualMode::HIDE);
}

void WatchShell::refreshTime()
{
    if (_time_label == nullptr || _date_label == nullptr) {
        return;
    }

    time_t now = 0;
    struct tm time_info = {};
    time(&now);
    localtime_r(&now, &time_info);

    const int minute_stamp=((time_info.tm_year*366+time_info.tm_yday)*24+
        time_info.tm_hour)*60+time_info.tm_min;
    if (minute_stamp == _last_minute) {
        return;
    }
    _last_minute = minute_stamp;

    char clock_text[16] = {};
    std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", time_info.tm_hour, time_info.tm_min);
    lv_label_set_text(_time_label, clock_text);

    char date_text[16] = {};
    std::snprintf(date_text, sizeof(date_text), "%02d-%02d", time_info.tm_mon + 1, time_info.tm_mday);
    lv_label_set_text(_date_label, date_text);

    char weekday_text[24] = {};
    std::snprintf(
        weekday_text, sizeof(weekday_text), "星期%s",
        WEEKDAY_NAMES[time_info.tm_wday % 7]
    );
    lv_label_set_text(_weekday_label, weekday_text);
}

void WatchShell::refreshStatus()
{
    if (_battery_fill == nullptr) {
        return;
    }

    brookesia::system_status::Snapshot snapshot;
    const bool snapshot_valid = brookesia::system_status::get_snapshot(snapshot);

    // A board running from USB may report no battery at all. The face always
    // shows the bar, so fall back to a full one instead of an empty track.
    int percent = 100;
    if (snapshot_valid && snapshot.battery_valid && snapshot.battery_present &&
            snapshot.battery_percent >= 0) {
        percent = snapshot.battery_percent;
        if (percent > 100) {
            percent = 100;
        }
        if (percent < 3) {
            percent = 3;
        }
    }

    if (percent == _last_battery) {
        return;
    }
    _last_battery = percent;
    lv_obj_set_width(_battery_fill, BAR_W * percent / 100);
    ESP_LOGI(TAG, "Battery bar %d%% (valid=%d present=%d raw=%d)", percent,
             static_cast<int>(snapshot_valid),
             snapshot_valid ? static_cast<int>(snapshot.battery_present) : -1,
             snapshot_valid ? snapshot.battery_percent : -1);
}

void WatchShell::refreshWeather()
{
    if (_temp_label == nullptr || _weather_icon == nullptr) {
        return;
    }

    weather_snapshot_t snapshot = {};
    const bool valid = weather_service_get(&snapshot);
    if (valid && snapshot.fetched_ms == _last_weather_ms) {
        return;
    }
    _last_weather_ms = snapshot.fetched_ms;

    char temp_text[12] = {};
    if (valid) {
        std::snprintf(temp_text, sizeof(temp_text), "%d°", snapshot.temp_c);
    } else {
        std::snprintf(temp_text, sizeof(temp_text), "--°");
    }
    lv_label_set_text(_temp_label, temp_text);
    lv_image_set_src(_weather_icon, weather_glyph(valid ? snapshot.icon : nullptr));
}

void WatchShell::keyCallback(board_key_t key, board_key_event_t event, void *user_data)
{
    auto *shell = static_cast<WatchShell *>(user_data);
    if (shell != nullptr && event == BOARD_KEY_SHORT_PRESS) {
        if (key == BOARD_KEY_BOOT) shell->_pending_boot.store(true);
        else if (key == BOARD_KEY_PWR) shell->_pending_pwr.store(true);
    } else if (shell != nullptr && key == BOARD_KEY_BOOT && event == BOARD_KEY_LONG_PRESS) {
        shell->_pending_boot_long.store(true);
    }
    if(shell)lvgl_port_task_wake(LVGL_PORT_EVENT_USER,nullptr);
}

void WatchShell::wristCallback(bool screen_up, void *user_data)
{
    auto *shell = static_cast<WatchShell *>(user_data);
    if (shell != nullptr) {
        shell->_pending_wrist.store(screen_up ? 1 : 0);
        lvgl_port_task_wake(LVGL_PORT_EVENT_USER,nullptr);
    }
}

void WatchShell::inputTimerCallback(lv_timer_t *timer)
{
    auto *shell = static_cast<WatchShell *>(lv_timer_get_user_data(timer));
    if (!shell) return;
    // This callback runs on taskLVGL, with its 20 KB stack and the LVGL lock.
    // Never render or load a GIF on the 3 KB wrist/key producer tasks.
    const int wrist = shell->_pending_wrist.exchange(-1);
    const bool boot = shell->_pending_boot.exchange(false);
    const bool boot_long = shell->_pending_boot_long.exchange(false);
    const bool pwr = shell->_pending_pwr.exchange(false);
    shell->controlTick();
    // Apps can also close themselves through an on-screen back button.
    if(shell->_return_to_launcher && !shell->_controls_visible &&
       shell->_phone->getManager().getActiveApp()==nullptr)shell->showGrid(true);
    if (wrist >= 0) shell->handleWrist(wrist != 0);
    if (pwr) shell->handleKey(BOARD_KEY_PWR, BOARD_KEY_SHORT_PRESS);
    else if (boot_long) shell->handleKey(BOARD_KEY_BOOT, BOARD_KEY_LONG_PRESS);
    else if (boot) shell->handleKey(BOARD_KEY_BOOT, BOARD_KEY_SHORT_PRESS);
}

void WatchShell::handleWrist(bool screen_up)
{
    if (_phone == nullptr || _screen_on == screen_up) {
        return;
    }

    if (_keep_awake || _torch_mode || (screen_up && !_raise_to_wake)) return;
    // Apps retain the screen unless our control-center overlay is in front.
    if (!_controls_visible && _phone->getManager().getActiveApp() != nullptr) {
        return;
    }

    setScreenOn(screen_up);
}

void WatchShell::handleKey(board_key_t key, board_key_event_t event)
{
    if (_phone == nullptr) {
        return;
    }
    if (key == BOARD_KEY_BOOT && event == BOARD_KEY_LONG_PRESS) {
        wakeVoiceAssistant();
        return;
    }
    if (event != BOARD_KEY_SHORT_PRESS) {
        return;
    }

    if (key == BOARD_KEY_PWR) {
        toggleControls();
        return;
    }
    if (_controls_visible) {
        if (!_touch_locked) {closeControls();if(!_screen_on)setScreenOn(true);}
        return;
    }
    if (key != BOARD_KEY_BOOT) {
        return;
    }

    if (_phone->getManager().getActiveApp() != nullptr) {
        // HOME performs the app's pause lifecycle and loads Brookesia's main
        // screen. BACK could merely change an app's internal subpage.
        _phone->sendNavigateEvent(base::Manager::NavigateType::HOME);
        if(_phone->getManager().getActiveApp()==nullptr)showGrid(true);
        if(!_screen_on)setScreenOn(true);
        return;
    }
    if (_face_visible) showGrid();
    else if (_launcher_open && _launcher_view->all_apps)
        watch_launcher_view_page(_launcher_view,false,false);
    else showFace();
    if(!_screen_on)setScreenOn(true);
}

void WatchShell::renderCallback(lv_event_t *event)
{
    #if WATCH_DEBUG_ENABLED
    auto *shell = static_cast<WatchShell *>(lv_event_get_user_data(event));
    if (!shell) return;
    if (shell->_controls_visible || !shell->_screen_on || !shell->_face_visible ||
        shell->_phone->getManager().getActiveApp() != nullptr) return;
    const int64_t now = esp_timer_get_time();
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        shell->_render_started_us = now;
    } else if (shell->_render_started_us > 0) {
        const int64_t duration = now - shell->_render_started_us;
        shell->_render_total_us += duration;
        if (duration > shell->_render_max_us) shell->_render_max_us = duration;
        shell->_render_count++;
        shell->_render_started_us = 0;
    }
    #endif
}

void WatchShell::timerCallback(lv_timer_t *timer)
{
    auto *shell = static_cast<WatchShell *>(lv_timer_get_user_data(timer));
    if (shell == nullptr) {
        return;
    }

    if(!shell->_screen_on)return;

    shell->refreshControls();
    shell->refreshTime();
    if ((shell->_tick++ % STATUS_REFRESH_TICKS) == 0) {
        shell->refreshStatus();
    }
    shell->refreshWeather();
    // Brookesia re-applies the status bar mode on every screen change, so the
    // hidden state is re-asserted on every tick.
    shell->applyStatusBarMode();
    #if WATCH_DEBUG_ENABLED
    const int64_t now = esp_timer_get_time();
    if (shell->_gif && !shell->_controls_visible && shell->_screen_on && shell->_face_visible &&
        now - shell->_perf_started_us >= 5000000) {
        ESP_LOGI(TAG, "FacePerf: renders=%u in %lldms render_avg=%lldus render_max=%lldus free_dma=%u",
                 static_cast<unsigned>(shell->_render_count),
                 static_cast<long long>((now - shell->_perf_started_us) / 1000),
                 static_cast<long long>(shell->_render_count ?
                     shell->_render_total_us / shell->_render_count : 0),
                 static_cast<long long>(shell->_render_max_us),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
        shell->_perf_started_us = now;
        shell->_render_total_us = shell->_render_max_us = 0;
        shell->_render_count = 0;
    }
    #endif
}

} // namespace brookesia::watch_ui
