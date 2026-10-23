"""Exercise production delayed Wi-Fi standby and STA lifecycle; no AP is contacted."""
import verify_standby_power as native
from verify_boot_voice import function
from render_lvgl import ROOT

HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "watch_power.h"
enum {ESP_OK=0,ESP_FAIL=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_WIFI_NOT_STARTED=3,
 ESP_ERR_WIFI_NOT_CONNECT=4,ESP_ERR_WIFI_SSID=5,WIFI_IF_STA=0,WIFI_MODE_STA=1};
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
using esp_event_base_t=int;
enum {WIFI_EVENT=1,IP_EVENT=2,WIFI_EVENT_STA_START=1,WIFI_EVENT_STA_CONNECTED,
 WIFI_EVENT_STA_DISCONNECTED,WIFI_EVENT_STA_STOP,IP_EVENT_STA_GOT_IP,IP_EVENT_STA_LOST_IP};
static int64_t now;
static watch_power_screen_state_t screen;
static watch_power_screen_state_t watch_power_get_screen_state(){return screen;}
static int64_t esp_timer_get_time(){return now;}
static void screen_edge(bool on){if(screen.on!=on)screen={on,now};}
struct Flag {bool value=false;bool load(){return value;}void store(bool b){value=b;}bool exchange(bool b){bool old=value;value=b;return old;}};
struct SemaphoreLock {SemaphoreLock(void *){}bool isLocked(){return true;}};
struct wifi_config_t {struct {unsigned char ssid[32],password[64];uint16_t listen_interval;unsigned other;} sta;};
static wifi_config_t saved,written;
static int get_error,set_error,start_error,stop_error;
static unsigned writes,starts,stops,config_reads;
static unsigned connections,persist_writes;
static bool persisted;
static int persist_error,connect_error;
static char order[256];static unsigned position;
static void event(char c){assert(position+1<sizeof(order));order[position++]=c;order[position]=0;}
static int esp_wifi_set_mode(int m){assert(m==WIFI_MODE_STA);event('M');return 0;}
static int esp_wifi_get_config(int i,wifi_config_t *c){assert(i==WIFI_IF_STA);event('G');++config_reads;*c=saved;return get_error;}
static int esp_wifi_set_config(int i,wifi_config_t *c){assert(i==WIFI_IF_STA);event('C');++writes;written=*c;
 if(!set_error)saved=*c;return set_error;}
static int esp_wifi_start(){event('S');++starts;return start_error;}
static int esp_wifi_stop(){++stops;return stop_error;}
static int esp_wifi_scan_stop(){return 0;}
static int esp_wifi_disconnect(){return 0;}
static int esp_wifi_connect(){++connections;return connect_error;}
struct Monitor {
 void *_api_mutex=nullptr,*_wifi_mutex=nullptr;
 Flag _running,_wifi_enabled,_wifi_started,_wifi_reconnect_requested,_wifi_event_connected,_manual_wifi_configuration;
 Flag _wifi_standby_paused;
 int _last_wifi_start_error=0,_last_wifi_connect_error=0,_snapshot_lock=0;
 struct {bool wifi_enabled=false,wifi_connected=false;int wifi_rssi=-127;} _snapshot;
 int persistWifiEnabled(bool enabled){++persist_writes;if(!persist_error)persisted=enabled;return persist_error;}
 Monitor(){_running.store(true);_wifi_enabled.store(true);}
 __START__
 __CONNECT__
 __POLICY__
 __SERVICE__
 __RECONNECT__
 __SWITCH__
 __EVENT__
};
static void reset(){memset(&saved,0,sizeof(saved));memset(&written,0,sizeof(written));
 strcpy((char *)saved.sta.ssid,"test-network");strcpy((char *)saved.sta.password,"test-password");saved.sta.other=123;
 get_error=set_error=start_error=stop_error=persist_error=connect_error=0;
 connections=persist_writes=0;persisted=true;now=10000000;screen={true,0};
 writes=starts=stops=config_reads=position=0;order[0]=0;}
int main(){
 for(unsigned cycle=0;cycle<200;++cycle){reset();Monitor m;
  assert(m.applyWifiEnabledState(true)==0 && m._wifi_started.load());
  assert(!strcmp(order,"MGCS") && writes==1 && starts==1 && saved.sta.listen_interval==WATCH_POWER_WIFI_LISTEN_INTERVAL);
  assert(!strcmp((char *)saved.sta.ssid,"test-network") && !strcmp((char *)saved.sta.password,"test-password") && saved.sta.other==123);
  assert(m.applyWifiEnabledState(true)==0 && writes==1 && config_reads==1 && starts==1);
  assert(m.applyWifiEnabledState(false)==0 && !m._wifi_started.load() && !m._wifi_reconnect_requested.load());
  position=0;order[0]=0;assert(m.applyWifiEnabledState(true)==0);
  assert(!strcmp(order,"MGS") && writes==1); /* A matching saved interval causes no extra NVS write. */
  wifi_config_t input=saved;input.sta.listen_interval=3;input.sta.other=456;
  assert(m.connectWifi(input)==0 && m._wifi_reconnect_requested.load() && !m._manual_wifi_configuration.load());
  assert(input.sta.listen_interval==3 && written.sta.listen_interval==WATCH_POWER_WIFI_LISTEN_INTERVAL && written.sta.other==456);
  set_error=ESP_FAIL;assert(m.connectWifi(input)==ESP_FAIL && !m._manual_wifi_configuration.load() && !m._wifi_reconnect_requested.load());
 }
 for(unsigned fault=0;fault<3;++fault){reset();Monitor m;
  if(fault==0)get_error=ESP_FAIL;if(fault==1)set_error=ESP_FAIL;if(fault==2)start_error=ESP_FAIL;
  assert(m.applyWifiEnabledState(true)==ESP_FAIL && !m._wifi_started.load());
  get_error=set_error=start_error=0;position=0;order[0]=0;
  assert(m.applyWifiEnabledState(true)==0 && m._wifi_started.load());}
 {reset();Monitor m;saved.sta.ssid[0]=0;
 assert(m.applyWifiEnabledState(true)==0 && writes==0 && starts==1);} /* New network can be entered later. */
 puts("WIFI_STANDBY_CONFIG=PASS (200 cycles, listen interval before connection, preserved credentials/fields, saved configuration no redundant writes, toggle/reconnect, new credentials, error propagation/retry, empty SSID)");
 for(unsigned cycle=0;cycle<200;++cycle){reset();Monitor m;
  assert(m.serviceWifiState() && starts==1 && connections==1);
  screen_edge(false);now+=59999999;
  assert(m.serviceWifiState() && stops==0); /* No early disconnect. */
  ++now;assert(!m.serviceWifiState() && stops==1 && !m._wifi_started.load());
  assert(m._wifi_enabled.load() && persisted && !persist_writes); /* No NVS write or user-switch change. */
  Monitor::eventHandler(&m,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr);
  Monitor::eventHandler(&m,WIFI_EVENT,WIFI_EVENT_STA_CONNECTED,nullptr);
  Monitor::eventHandler(&m,IP_EVENT,IP_EVENT_STA_GOT_IP,nullptr);
  assert(!m._wifi_reconnect_requested.load() && !m._wifi_event_connected.load());
  assert(!m.serviceWifiState() && connections==1 && stops==1);
  screen_edge(true);now+=2999999;assert(!m.serviceWifiState() && starts==1);
  /* Even a brief off/on between worker polls must restart the 3s deadline. */
  screen_edge(false);++now;screen_edge(true);now+=2999999;
  assert(!m.serviceWifiState() && starts==1);++now;
  start_error=ESP_FAIL;assert(m.serviceWifiState() && !m._wifi_started.load());
  assert(connections==1);start_error=0;
  assert(m.serviceWifiState() && m._wifi_started.load() && connections==2);
  assert(m.setWifiEnabled(false)==0 && !persisted && persist_writes==1);
  unsigned starts_before=starts;screen_edge(false);now+=60000000;assert(!m.serviceWifiState());
  screen_edge(true);now+=3000000;assert(!m.serviceWifiState() && starts==starts_before);
  /* A manual OFF survives auto-resume. An enabled switch during pause waits. */
  screen_edge(false);now+=60000000;assert(!m.serviceWifiState());
  assert(m.setWifiEnabled(true)==0 && persisted && starts==starts_before);
  screen_edge(true);now+=3000000;assert(m.serviceWifiState() && starts==starts_before+1);
  screen_edge(false);now+=60000000;stop_error=ESP_FAIL;
  unsigned connections_before=connections;
  assert(!m.serviceWifiState() && m._wifi_started.load());
  Monitor::eventHandler(&m,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr);
  assert(!m._wifi_reconnect_requested.load() && connections==connections_before);
  stop_error=0;assert(!m.serviceWifiState() && !m._wifi_started.load());
 }
 reset();Monitor m;m.serviceWifiState();screen_edge(false);now+=59000000;m.serviceWifiState();
 screen_edge(true);now+=5000000;assert(m.serviceWifiState() && stops==0); /* Short dark period never drops Wi-Fi. */
 persist_error=ESP_FAIL;assert(m.setWifiEnabled(false)==ESP_FAIL && m._wifi_enabled.load());
 puts("WIFI_STANDBY_DELAY=PASS (200 cycles, 60s/3s boundaries, screen-edge deadline reset, brief wake suppression, actual stop/start/connect, manual OFF preserved, no automatic NVS writes, late events blocked, stop/start failure retry)");
}
'''

def main():
    source=(ROOT/'components/system_status/system_status.cpp').read_text(encoding='utf-8')
    code=HARNESS.replace('__START__',function(source,'    esp_err_t applyWifiEnabledState('))
    code=code.replace('__CONNECT__',function(source,'    esp_err_t connectWifi('))
    for marker,signature in [('__POLICY__','    bool wifiDesiredRunning('),
                             ('__SERVICE__','    bool serviceWifiState('),
                             ('__RECONNECT__','    void serviceWifiReconnect('),
                             ('__SWITCH__','    esp_err_t setWifiEnabled('),
                             ('__EVENT__','    static void eventHandler(')]:
        code=code.replace(marker,function(source,signature))
    assert 'const bool should_run=serviceWifiState();' in function(source,'    void taskLoop(')
    native.BUILD=ROOT/'build/wifi-standby-host';native.BUILD.mkdir(parents=True,exist_ok=True)
    (native.BUILD/'esp_err.h').write_text('typedef int esp_err_t;\n',encoding='utf-8')
    result=native.compile_run('wifi_standby',code,True)
    print(result)
    (ROOT/'output/watchface/wifi-standby-verification.txt').write_text(result,encoding='utf-8')

if __name__=='__main__':main()
