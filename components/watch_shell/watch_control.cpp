#include "watch_shell.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "esp_brookesia.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "system_status.hpp"

using namespace esp_brookesia;
using namespace esp_brookesia::systems;
namespace brookesia::watch_ui {
namespace { constexpr const char *TAG="ControlCenter"; }

void WatchShell::initControlService()
{
    if(_control_queue)return;
    _normal_brightness=std::clamp(bsp_display_brightness_get(),10,100);
    _control_queue=xQueueCreate(2,sizeof(ControlCommand));
    if (!_control_queue) return;
    // Only network/codec/haptic work runs here, never LVGL. NVS/Wi-Fi can
    // disable the flash cache, so this small worker must use internal SRAM.
    TaskHandle_t worker=nullptr;
    if(xTaskCreateWithCaps(controlWorker,"watch_controls",4096,this,3,&worker,
                          MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
        vQueueDelete(_control_queue);_control_queue=nullptr;
        ESP_LOGW(TAG,"Control worker allocation failed: internal free=%u largest=%u; will retry on action",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
    }
}
void WatchShell::controlWorker(void *context)
{
    auto *s=static_cast<WatchShell *>(context);
    ControlCommand command;
    while(xQueueReceive(s->_control_queue,&command,portMAX_DELAY)==pdTRUE) {
        esp_err_t result=ESP_OK;
        switch(command.action) {
        case WC_WIFI: case WC_AIRPLANE:
            result=system_status::set_wifi_enabled(command.value!=0);break;
        case WC_VOLUME: result=bsp_extra_codec_volume_set(command.value,nullptr);break;
        case WC_MUTE: result=bsp_extra_codec_user_mute_set(command.value!=0);break;
        case WC_TIMER:
            for(int i=0;i<3;++i) {
                if(board_keys_haptic_enabled())board_keys_haptic_request(60);
                vTaskDelay(pdMS_TO_TICKS(160));
            }
            break;
        default: result=ESP_ERR_INVALID_ARG;break;
        }
        s->_control_result=result;
        s->_control_result_ready.store(true);
    }
    vTaskDeleteWithCaps(nullptr);
}
bool WatchShell::queueControl(watch_control_action_t action,int value)
{
    if(_control_busy){controlToast("正在更新，请稍候");return false;}
    // Startup can temporarily exhaust internal RAM while app_main still owns
    // its stack. Retry after it returns; never move the NVS worker to PSRAM.
    if(!_control_queue)initControlService();
    if(!_control_queue){controlToast("后台服务不可用");return false;}
    _control_pending={static_cast<int>(action),value};
    _control_busy=true;
    if(xQueueSend(_control_queue,&_control_pending,0)!=pdTRUE) {
        _control_busy=false;controlToast("操作未提交，请重试");return false;
    }
    return true;
}
void WatchShell::controlToast(const char *message)
{
    if(!_control_view.root)return;
    watch_control_view_toast(&_control_view,message);
    _toast_until_us=esp_timer_get_time()+2000000;
}
void WatchShell::toggleControls()
{
    if(!_control_view.root && !watch_control_view_create(lv_layer_sys(),&_control_view,controlCallback,this))return;
    // PWR wakes directly into controls. A locked screen requires this physical
    // key; no touch gesture can dismiss the lock.
    if(_touch_locked) {
        _touch_locked=false;watch_control_view_lock(&_control_view,false);
        if(!_screen_on)setScreenOn(true);
        return;
    }
    if(_torch_mode) {
        _torch_mode=0;watch_control_view_torch(&_control_view,0);
        bsp_display_brightness_set(_normal_brightness);
        refreshControls();return;
    }
    if(_controls_visible && _screen_on){closeControls();return;}
    if(_screen_on)_normal_brightness=std::clamp(bsp_display_brightness_get(),10,100);
    _controls_visible=true;
    if(_gif)lv_gif_pause(_gif);
    if(auto *g=_phone->getManager().getGesture())g->setInputSuspended(true);
    #if WATCH_DEBUG_ENABLED
    const int64_t capture_started=esp_timer_get_time();
    #endif
    [[maybe_unused]] const bool backdrop=watch_control_view_capture(&_control_view,_launcher_open?watch_launcher_view_surface(_launcher_view):lv_screen_active());
    #if WATCH_DEBUG_ENABLED
    ESP_LOGI(TAG,"Backdrop: ready=%d prepare=%lldms snapshot=%ums blur=%ums PSRAM free=%u",backdrop,
             static_cast<long long>((esp_timer_get_time()-capture_started)/1000),
             _control_view.snapshot_ms,_control_view.blur_ms,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    #endif
    lv_obj_set_y(_control_view.root,0);
    lv_obj_remove_flag(_control_view.root,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(_control_view.root,-1);
    watch_control_view_detail(&_control_view,-1,"","","",0);
    watch_control_view_transition(&_control_view,1024);
    lv_obj_scroll_to_y(_control_view.list,0,LV_ANIM_OFF);
    refreshControls();
    // Present the complete control center in one refresh. No intermediate
    // progress values, sprite-cache preparation, fade or movement animation.
    if(!_screen_on)setScreenOn(true);
    ESP_LOGI(TAG,"Opened on %s",pcTaskGetName(nullptr));
}
void WatchShell::closeControls()
{
    if(!_controls_visible)return;
    finishControls();
}
void WatchShell::finishControls()
{
    _torch_mode=0;_touch_locked=false;
    watch_control_view_torch(&_control_view,0);
    watch_control_view_lock(&_control_view,false);
    watch_control_view_toast(&_control_view,nullptr);
    watch_control_view_detail(&_control_view,-1,"","","",0);
    lv_obj_add_flag(_control_view.root,LV_OBJ_FLAG_HIDDEN);
    watch_control_view_transition(&_control_view,1024);
    watch_control_view_release(&_control_view);
    _controls_visible=false;
    if(_screen_on)bsp_display_brightness_set(_normal_brightness);
    if(auto *g=_phone->getManager().getGesture())g->setInputSuspended(_launcher_open);
    if(_screen_on&&_gif&&_face_visible) {
        lv_gif_resume(_gif);
        #if WATCH_DEBUG_ENABLED
        _perf_started_us=esp_timer_get_time();
        _render_started_us=_render_total_us=_render_max_us=0;
        _render_count=0;
        #endif
    }
}
void WatchShell::controlCallback(void *context,watch_control_action_t action,int value)
{
    static_cast<WatchShell *>(context)->controlAction(action,value);
}
void WatchShell::controlAction(watch_control_action_t action,int value)
{
    if(_touch_locked)return;
    if(action!=WC_SLIDER||value>=1000)board_keys_haptic_request(30);
    system_status::Snapshot state;
    const bool valid=system_status::get_snapshot(state);
    switch(action) {
    case WC_CLOSE: closeControls();return;
    case WC_BACK:
        watch_control_view_detail(&_control_view,-1,"","","",0);return;
    case WC_WIFI:
        if(!valid){controlToast("Wi-Fi 状态服务不可用");break;}
        if(queueControl(action,!state.wifi_enabled))controlToast("正在切换 Wi-Fi");
        break;
    case WC_AIRPLANE:
        if(!valid){controlToast("Wi-Fi 状态服务不可用");break;}
        if(!_control_busy&&!_airplane)_airplane_restore_wifi=state.wifi_enabled;
        if(queueControl(action,_airplane?_airplane_restore_wifi:false))controlToast("正在切换飞行模式");
        break;
    case WC_MUTE: queueControl(action,!bsp_extra_codec_user_mute_get());break;
    case WC_BATTERY: case WC_BRIGHTNESS: case WC_VOLUME: case WC_TIMER:
        _control_view.detail_kind=action;break;
    case WC_SLIDER: {
        const bool released=value>=1000;
        value%=1000;
        char text[16];std::snprintf(text,sizeof(text),"%d%%",value);
        lv_label_set_text(_control_view.detail_value,text);
        lv_obj_align(_control_view.detail_value,LV_ALIGN_TOP_MID,0,146);
        if(_control_view.detail_kind==WC_BRIGHTNESS) {
            const int brightness=std::clamp(value,10,100);
            if(bsp_display_brightness_set(brightness)==ESP_OK)_normal_brightness=brightness;
            else controlToast("亮度设置失败");
        } else if(_control_view.detail_kind==WC_VOLUME&&released)queueControl(WC_VOLUME,std::clamp(value,0,100));
        return;
    }
    case WC_TORCH: _torch_mode=1;watch_control_view_torch(&_control_view,1);bsp_display_brightness_set(100);break;
    case WC_TORCH_NEXT: _torch_mode=_torch_mode==1?2:1;watch_control_view_torch(&_control_view,_torch_mode);break;
    case WC_WRIST: _raise_to_wake=!_raise_to_wake;break;
    case WC_HAPTIC: board_keys_set_haptic_enabled(!board_keys_haptic_enabled());break;
    case WC_KEEP_AWAKE: _keep_awake=!_keep_awake;break;
    case WC_LOCK: _touch_locked=true;watch_control_view_lock(&_control_view,true);break;
    case WC_SCREEN_OFF: {
        // Manual sleep is authoritative even with "stay awake" enabled.
        // Keep the current prepared frame; do not show a background-only wake.
        closeControls();setScreenOn(false);return;
    }
    case WC_TIMER_1: case WC_TIMER_3: case WC_TIMER_5: {
        const int minutes=action==WC_TIMER_1?1:action==WC_TIMER_3?3:5;
        _countdown_end_us=esp_timer_get_time()+minutes*60000000LL;
        controlToast("倒计时已开始");break;
    }
    case WC_TIMER_CANCEL: _countdown_end_us=0;controlToast("倒计时已取消");break;
    case WC_SETTINGS:
        for(int id=base::App::APP_ID_MIN;id<base::App::APP_ID_MIN+64;++id) {
            auto *app=_phone->getManager().getInstalledApp(id);
            if(app&&std::strcmp(app->getName(),"Settings")==0) {
                closeControls();
                launchApp(id);return;
            }
        }
        controlToast("设置应用不可用");break;
    default:break;
    }
    refreshControls();
}
void WatchShell::controlTick()
{
    if(_control_result_ready.exchange(false)) {
        const auto command=_control_pending;
        const esp_err_t result=static_cast<esp_err_t>(_control_result);
        _control_busy=false;
        if(result==ESP_OK) {
            if(command.action==WC_AIRPLANE)_airplane=!_airplane;
            if(command.action==WC_WIFI&&command.value)_airplane=false;
            if(command.action!=WC_TIMER)controlToast("已更新");
        } else {
            ESP_LOGW(TAG,"Control %d failed: %s",command.action,esp_err_to_name(result));
            controlToast("操作失败，请重试");
        }
        refreshControls();
    }
    const int64_t now=esp_timer_get_time();
    if(_toast_until_us&&now>=_toast_until_us&&_control_view.root) {
        watch_control_view_toast(&_control_view,nullptr);_toast_until_us=0;
    }
    if(_countdown_end_us&&now>=_countdown_end_us) {
        _countdown_end_us=0;
        _timer_alarm_pending=true;
        if(!_controls_visible)toggleControls();
        if(!_screen_on)setScreenOn(true);
        watch_control_view_detail(&_control_view,WC_BATTERY,"倒计时","时间到","按 PWR 返回",0);
        // The result remains visible until dismissed, rather than overwritten
        // by the battery detail refresh on the next status tick.
        _control_view.detail_kind=-2;
    }
    if(_timer_alarm_pending&&!_control_busy&&_control_queue) {
        if(queueControl(WC_TIMER,0))_timer_alarm_pending=false;
    }
}
void WatchShell::refreshControls()
{
    if(!_controls_visible||!_control_view.root)return;
    auto *v=&_control_view;
    system_status::Snapshot state;
    const bool valid=system_status::get_snapshot(state);
    watch_control_view_status(v,valid&&state.wifi_connected);
    if(_airplane&&state.wifi_enabled)_airplane=false;
    watch_control_view_tile(v,WC_WIFI,valid&&state.wifi_enabled,
                           !valid?"Wi-Fi 不可用":!state.wifi_enabled?"Wi-Fi 已关闭":state.wifi_connected?"Wi-Fi 已连接":"Wi-Fi 未连接");
    char percent[16]="--%";
    const bool battery=valid&&state.battery_valid&&state.battery_present&&state.battery_percent>=0;
    if(battery)std::snprintf(percent,sizeof(percent),"%d%%",state.battery_percent);
    if(std::strcmp(lv_label_get_text(v->battery),percent))lv_label_set_text(v->battery,percent);
    watch_control_view_tile(v,WC_BATTERY,false,!valid||!state.battery_valid?"电量读取中":!state.battery_present?"未接电池":state.charging?"正在充电":"电池电量");
    const bool muted=bsp_extra_codec_user_mute_get();
    watch_control_view_tile(v,WC_MUTE,muted,muted?"静音已开启":"静音");
    watch_control_view_tile(v,WC_WRIST,_raise_to_wake,_raise_to_wake?"抬腕亮屏 开":"抬腕亮屏 关");
    watch_control_view_tile(v,WC_HAPTIC,board_keys_haptic_enabled(),board_keys_haptic_enabled()?"振动 开":"振动 关");
    watch_control_view_tile(v,WC_KEEP_AWAKE,_keep_awake,_keep_awake?"保持亮屏 开":"保持亮屏");
    watch_control_view_tile(v,WC_AIRPLANE,_airplane,nullptr);
    char brightness[24],volume[24];
    std::snprintf(brightness,sizeof(brightness),"亮度 %d%%",_normal_brightness);
    std::snprintf(volume,sizeof(volume),"音量 %d%%",bsp_extra_codec_volume_get());
    watch_control_view_tile(v,WC_BRIGHTNESS,false,brightness);
    watch_control_view_tile(v,WC_VOLUME,false,volume);
    const long long seconds=_countdown_end_us?std::max(0LL,static_cast<long long>((_countdown_end_us-esp_timer_get_time()+999999)/1000000)):0;
    char countdown[32];std::snprintf(countdown,sizeof(countdown),"%02lld:%02lld",seconds/60,seconds%60);
    watch_control_view_tile(v,WC_TIMER,_countdown_end_us!=0,_countdown_end_us?countdown:"倒计时");
    const int detail=v->detail_kind;
    if(detail==WC_BATTERY) {
        const char *body=!valid||!state.battery_valid?"暂时无法读取电池\n不会显示估算电量":!state.battery_present?"当前未检测到电池\nUSB 供电不等于电池满电":state.charging?"电池正在充电":"来自 AXP2101 的实时电量";
        watch_control_view_detail(v,detail,"电池电量",percent,body,0);
    } else if(detail==WC_BRIGHTNESS||detail==WC_VOLUME) {
        const int value=lv_obj_has_state(v->slider,LV_STATE_PRESSED)?lv_slider_get_value(v->slider):detail==WC_BRIGHTNESS?_normal_brightness:bsp_extra_codec_volume_get();
        char text[16];std::snprintf(text,sizeof(text),"%d%%",value);
        watch_control_view_detail(v,detail,detail==WC_BRIGHTNESS?"屏幕亮度":"扬声器音量",text,
                                 detail==WC_BRIGHTNESS?"滑动调节\n亮度会保留到下次亮屏":muted?"静音已开启\n调节音量不会解除静音":"滑动调节",value);
    } else if(detail==WC_TIMER)watch_control_view_detail(v,detail,"倒计时",_countdown_end_us?countdown:"选择时长",board_keys_haptic_enabled()?"结束后屏幕与振动提醒":"结束后屏幕提醒",0);
}
} // namespace brookesia::watch_ui
