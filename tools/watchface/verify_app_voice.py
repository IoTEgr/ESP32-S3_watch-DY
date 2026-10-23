"""Native checks of production in-app wake, exit and continuous-dialogue policy.

Codec/network operations use adapters. This does not measure real microphone
recognition, display frame rate or device power consumption.
"""
import re
import subprocess
from verify_boot_voice import function
from render_lvgl import ROOT, CLANG, LLD, VC, SDK

BUILD=ROOT/'build/app-voice-host'
HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define ESP_UTILS_LOGI(...) ((void)0)
#define ESP_UTILS_LOGW(...) ((void)0)
#define ESP_UTILS_LOGD(...) ((void)0)
#define ESP_UTILS_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
typedef uint32_t TickType_t;
static TickType_t tick;
static TickType_t xTaskGetTickCount(){return tick;}
template<class T>struct Flag {
 T value;Flag(T v):value(v){} T load()const{return value;}void store(T v){value=v;}
 T exchange(T v){T old=value;value=v;return old;}
};
enum {ESP_OK=0,ESP_FAIL=1,ESP_ERR_INVALID_STATE=2,pdTRUE=1,portMAX_DELAY=0,RETRY_INTERVAL_MS=5000};
typedef int esp_err_t;
static int xSemaphoreTake(int,unsigned){return pdTRUE;}
static void xSemaphoreGive(int){}
static void xQueueReset(int){}
struct XiaozhiClient {
 enum class AbortReason {WakeWordDetected};
 unsigned aborts=0;esp_err_t result=ESP_OK;
 esp_err_t sendAbortSpeaking(AbortReason){++aborts;return result;}
};
struct Processor {
 bool wake=false,voice=false;unsigned wake_calls=0;
 bool isReady(){return true;}
 bool enableWakeWord(bool enabled){wake=enabled;++wake_calls;return true;}
 bool enableVoiceProcessing(bool enabled){voice=enabled;return true;}
};
struct XiaozhiApp {
 __STATES__
 enum class ListenMode {None,AutoStop};
 enum class ChannelState {Closed,Open};
 enum class ControlEventType {WakeWord};
 struct ControlEvent {ControlEventType type=ControlEventType::WakeWord;char text[512]="hello";};
 Flag<bool> _visible{false},_voice_screen_on{true},_voice_screen_changed{false},_shutdown{false};
 Flag<bool> _capture_enabled{false},_voice_enabled{false},_uplink_ready{false};
 Flag<bool> _network_ready{true},_chat_started{true},_input_resampler_reset_requested{false};
 Flag<TickType_t> _uplink_started_tick{0};Flag<unsigned> _pcm_frames_dropped{0};
 Flag<State> _state{State::Ready};Flag<ChannelState> _channel_state{ChannelState::Closed};
 XiaozhiClient client;Flag<XiaozhiClient *> _protocol{&client};
 Processor processor;Processor *_audio_processor=&processor;
 int _pcm_queue=1,_audio_tx_mutex=1;
 bool _continuous_conversation=false,_button_wake_pending=false,_transport_connected=true;
 bool _tts_active=false,release_ok=true,listen_ok=true;
 ListenMode _listen_mode=ListenMode::None;
 TickType_t _next_listen_tick=0;
 unsigned releases=0,listens=0,closed=0,destroyed=0,visible_handlers=0;
 void setState(State state){_state.store(state);}
 void setError(int,const char *){_state.store(State::Error);}
 void schedulePrepare(unsigned){}
 void stopActivationPrompt(){} void stopUplink(bool){}
 void closeChannel(bool){++closed;_listen_mode=ListenMode::None;setAudioModes(false,false);}
 void destroyChat(){++destroyed;_chat_started.store(false);_transport_connected=false;}
 bool shutdownAudio(){++releases;setAudioModes(false,false);return release_ok;}
 void handleAppVisible(){++visible_handlers;setState(State::Ready);}
 bool startListening(ListenMode mode,const char *word=nullptr){
  assert(mode==ListenMode::AutoStop);++listens;
  if(listen_ok){setState(State::Listening);_continuous_conversation=true;}return listen_ok;}
 void handleWakeForTest(const ControlEvent &);
 void handleAppHidden(bool);void setVoiceScreenOn(bool);void processVoiceScreenState();
 bool setAudioModes(bool,bool);bool enterReadyAudioMode();void processContinuousListening();
 static bool tickReached(TickType_t,TickType_t);
};
__METHODS__
void XiaozhiApp::handleWakeForTest(const ControlEvent &event){switch(event.type){
__WAKE_CASE__
}}
static void wake(){
 {XiaozhiApp a;assert(a.enterReadyAudioMode());assert(!a.processor.wake && !a._capture_enabled.load());
  a.handleWakeForTest({});assert(!a.listens && !a.processor.wake);}
 {XiaozhiApp a;a._visible.store(true);assert(a.enterReadyAudioMode());assert(a.processor.wake);
  a.handleWakeForTest({});assert(a.listens==1 && a._continuous_conversation);}
 {XiaozhiApp a;a._visible.store(true);a._state.store(XiaozhiApp::State::Speaking);
  a._channel_state.store(XiaozhiApp::ChannelState::Open);a.handleWakeForTest({});
  assert(a.client.aborts==1 && a.listens==1);}
 {XiaozhiApp a;a._visible.store(true);a._voice_screen_on.store(false);
  assert(!a.setAudioModes(true,true));assert(!a.processor.wake && !a.processor.voice);
  a.handleWakeForTest({});assert(!a.listens && !a.client.aborts);}
 puts("APP_WAKE=PASS (no hidden capture, in-app wake/listening, interrupt spoken reply, screen-off rejects stale detections)");
}
static void lifecycle(){
 for(unsigned i=0;i<200;++i){
  XiaozhiApp a;a._visible.store(true);a.setAudioModes(true,true);
  a._continuous_conversation=true;a._button_wake_pending=true;
  a.handleAppHidden(true);assert(a.releases==1 && a.closed==1 && a.destroyed==1);
  assert(!a._visible.load() && !a._continuous_conversation && !a._button_wake_pending);
  assert(!a._capture_enabled.load() && !a._voice_enabled.load() && !a.processor.wake);
  a.setVoiceScreenOn(false);a.processVoiceScreenState();a.setVoiceScreenOn(true);a.processVoiceScreenState();
  a.processContinuousListening();a.handleWakeForTest({});
  assert(a.releases==1 && !a.visible_handlers && !a.listens);
 }
 {XiaozhiApp a;a._visible.store(true);a._continuous_conversation=true;
  a.setAudioModes(true,true);a._uplink_ready.store(true);a.setVoiceScreenOn(false);
  assert(!a._capture_enabled.load() && !a._voice_enabled.load() && !a._uplink_ready.load());
  a.release_ok=false;a.processVoiceScreenState();assert(a._voice_screen_changed.load());
  a.release_ok=true;a.processVoiceScreenState();assert(!a._voice_screen_changed.load());
  assert(!a._continuous_conversation);a.setVoiceScreenOn(true);a.processVoiceScreenState();
  assert(a.visible_handlers==1 && !a._continuous_conversation);}
 puts("APP_EXIT=PASS (200 exits release audio, screen changes outside app stay idle, foreground screen-off cleanup, release retry)");
}
static void conversation(){
 XiaozhiApp a;a._visible.store(true);a._continuous_conversation=true;tick=20000;
 for(unsigned turn=0;turn<20;++turn){
  a.processContinuousListening();assert(a.listens==turn+1);a.processContinuousListening();assert(a.listens==turn+1);
  a.setState(XiaozhiApp::State::Speaking);tick+=2000;a.processContinuousListening();assert(a.listens==turn+1);
  a.setState(XiaozhiApp::State::Ready);a._transport_connected=false;
  a.processContinuousListening();assert(a.listens==turn+1);a._transport_connected=true;
 }
 a._visible.store(false);a.processContinuousListening();assert(a.listens==20);
 a._visible.store(true);a._voice_screen_on.store(false);a.processContinuousListening();assert(a.listens==20);
 a._voice_screen_on.store(true);a._continuous_conversation=false;a.processContinuousListening();assert(a.listens==20);
 puts("APP_CONVERSATION=PASS (20 turns, transport reconnect, no hidden/dark continuation, explicit stop)");
}
int main(){wake();lifecycle();conversation();}
'''

def main():
    app=(ROOT/'components/XiaozhiApp/XiaozhiApp.cpp').read_text(encoding='utf-8')
    header=(ROOT/'components/XiaozhiApp/XiaozhiApp.hpp').read_text(encoding='utf-8')
    shell=(ROOT/'components/watch_shell/watch_shell.cpp').read_text(encoding='utf-8')
    launcher=(ROOT/'components/watch_shell/watch_launcher.cpp').read_text(encoding='utf-8')
    for removed in ['serviceBackgroundWake','handleGlobalWakeWord','setGlobalWakeEnabled',
                    'takeGlobalVoiceWake','BackgroundWakeChanged','syncGlobalVoiceWake','_global_wake_enabled']:
        assert removed not in app+header+shell+launcher, removed
    for signature in ['bool XiaozhiApp::ensureAudioReady()', 'bool XiaozhiApp::startListening(',
                      'void XiaozhiApp::inputAudio()', 'void XiaozhiApp::encodeAudio()']:
        body=function(app,signature)
        assert '!_visible.load() || !_voice_screen_on.load()' in body
    hidden=function(app,'void XiaozhiApp::handleAppHidden(')
    assert 'shutdownAudio();' in hidden and 'destroyChat();' in hidden
    assert '_continuous_conversation = auto_stop_mode;' in function(app,'bool XiaozhiApp::startListening(')
    methods=''.join(function(app,s) for s in [
        'void XiaozhiApp::handleAppHidden(', 'void XiaozhiApp::setVoiceScreenOn(',
        'void XiaozhiApp::processVoiceScreenState()', 'void XiaozhiApp::processContinuousListening()',
        'bool XiaozhiApp::setAudioModes(', 'bool XiaozhiApp::enterReadyAudioMode()', 'bool XiaozhiApp::tickReached('])
    wake_case=app[app.index('    case ControlEventType::WakeWord:'):app.index('    case ControlEventType::TransportConnected:')]
    states=re.search(r'enum class State : uint8_t \{.*?\};',header,re.S).group()
    source=HARNESS.replace('__METHODS__',methods).replace('__WAKE_CASE__',wake_case).replace('__STATES__',states)
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
    (ROOT/'output/watchface/app-voice-verification.txt').write_text(result.stdout,encoding='utf-8')

if __name__=='__main__':main()
