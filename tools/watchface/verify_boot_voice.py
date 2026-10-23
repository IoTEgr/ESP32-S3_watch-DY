"""Native tests of production key scanning, UI routing and voice-wake policy.

Hardware, app manager and network/audio operations use adapters. The scanner,
event handoff, navigation and deferred-wake methods are extracted unchanged.
"""
import re
import subprocess
from render_lvgl import ROOT, CLANG, LLD, VC, SDK

BUILD=ROOT/'build/boot-voice-host'


def function(source, signature):
    start=source.index(signature)
    brace=source.index('{',start)
    depth=1
    end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]+'\n'


HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <cstring>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_UTILS_LOGI(...) ((void)0)
#define ESP_UTILS_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
enum {LVGL_PORT_EVENT_USER=128};
static unsigned ui_notifications;
static void lvgl_port_task_wake(int event,void *){assert(event==LVGL_PORT_EVENT_USER);++ui_notifications;}
typedef uint32_t TickType_t;
static TickType_t tick;
static TickType_t xTaskGetTickCount(){return tick;}
template<class T>struct Flag {
 T value;Flag(T v):value(v){} T load()const{return value;}void store(T v){value=v;}
 T exchange(T v){T old=value;value=v;return old;}
};
typedef int gpio_num_t;
enum board_key_t {BOARD_KEY_BOOT,BOARD_KEY_PWR,BOARD_KEY_MAX};
enum board_key_event_t {BOARD_KEY_SHORT_PRESS,BOARD_KEY_LONG_PRESS};
static int short_count,long_count,haptics;
static void pulse_haptic(){++haptics;}
static void dispatch_event(board_key_t,board_key_event_t event){
 if(event==BOARD_KEY_SHORT_PRESS)++short_count;else ++long_count;
}
__SCANNER__
static void reset_counts(){short_count=long_count=haptics=0;}
static void samples(key_state_t &key,board_key_t id,bool pressed,int milliseconds,int64_t &now){
 for(int ms=0;ms<milliseconds;ms+=10){now+=10000;scan_key(&key,id,pressed,now);}
}
static void verify_scanner(){
 key_state_t key={};int64_t now=0;
 samples(key,BOARD_KEY_BOOT,false,50,now);
 reset_counts();
 samples(key,BOARD_KEY_BOOT,true,10,now);samples(key,BOARD_KEY_BOOT,false,60,now);
 assert(!short_count && !long_count);
 samples(key,BOARD_KEY_BOOT,true,200,now);samples(key,BOARD_KEY_BOOT,false,60,now);
 assert(short_count==1 && !long_count);
 reset_counts();samples(key,BOARD_KEY_BOOT,true,5000,now);
 assert(long_count==1 && short_count==0); // Fires while held, once only.
 samples(key,BOARD_KEY_BOOT,false,100,now);
 assert(long_count==1 && short_count==0 && haptics==1);
 // A scanner delayed around release must still report exactly one long press.
 key={};key.stable_pressed=true;key.candidate_pressed=false;
 key.candidate_samples=KEY_DEBOUNCE_SAMPLES-1;
 reset_counts();scan_key(&key,BOARD_KEY_BOOT,false,BOOT_LONG_MS*1000);
 assert(long_count==1 && !short_count);
 key={};now=0;reset_counts();
 samples(key,BOARD_KEY_PWR,true,200,now);samples(key,BOARD_KEY_PWR,false,60,now);
 assert(short_count==1 && !long_count);
 reset_counts();samples(key,BOARD_KEY_PWR,true,1100,now);
 assert(!long_count);samples(key,BOARD_KEY_PWR,true,500,now);
 assert(long_count==1);samples(key,BOARD_KEY_PWR,false,80,now);assert(!short_count);
 puts("BOOT_SCAN=PASS (debounce, short taps, 0.9s hold, no release tap, one event per hold, threshold release, unchanged PWR timing)");
}

namespace esp_brookesia {namespace apps {
class XiaozhiApp {
public:
 __STATES__
 enum class ListenMode {None,Manual,AutoStop};
 enum class ControlEventType {ButtonWake};
 struct ControlEvent {ControlEventType type=ControlEventType::ButtonWake;};
 Flag<bool> _visible{true},_shutdown{false},_network_ready{true},_chat_started{true};
 Flag<bool> _voice_screen_on{true};
 Flag<State> _state{State::Ready};
 bool _transport_connected=true,_button_wake_pending=false,online=true;
 bool _continuous_conversation=false;
 void setVoiceScreenOn(bool on){_voice_screen_on.store(on);}
 TickType_t _button_wake_deadline=0;
 unsigned posted=0,listens=0,aborts=0,announcements=0,schedules=0;
 bool listen_ok=true;
 static XiaozhiApp *instance;
 static XiaozhiApp *requestInstance(){assert(instance);return instance;}
 bool postControlEvent(const ControlEvent &event){assert(event.type==ControlEventType::ButtonWake);++posted;return true;}
 bool isNetworkReady(){return online;}
 void setState(State s){_state.store(s);}
 void announceActivation(bool force){assert(force);++announcements;}
 void closeChannel(bool abort){assert(abort);++aborts;}
 void schedulePrepare(unsigned delay){assert(delay==0);++schedules;}
 void handleAppVisible(){
  if(!_chat_started.load())setState(State::Preparing);
  else if(!_transport_connected)setState(State::Connecting);
  else setState(State::Ready);
 }
 bool startListening(ListenMode mode){assert(mode==ListenMode::AutoStop);++listens;if(listen_ok)setState(State::Listening);return listen_ok;}
 static bool tickReached(TickType_t now,TickType_t target);
 bool requestVoiceWake();void handleButtonWake();void processButtonWake();
};
XiaozhiApp *XiaozhiApp::instance=nullptr;
__VOICE_METHODS__
}}
using esp_brookesia::apps::XiaozhiApp;

namespace base {
struct App {
 static constexpr int APP_ID_MIN=3;const char *name;
 const char *getName(){return name;}
};
struct Manager {enum class NavigateType {HOME};};
}
struct ManagerMock {
 base::App ai={"AIChats"},music={"MusicPlayer"};base::App *active=nullptr;
 bool ai_installed=true;
 base::App *getInstalledApp(int id){return id==13&&ai_installed?&ai:id==8?&music:nullptr;}
 base::App *getActiveApp(){return active;}
};
struct PhoneMock {
 ManagerMock manager;unsigned homes=0;
 ManagerMock &getManager(){return manager;}
 void sendNavigateEvent(base::Manager::NavigateType){++homes;manager.active=nullptr;}
};
struct Launcher {bool all_apps=false;};
static void watch_launcher_view_page(Launcher *l,bool all,bool){l->all_apps=all;}
struct lv_timer_t {void *context;};
static void *lv_timer_get_user_data(lv_timer_t *timer){return timer->context;}
class WatchShell {
public:
 PhoneMock phone;PhoneMock *_phone=&phone;Launcher launcher;Launcher *_launcher_view=&launcher;
 bool _touch_locked=false,_controls_visible=false,_screen_on=true,_face_visible=true;
 bool _launcher_open=false,_return_to_launcher=false;
 Flag<bool> _pending_boot{false},_pending_boot_long{false},_pending_pwr{false};
 Flag<int> _pending_wrist{-1};unsigned launches=0,grids=0,wakes=0;
 void launchApp(int id){++launches;phone.manager.active=phone.manager.getInstalledApp(id);_face_visible=_launcher_open=false;_return_to_launcher=true;}
 void showGrid(bool=false){++grids;_face_visible=false;_launcher_open=true;}
 void showFace(){_face_visible=true;_launcher_open=false;}
 void setScreenOn(bool on){_screen_on=on;++wakes;XiaozhiApp::requestInstance()->setVoiceScreenOn(on);}
 void closeControls(){_controls_visible=false;}
 void toggleControls(){_controls_visible=!_controls_visible;}
 void controlTick(){}void handleWrist(bool on){setScreenOn(on);}
 static void keyCallback(board_key_t,board_key_event_t,void *);
 static void inputTimerCallback(lv_timer_t *);
 void handleKey(board_key_t,board_key_event_t);void wakeVoiceAssistant();
};
__SHELL_METHODS__

static void verify_voice(){
 typedef XiaozhiApp::State State;
 {XiaozhiApp app;tick=10;
  assert(app.requestVoiceWake() && app.posted==1 && app.listens==0); // UI only enqueues.
  app.handleButtonWake();app.processButtonWake();app.processButtonWake();assert(app.listens==1);
  app.handleButtonWake();app.processButtonWake();assert(app.listens==1); // No toggle while listening.
 }
 for(unsigned scenario=0;scenario<8;++scenario){
  XiaozhiApp app;tick=100;
  app._chat_started.store(false);app._transport_connected=false;
  app.handleButtonWake();app.processButtonWake();assert(!app.listens && app._button_wake_pending);
  if(scenario==1)app._visible.store(false);
  if(scenario==2)app._network_ready.store(false);
  if(scenario==3)app._shutdown.store(true);
  if(scenario==4)app.setState(State::ActivationRequired);
  if(scenario==5)tick+=30001;
  app.processButtonWake();
  if(scenario>=1 && scenario<=5){assert(!app._button_wake_pending && !app.listens);continue;}
  app._chat_started.store(true);app._transport_connected=true;app.setState(State::Ready);
  app.processButtonWake();assert(app.listens==1 && !app._button_wake_pending);
 }
 {XiaozhiApp app;app._network_ready.store(false);app.handleButtonWake();
  assert(!app._button_wake_pending && app._state.load()==State::NetworkRequired && !app.listens);}
 {XiaozhiApp app;app.online=false;app.handleButtonWake();assert(!app._button_wake_pending);}
 {XiaozhiApp app;app.setState(State::ActivationRequired);app.handleButtonWake();
  assert(app.announcements==1 && !app._button_wake_pending);}
 const State reply_states[]={State::Speaking,State::Processing};
 for(auto state:reply_states){
  XiaozhiApp app;app.setState(state);app.handleButtonWake();app.processButtonWake();assert(app.aborts==1 && app.listens==1);
 }
 {XiaozhiApp app;app._visible.store(false);app.handleButtonWake();assert(!app._button_wake_pending);}
 {XiaozhiApp app;tick=0xfffffff0u;app._chat_started.store(false);app.handleButtonWake();
  tick+=100;app.processButtonWake();assert(app._button_wake_pending);
  tick+=30000;app.processButtonWake();assert(!app._button_wake_pending);}
 {XiaozhiApp app;app.listen_ok=false;app.handleButtonWake();app.processButtonWake();app.processButtonWake();assert(app.listens==1);}
 puts("BOOT_VOICE=PASS (enqueue only, deferred ready, offline/unbound/hidden/timeout cancellation, tick wrap, repeat hold, interrupt reply, one listening request)");
}
static void verify_routing(){
 XiaozhiApp app;XiaozhiApp::instance=&app;
 for(unsigned scene=0;scene<5;++scene){
  WatchShell shell;app.posted=0;
  if(scene==1){shell._face_visible=false;shell._launcher_open=true;}
  if(scene==2)shell.phone.manager.active=&shell.phone.manager.music;
  if(scene==3)shell._screen_on=false;
  if(scene==4)shell._controls_visible=true;
  WatchShell::keyCallback(BOARD_KEY_BOOT,BOARD_KEY_LONG_PRESS,&shell);
  assert(shell.launches==0 && app.posted==0); // Key task does no UI/network work.
  lv_timer_t timer={&shell};WatchShell::inputTimerCallback(&timer);
  assert(shell.launches==1 && app.posted==1 && shell._screen_on && !shell._controls_visible);
  assert(shell.phone.manager.active==&shell.phone.manager.ai);
  if(scene==2)assert(shell.phone.homes==1); // Old app is released first.
  WatchShell::keyCallback(BOARD_KEY_BOOT,BOARD_KEY_LONG_PRESS,&shell);
  WatchShell::inputTimerCallback(&timer);assert(shell.launches==1 && app.posted==2);
  WatchShell::keyCallback(BOARD_KEY_BOOT,BOARD_KEY_SHORT_PRESS,&shell);
  WatchShell::inputTimerCallback(&timer);assert(shell.grids==1 && !shell.phone.manager.active);
 }
 {WatchShell shell;app.posted=0;shell._touch_locked=true;shell.wakeVoiceAssistant();assert(!shell.launches && !app.posted);}
 {WatchShell shell;app.posted=0;shell.phone.manager.ai_installed=false;shell.wakeVoiceAssistant();assert(!shell.launches && !app.posted);}
 {WatchShell shell;WatchShell::keyCallback(BOARD_KEY_BOOT,BOARD_KEY_SHORT_PRESS,&shell);
  lv_timer_t timer={&shell};WatchShell::inputTimerCallback(&timer);assert(shell.grids==1 && !shell.launches);}
 {WatchShell shell;app.posted=0;shell._screen_on=false;
  lv_timer_t timer={&shell};WatchShell::inputTimerCallback(&timer);
  assert(!shell.launches && !app.posted && !shell._screen_on);}
 {WatchShell shell;app.posted=0;shell._pending_wrist.store(0);
  lv_timer_t timer={&shell};WatchShell::inputTimerCallback(&timer);
  assert(!shell.launches && !app.posted && !shell._screen_on);}
 {WatchShell shell;app.posted=0;
  lv_timer_t timer={&shell};WatchShell::inputTimerCallback(&timer);
  assert(!shell.launches && !app.posted && shell._screen_on);}
 puts("BOOT_ROUTING=PASS (face/menu/other app/screen off/controls, LVGL dispatch, audio-owner release, reuse active AI, short menu/back, touch lock)");
}
int main(){verify_scanner();verify_voice();verify_routing();}
'''


def main():
    keys=(ROOT/'components/board_keys/board_keys.c').read_text(encoding='utf-8')
    app=(ROOT/'components/XiaozhiApp/XiaozhiApp.cpp').read_text(encoding='utf-8')
    header=(ROOT/'components/XiaozhiApp/XiaozhiApp.hpp').read_text(encoding='utf-8')
    shell=(ROOT/'components/watch_shell/watch_shell.cpp').read_text(encoding='utf-8')
    launcher=(ROOT/'components/watch_shell/watch_launcher.cpp').read_text(encoding='utf-8')
    constants='\n'.join(re.findall(r'^#define (?:KEY_\w+|BOOT_LONG_MS)\s+\d+',keys,re.M))
    state=keys[keys.index('typedef struct {'):keys.index('} key_state_t;')+len('} key_state_t;')]
    scanner=constants+'\n'+state+'\n'+function(keys,'static void scan_key(')
    states=re.search(r'enum class State : uint8_t \{.*?\};',header,re.S).group()
    voice=''.join(function(app,s) for s in ['bool XiaozhiApp::requestVoiceWake()',
        'void XiaozhiApp::handleButtonWake()','void XiaozhiApp::processButtonWake()',
        'bool XiaozhiApp::tickReached('])
    routing=''.join(function(shell,s) for s in ['void WatchShell::keyCallback(',
        'void WatchShell::inputTimerCallback(','void WatchShell::handleKey('])
    routing+=function(launcher,'void WatchShell::wakeVoiceAssistant()')
    screen=function(shell,'void WatchShell::setScreenOn(')
    assert 'setVoiceScreenOn(on)' in screen and 'setVoiceScreenOn(false)' in screen
    assert 'syncGlobalVoiceWake' not in shell+launcher
    assert 'takeGlobalVoiceWake' not in shell+launcher
    # Verify lifecycle integration outside the extracted state-policy methods.
    assert '_button_wake_pending = false;' in function(app,'void XiaozhiApp::handleAppHidden(')
    assert '_button_wake_pending = false;' in function(app,'bool XiaozhiApp::init()')
    assert 'processButtonWake();' in function(app,'void XiaozhiApp::controlLoop()')
    assert 'case ControlEventType::ButtonWake:' in app
    assert re.search(r'if \(!event.flag\)\s*\{\s*_button_wake_pending = false;',app)
    source=HARNESS.replace('__SCANNER__',scanner).replace('__STATES__',states).replace('__VOICE_METHODS__',voice).replace('__SHELL_METHODS__',routing)
    BUILD.mkdir(parents=True,exist_ok=True)
    (BUILD/'test.cpp').write_text(source,encoding='utf-8')
    flags=['--target=x86_64-pc-windows-msvc','-std=c++11','-O1','-fms-extensions','-Wno-everything']
    for path in [VC/'include',SDK/'Include/shared',SDK/'Include/um']:flags+=['-I',str(path)]
    subprocess.run([str(CLANG),*flags,'-c',str(BUILD/'test.cpp'),'-o',str(BUILD/'test.obj')],check=True)
    subprocess.run([str(LLD),'-flavor','link',str(BUILD/'test.obj'),'/out:'+str(BUILD/'test.exe'),
        '/subsystem:console','/defaultlib:libcmt','/defaultlib:oldnames','/defaultlib:kernel32',
        '/libpath:'+str(VC/'lib/amd64'),'/libpath:'+str(SDK/'Lib/winv6.3/um/x64')],check=True)
    result=subprocess.run([str(BUILD/'test.exe')],capture_output=True,text=True,check=True)
    print(result.stdout)
    (ROOT/'output/watchface/boot-voice-verification.txt').write_text(result.stdout,encoding='utf-8')


if __name__=='__main__':main()
