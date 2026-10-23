"""Exercise production power policy, I2S idle and UI sleep/wake sequencing.

PM locks, panel I/O and RTOS primitives are adapters. Device current, real
Light-sleep residency and physical wake reliability require a hardware check.
"""
import subprocess
from verify_boot_voice import function
from render_lvgl import ROOT, CLANG, LLD, VC, SDK

BUILD=ROOT/'build/standby-power-host'
MOCK=r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_WIFI_NOT_INIT 4
#define portMAX_DELAY 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(expr) assert((expr)==ESP_OK)
typedef int *SemaphoreHandle_t;
typedef struct {int type,refs;bool live;} Lock;
typedef Lock *esp_pm_lock_handle_t;
typedef struct {int max_freq_mhz,min_freq_mhz;bool light_sleep_enable;} esp_pm_config_t;
enum {ESP_PM_CPU_FREQ_MAX=0,ESP_PM_NO_LIGHT_SLEEP=1};
typedef enum {WIFI_MODE_NULL,WIFI_MODE_STA,WIFI_MODE_APSTA} wifi_mode_t;
typedef enum {WIFI_PS_NONE,WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM} wifi_ps_type_t;
static bool wifi_ready;
static wifi_ps_type_t wifi_ps=WIFI_PS_MIN_MODEM;
static unsigned wifi_writes;
static int wifi_read_error,wifi_write_error;
static int64_t policy_now;
static int64_t esp_timer_get_time(void){return policy_now;}
static const char *esp_err_to_name(int e){return "mock";}
static int esp_wifi_get_mode(wifi_mode_t *m){*m=WIFI_MODE_STA;return wifi_ready?ESP_OK:ESP_ERR_WIFI_NOT_INIT;}
static int esp_wifi_get_ps(wifi_ps_type_t *ps){*ps=wifi_ps;return wifi_read_error;}
static int esp_wifi_set_ps(wifi_ps_type_t ps){++wifi_writes;if(wifi_write_error)return wifi_write_error;wifi_ps=ps;return ESP_OK;}
static Lock locks[2];static int mutex_token,created,deleted,fail_at,configure_fail;
static SemaphoreHandle_t xSemaphoreCreateMutex(void){return &mutex_token;}
static void vSemaphoreDelete(SemaphoreHandle_t s){assert(s==&mutex_token);}
static void xSemaphoreTake(SemaphoreHandle_t s,int t){assert(s);}
static void xSemaphoreGive(SemaphoreHandle_t s){assert(s);}
static int esp_pm_lock_create(int type,int arg,const char *name,esp_pm_lock_handle_t *out){
 if(++created==fail_at)return ESP_ERR_NO_MEM;
 locks[type]=(Lock){type,0,true};*out=&locks[type];return ESP_OK;}
static int esp_pm_lock_acquire(Lock *l){assert(l && l->live);++l->refs;return 0;}
static int esp_pm_lock_release(Lock *l){assert(l && l->live && l->refs==1);--l->refs;return 0;}
static int esp_pm_lock_delete(Lock *l){assert(l && l->live && !l->refs);l->live=false;++deleted;return 0;}
static int esp_pm_configure(const esp_pm_config_t *c){assert(c->min_freq_mhz==20 && c->max_freq_mhz==240 && c->light_sleep_enable);return configure_fail?ESP_FAIL:0;}
void watch_power_sleep_set_screen_on(bool on){}
void watch_power_sleep_poll(void){}
'''
POLICY=r'''
#include "mock_sdk.h"
#include "components/watch_power/watch_power.c"
static void reset_policy(void){
 memset(locks,0,sizeof(locks));s_mutex=NULL;s_cpu=s_awake=NULL;s_ready=s_locked=false;
 atomic_store(&s_screen_on,true);s_audio_users=0;created=deleted=fail_at=configure_fail=0;
 s_wifi_reported_mode=-1;s_wifi_last_error=ESP_OK;wifi_ready=false;
 s_screen_since_us=policy_now=0;
 wifi_ps=WIFI_PS_MIN_MODEM;wifi_writes=0;wifi_read_error=wifi_write_error=ESP_OK;}
static void check(bool busy){assert(locks[0].refs==(int)busy && locks[1].refs==(int)busy);}
typedef struct {bool is_enabled;} i2s_chan_info_t;
typedef struct {bool enabled;int failed;} Channel;
typedef Channel *i2s_chan_handle_t;
static Channel rx,tx;static i2s_chan_handle_t i2s_rx_chan=&rx,i2s_tx_chan=&tx;
static int disables,pa=1;
#define BSP_POWER_AMP_IO 46
static int i2s_channel_get_info(Channel *c,i2s_chan_info_t *info){if(c->failed)return ESP_FAIL;info->is_enabled=c->enabled;return 0;}
static int i2s_channel_disable(Channel *c){assert(c->enabled);c->enabled=false;++disables;return 0;}
static int gpio_set_level(int pin,int level){assert(pin==46 && !rx.enabled && !tx.enabled);pa=level;return 0;}
static void bsp_audio_update_result(int *r,int e){if(*r==0 && e)*r=e;}
__IDLE_AUDIO__
int main(void){
 for(int f=1;f<=3;++f){reset_policy();if(f<3)fail_at=f;else configure_fail=1;
  assert(watch_power_init()!=ESP_OK);assert(!s_ready && !s_mutex && !s_cpu && !s_awake);
  assert(!locks[0].live && !locks[1].live);}
 reset_policy();assert(watch_power_init()==0);check(true);assert(watch_power_init()==0 && created==2);
 wifi_ready=true;watch_power_poll();assert(wifi_ps==WIFI_PS_MIN_MODEM);
 policy_now=100;watch_power_set_screen_on(false);
 watch_power_screen_state_t screen=watch_power_get_screen_state();assert(!screen.on && screen.since_us==100);
 policy_now=200;watch_power_set_screen_on(false);
 screen=watch_power_get_screen_state();assert(!screen.on && screen.since_us==100); /* Repeated calls cannot restart a deadline. */
 policy_now=300;watch_power_set_screen_on(true);
 screen=watch_power_get_screen_state();assert(screen.on && screen.since_us==300);
 for(unsigned i=0;i<2000;++i){
  watch_power_set_screen_on(false);check(false);assert(!watch_power_is_screen_on());
  assert(wifi_ps==WIFI_PS_MAX_MODEM);
  watch_power_audio_begin();check(true);assert(wifi_ps==WIFI_PS_NONE);
  unsigned writes=wifi_writes;watch_power_audio_begin();check(true);assert(wifi_writes==writes);
  watch_power_audio_end();check(true);assert(wifi_ps==WIFI_PS_NONE && wifi_writes==writes);
  watch_power_set_screen_on(true);check(true);assert(wifi_ps==WIFI_PS_NONE);
  watch_power_audio_end();check(true);assert(wifi_ps==WIFI_PS_MIN_MODEM);
  watch_power_set_screen_on(true);check(true);watch_power_set_screen_on(false);check(false);
  watch_power_audio_end();check(false); /* Failed/unmatched release must not underflow. */
  watch_power_set_screen_on(true);check(true);
 }
 wifi_write_error=ESP_FAIL;watch_power_set_screen_on(false);check(false);assert(wifi_ps==WIFI_PS_MIN_MODEM);
 wifi_write_error=ESP_OK;watch_power_poll();assert(wifi_ps==WIFI_PS_MAX_MODEM);
 wifi_read_error=ESP_FAIL;watch_power_set_screen_on(true);check(true);assert(wifi_ps==WIFI_PS_MAX_MODEM);
 wifi_read_error=ESP_OK;watch_power_poll();assert(wifi_ps==WIFI_PS_MIN_MODEM);
 wifi_ready=false;watch_power_set_screen_on(false);check(false);
 wifi_ready=true;wifi_ps=WIFI_PS_NONE;watch_power_poll();assert(wifi_ps==WIFI_PS_MAX_MODEM);
 puts("POWER_POLICY=PASS (240MHz active/audio, 20MHz minimum plus auto-sleep allowed when idle, balanced locks, ownership handoff, init-failure cleanup, 2000 cycles)");
 puts("WIFI_POWER=PASS (MAX_MODEM dark/idle, MIN_MODEM awake, NONE during audio, immediate ownership transitions, no redundant writes, late driver init/restart, PS failure retry)");
 for(unsigned i=0;i<500;++i){rx=(Channel){true,0};tx=(Channel){true,0};pa=1;disables=0;
  assert(bsp_audio_idle_channels()==0 && disables==2 && !pa);
  assert(bsp_audio_idle_channels()==0 && disables==2); /* Retained handles, no double disable. */}
 rx=(Channel){true,0};tx=(Channel){true,1};pa=1;
 assert(bsp_audio_idle_channels()!=0 && pa==1);tx.failed=0;
 assert(bsp_audio_idle_channels()==0 && pa==0);
 puts("IDLE_AUDIO=PASS (initial unused RX and TX stop, retained handles, repeated close/resume model, failure retry, PA off after both channels stop)");
}
'''
UI=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
enum {ESP_OK=0,ESP_FAIL=1,INPUT_PERIOD_MS=20,STANDBY_INPUT_PERIOD_MS=50};
typedef int esp_err_t;
struct lv_timer_t {bool paused=false;unsigned period=15;};
struct lv_obj_t{};struct lv_display_t{};struct lv_indev_t {bool enabled=true;};
struct Gif {bool loaded=true,paused=false;};
static lv_display_t display;static lv_indev_t touch;static lv_timer_t refresh_timer,touch_timer;
static bool high_performance=true,panel=true,fail_panel=false,fail_brightness=false,voice_screen=true;
static int brightness=75,draws,prefetches;
static void watch_power_set_screen_on(bool on){high_performance=on;}
static lv_indev_t *bsp_display_get_input_dev(){return &touch;}
static lv_display_t *lv_display_get_default(){return &display;}
static lv_timer_t *lv_display_get_refr_timer(lv_display_t *){return &refresh_timer;}
static lv_timer_t *lv_indev_get_read_timer(lv_indev_t *){return &touch_timer;}
static bool lv_timer_get_paused(lv_timer_t *t){return t->paused;}
static void lv_timer_pause(lv_timer_t *t){t->paused=true;}
static void lv_timer_resume(lv_timer_t *t){t->paused=false;}
static void lv_timer_set_period(lv_timer_t *t,unsigned p){t->period=p;}
static void lv_indev_enable(lv_indev_t *i,bool enabled){i->enabled=enabled;}
static int bsp_display_brightness_get(){return brightness;}
static void bsp_display_backlight_off(){brightness=0;}
static int bsp_display_set_panel_on(bool on){if(on && fail_panel)return ESP_FAIL;panel=on;return 0;}
static int bsp_display_brightness_set(int value){assert(high_performance && panel && draws>0);
 if(fail_brightness)return ESP_FAIL;brightness=value;return 0;}
static int64_t esp_timer_get_time(){return 100000;}
static bool lv_gif_is_loaded(Gif *g){return g->loaded;}
static void lv_gif_pause(Gif *g){g->paused=true;}
static void lv_gif_resume(Gif *g){assert(brightness>0 && panel);g->paused=false;}
static void lv_obj_invalidate(lv_obj_t *){}
static lv_display_t *lv_obj_get_display(lv_obj_t *){return &display;}
static void lv_refr_now(lv_display_t *){assert(high_performance && brightness==0);++draws;}
static lv_obj_t *lv_screen_active(){static lv_obj_t o;return &o;}
static lv_obj_t *watch_launcher_view_surface(void *){return lv_screen_active();}
namespace esp_brookesia{namespace apps{
struct XiaozhiApp{static XiaozhiApp *requestInstance(){static XiaozhiApp a;return &a;}
 void setVoiceScreenOn(bool on){voice_screen=on;}};}}
struct GestureMock {bool suspended=false;void setInputSuspended(bool on){suspended=on;}};
struct ManagerMock {GestureMock gesture;GestureMock *getGesture(){return &gesture;}};
struct PhoneMock {ManagerMock manager;ManagerMock &getManager(){return manager;}};
class WatchShell {public:
 PhoneMock phone;PhoneMock *_phone=&phone;
 bool _screen_on=true,_torch_mode=false,_controls_visible=false,_face_visible=true,_launcher_open=false;
 int _normal_brightness=75;Gif gif;Gif *_gif=&gif;lv_obj_t obj;lv_obj_t *_face=&obj;
 struct {lv_obj_t *root=nullptr;} _control_view;void *_launcher_view=nullptr;
 lv_timer_t clock_timer,input_timer;lv_timer_t *_timer=&clock_timer,*_input_timer=&input_timer;
 lv_timer_t *_sleep_refr_timer=nullptr,*_sleep_touch_timer=nullptr;
 int64_t _perf_started_us=0,_render_started_us=0,_render_total_us=0,_render_max_us=0;unsigned _render_count=0;
 void refreshTime(){}void refreshStatus(){}void refreshWeather(){}
 void startRandomGif(){assert(high_performance && brightness==0);++prefetches;gif.loaded=true;gif.paused=true;}
 void setUiStandby(bool);void setScreenOn(bool);
};
__UI_METHODS__
int main(){
 for(unsigned i=0;i<200;++i){WatchShell s;high_performance=panel=true;brightness=75;draws=0;
  refresh_timer.paused=touch_timer.paused=false;touch.enabled=true;
  s.setScreenOn(false);assert(!high_performance && !panel && !brightness && !voice_screen);
  assert(refresh_timer.paused && touch_timer.paused && s.clock_timer.paused && !touch.enabled);
  assert(s.phone.manager.gesture.suspended);
  assert(!s.input_timer.paused && s.input_timer.period==50); /* Input/countdown pump remains alive. */
  s.setScreenOn(true);assert(high_performance && panel && brightness==75 && voice_screen && draws==1);
  assert(!refresh_timer.paused && !touch_timer.paused && !s.clock_timer.paused && touch.enabled);
  assert(!s.phone.manager.gesture.suspended);
  assert(s.input_timer.period==20 && !s.gif.paused);}
 for(unsigned fail=0;fail<2;++fail){WatchShell s;brightness=75;high_performance=panel=true;
  s.setScreenOn(false);fail_panel=fail==0;fail_brightness=fail==1;s.setScreenOn(true);
  assert(!s._screen_on && !high_performance && !panel && !brightness && !voice_screen && refresh_timer.paused);
  fail_panel=fail_brightness=false;s.setScreenOn(true);assert(s._screen_on && brightness==75);}
 for(unsigned page=0;page<3;++page){WatchShell s;brightness=75;high_performance=panel=true;
  s._launcher_open=page==1;s._controls_visible=page==2;
  s.setScreenOn(false);assert(s.phone.manager.gesture.suspended);
  s.setScreenOn(true);assert(s.phone.manager.gesture.suspended==(page!=0));}
 puts("STANDBY_UI=PASS (200 sleep/wake cycles, full-performance prefetch/redraw before panel-on, paused hidden refresh/touch/gesture, page-aware gesture restore, live 50ms input/countdown pump, brightness/panel failure recovery)");
}
'''

def compile_run(name,source,cpp=False):
    ext='cpp' if cpp else 'c'
    (BUILD/f'{name}.{ext}').write_text(source,encoding='utf-8')
    flags=['--target=x86_64-pc-windows-msvc','-std=c++11' if cpp else '-std=c11','-O1','-fms-extensions','-Wno-everything']
    for p in [BUILD,ROOT,ROOT/'components/watch_power/include',VC/'include',SDK/'Include/shared',SDK/'Include/um']:flags+=['-I',str(p)]
    subprocess.run([str(CLANG),*flags,'-c',str(BUILD/f'{name}.{ext}'),'-o',str(BUILD/f'{name}.obj')],check=True)
    subprocess.run([str(LLD),'-flavor','link',str(BUILD/f'{name}.obj'),'/out:'+str(BUILD/f'{name}.exe'),
        '/subsystem:console','/defaultlib:libcmt','/defaultlib:oldnames','/defaultlib:kernel32',
        '/libpath:'+str(VC/'lib/amd64'),'/libpath:'+str(SDK/'Lib/winv6.3/um/x64')],check=True)
    return subprocess.run([str(BUILD/f'{name}.exe')],capture_output=True,text=True,check=True).stdout

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    (BUILD/'mock_sdk.h').write_text(MOCK,encoding='utf-8')
    for name in ['esp_err.h','esp_log.h','esp_pm.h','esp_timer.h','esp_wifi.h','freertos/FreeRTOS.h','freertos/semphr.h']:
        p=BUILD/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "mock_sdk.h"\n',encoding='utf-8')
    bsp=(ROOT/'components/waveshare__esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c').read_text(encoding='utf-8')
    shell=(ROOT/'components/watch_shell/watch_shell.cpp').read_text(encoding='utf-8')
    policy=POLICY.replace('__IDLE_AUDIO__',function(bsp,'esp_err_t bsp_audio_idle_channels('))
    ui=''.join(function(shell,s) for s in ['void WatchShell::setUiStandby(', 'void WatchShell::setScreenOn('])
    report=''
    for debug in [0,1]:
        prefix=f'#define WATCH_DEBUG_ENABLED {debug}\n'
        report+=f'DEBUG_MODE={debug}\n'
        report+=compile_run(f'policy_{debug}',prefix+policy)
        report+=compile_run(f'ui_{debug}',prefix+UI.replace('__UI_METHODS__',ui),True)
    main_src=(ROOT/'main/main.cpp').read_text(encoding='utf-8')
    assert main_src.index('lv_tick_set_cb(lvgl_clock_ms)')<main_src.index('lvgl_port_stop()')<main_src.index('lv_timer_enable(true)')
    assert 'lvgl_port_task_wake(LVGL_PORT_EVENT_USER' in function(shell,'void WatchShell::keyCallback(')
    assert 'lvgl_port_task_wake(LVGL_PORT_EVENT_USER' in function(shell,'void WatchShell::wristCallback(')
    assert 'if(!shell->_screen_on)return;' in function(shell,'void WatchShell::timerCallback(')
    extra=(ROOT/'components/bsp_extra/src/bsp_board_extra.c').read_text(encoding='utf-8')
    assert 'bsp_extra_codec_dev_stop();' in function(extra,'esp_err_t bsp_extra_codec_init(')
    assert 'bsp_audio_idle_channels()' in function(extra,'esp_err_t bsp_extra_codec_dev_stop(')
    assert 'watch_power_audio_begin();' in function(extra,'esp_err_t bsp_extra_audio_session_acquire(')
    assert 'watch_power_audio_end();' in function(extra,'esp_err_t bsp_extra_audio_session_release(')
    cfg=(ROOT/'sdkconfig').read_text(encoding='utf-8')
    for option in ['CONFIG_PM_ENABLE=y','CONFIG_FREERTOS_USE_TICKLESS_IDLE=y',
                   '# CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP is not set', 'CONFIG_PM_SLP_DISABLE_GPIO=y']:
        assert option in cfg,option
    bsp=(ROOT/'components/waveshare__esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c').read_text(encoding='utf-8')
    preserve=function(bsp,'esp_err_t bsp_sleep_preserve_io(')
    assert 'GPIO_IS_VALID_GPIO(pin)' in preserve and '(pin>21 && pin<38)' in preserve
    assert 'gpio_sleep_sel_dis((gpio_num_t)pin)' in preserve
    assert 'bsp_sleep_preserve_io()' in (ROOT/'main/main.cpp').read_text(encoding='utf-8')
    print(report)
    (ROOT/'output/watchface/standby-power-verification.txt').write_text(report,encoding='utf-8')

if __name__=='__main__':main()
