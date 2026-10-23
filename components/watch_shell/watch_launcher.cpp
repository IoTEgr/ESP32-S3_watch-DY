#include "watch_shell.hpp"
#include <cstring>
#include "esp_brookesia.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "XiaozhiApp.hpp"
using namespace esp_brookesia;
using namespace esp_brookesia::systems;
namespace brookesia::watch_ui {
namespace { constexpr const char *TAG="WatchLauncher"; }
void WatchShell::createGrid()
{
    watch_launcher_app_t apps[WATCH_LAUNCHER_MAX_APPS]={};
    watch_launcher_app_t favorites[WATCH_LAUNCHER_FAVORITES];
    const char *names[WATCH_LAUNCHER_FAVORITES]={"MusicPlayer","Settings","Gallery","DrawPanel","Recorder","AIChats"};
    for(auto &item:favorites)item={-1,nullptr};
    unsigned count=0;
    for(int id=base::App::APP_ID_MIN;id<base::App::APP_ID_MIN+64;++id) {
        auto *app=_phone->getManager().getInstalledApp(id);
        if(!app)continue;
        watch_launcher_app_t item={id,app->getLauncherIcon().resource};
        apps[count++]=item;
        for(unsigned n=0;n<WATCH_LAUNCHER_FAVORITES;++n)if(std::strcmp(app->getName(),names[n])==0)favorites[n]=item;
    }
    _launcher_view=static_cast<watch_launcher_view_t *>(heap_caps_calloc(
        1,sizeof(watch_launcher_view_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!_launcher_view)return;
    if(!watch_launcher_view_create(lv_layer_sys(),_launcher_view,favorites,apps,count,launcherCallback,this)) {
        heap_caps_free(_launcher_view);_launcher_view=nullptr;return;
    }
    for(unsigned n=0;n<WATCH_LAUNCHER_FAVORITES;++n)if(favorites[n].id<0)ESP_LOGW(TAG,"Favorite not installed: %s",names[n]);
    ESP_LOGI(TAG,"Ready: ChatGPT at center, 5 surrounding apps, %u installed apps",count);
}
void WatchShell::showFace()
{
    if(!_face)return;
    if(_launcher_view) {
        watch_launcher_view_hide(_launcher_view);
        watch_control_view_release(&_launcher_view->backdrop);
    }
    _launcher_open=false;_return_to_launcher=false;_face_visible=true;
    lv_obj_remove_flag(_face,LV_OBJ_FLAG_HIDDEN);lv_obj_move_to_index(_face,-1);
    if(!_gif)startRandomGif();
    else if(_screen_on&&!_controls_visible)lv_gif_resume(_gif);
    if(auto *gesture=_phone->getManager().getGesture())gesture->setInputSuspended(false);
    #if WATCH_DEBUG_ENABLED
    _perf_started_us=esp_timer_get_time();
    _render_started_us=_render_total_us=_render_max_us=0;_render_count=0;
    #endif
    applyStatusBarMode();
}
void WatchShell::showGrid(bool preserve_page)
{
    if(!_launcher_view)return;
    if(_gif)lv_gif_pause(_gif);
    const bool all_apps=preserve_page && _launcher_view->all_apps;
    auto *backdrop=&_launcher_view->backdrop;
    if(!backdrop->blur_pixels) {
        if(_face) {
            lv_obj_remove_flag(_face,LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_to_index(_face,-1);
        }
        #if WATCH_DEBUG_ENABLED
        const int64_t started=esp_timer_get_time();
        #endif
        [[maybe_unused]] const bool ready=watch_control_view_capture(backdrop,lv_screen_active());
        #if WATCH_DEBUG_ENABLED
        ESP_LOGI(TAG,"Glass: ready=%d prepare=%lldms",ready,
                 static_cast<long long>((esp_timer_get_time()-started)/1000));
        #endif
    } else watch_control_backdrop_hold(backdrop,lv_screen_active());
    _face_visible=false;_launcher_open=true;_return_to_launcher=false;
    watch_launcher_view_page(_launcher_view,all_apps,!preserve_page);
    if(auto *gesture=_phone->getManager().getGesture())gesture->setInputSuspended(true);
    applyStatusBarMode();
}
void WatchShell::launchApp(int app_id)
{
    if(!_phone->checkAppID_Valid(app_id))return;
    if(_gif)lv_gif_pause(_gif);
    if(_launcher_view)watch_launcher_view_hide(_launcher_view);
    _launcher_open=false;_face_visible=false;
    if(_face)lv_obj_add_flag(_face,LV_OBJ_FLAG_HIDDEN);
    if(auto *gesture=_phone->getManager().getGesture())gesture->setInputSuspended(false);
    base::Context::AppEventData event={.id=app_id,.type=base::Context::AppEventType::START,.data=nullptr};
    _phone->sendAppEvent(&event);
    _return_to_launcher=_phone->getManager().getActiveApp()!=nullptr;
    if(!_return_to_launcher)showGrid(true);
}
void WatchShell::launcherCallback(void *context,int app_id)
{
    auto *shell=static_cast<WatchShell *>(context);
    board_keys_haptic_request(30);
    shell->launchApp(app_id);
}
void WatchShell::wakeVoiceAssistant()
{
    // The explicit touch lock still requires PWR to unlock.
    if (_touch_locked) return;
    int ai_id=-1;
    for(int id=base::App::APP_ID_MIN;id<base::App::APP_ID_MIN+64;++id) {
        auto *app=_phone->getManager().getInstalledApp(id);
        if(app && std::strcmp(app->getName(),"AIChats")==0) {ai_id=id;break;}
    }
    if(ai_id<0) {ESP_LOGW(TAG,"BOOT voice wake: AIChats is not installed");return;}
    auto *ai=_phone->getManager().getInstalledApp(ai_id);
    if(_controls_visible)closeControls();
    if(_phone->getManager().getActiveApp()!=ai) {
        if(_phone->getManager().getActiveApp()!=nullptr) {
            // Release the previous app's audio session through its lifecycle.
            _phone->sendNavigateEvent(base::Manager::NavigateType::HOME);
            if(_phone->getManager().getActiveApp()!=nullptr)return;
        }
        launchApp(ai_id);
    }
    if(_phone->getManager().getActiveApp()!=ai) {
        ESP_LOGW(TAG,"BOOT voice wake: unable to open AIChats");return;
    }
    if(!_screen_on)setScreenOn(true);
    if(!_screen_on)return; // A failed panel wake must not start hidden capture.
    const bool queued=esp_brookesia::apps::XiaozhiApp::requestInstance()->requestVoiceWake();
    ESP_LOGI(TAG,"BOOT voice wake: request queued=%d",queued);
}
} // namespace brookesia::watch_ui
