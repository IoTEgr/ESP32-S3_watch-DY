"""Check SDK gate observation without changing PM modes or sleep policy."""
import verify_standby_power as native
from render_lvgl import ROOT

MOCK=r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define DRAM_ATTR
#define IRAM_ATTR
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#define configEXPECTED_IDLE_TIME_BEFORE_SLEEP 3
#define portTICK_PERIOD_MS 1
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static int depth,core,real_sleeps,behavior,logs;
static int64_t clock_us,alarm_us=INT64_MAX;
static TickType_t forwarded_ticks;
static bool tsf_active;
static unsigned tsf_calls;
static void enter(void){assert(!depth);++depth;}
static void leave(void){assert(depth==1);--depth;}
#define portENTER_CRITICAL(p) enter()
#define portEXIT_CRITICAL(p) leave()
#define portENTER_CRITICAL_ISR(p) enter()
#define portEXIT_CRITICAL_ISR(p) leave()
static int xPortGetCoreID(void){return core;}
static int64_t esp_timer_get_time(void){return clock_us;}
static int64_t esp_timer_get_next_alarm_for_wake_up(void){return alarm_us;}
#define ESP_LOGI(...) (++logs)
'''

TEST=r'''
#include "mock_gate.h"
#include <stdio.h>
#include "components/watch_power/watch_power_gate.c"
bool __real_esp_wifi_internal_is_tsf_active(void){assert(!depth);++tsf_calls;return tsf_active;}
void __real_vApplicationSleep(TickType_t t){assert(!depth);++real_sleeps;forwarded_ticks=t;
 if(behavior==1)watch_power_gate_sleep_attempt();
 if(behavior==2){core=1-core;watch_power_gate_sleep_attempt();core=1-core;}
 if(behavior==3){watch_power_gate_stop();watch_power_gate_start();}}
static void reset(void){memset(s_core_api,0,sizeof(s_core_api));
 memset(&s_window,0,sizeof(s_window));s_generation=0;s_measuring=false;
 depth=core=real_sleeps=behavior=logs=tsf_calls=0;tsf_active=false;clock_us=0;alarm_us=INT64_MAX;}
int main(void){
 for(unsigned cycle=0;cycle<200;++cycle){reset();
  behavior=1;__wrap_vApplicationSleep(50); /* Awake calls are forwarded but excluded. */
  tsf_active=true;assert(__wrap_esp_wifi_internal_is_tsf_active());
  watch_power_gate_stats_t r;watch_power_gate_snapshot(&r);assert(!r.tickless && !r.api);
  assert(!r.wifi_tsf_checks && !r.wifi_tsf_veto && tsf_calls==1);
  real_sleeps=0;
  watch_power_gate_start();behavior=1;__wrap_vApplicationSleep(50);
  for(unsigned i=0;i<10;++i){tsf_active=i<7;assert(__wrap_esp_wifi_internal_is_tsf_active()==tsf_active);}
  assert(forwarded_ticks==50 && real_sleeps==1);
  behavior=0;alarm_us=clock_us+2000;__wrap_vApplicationSleep(5);
  /* A sleep entry on the other core must not hide this core's skipped entry. */
  behavior=2;core=1;alarm_us=INT64_MAX;__wrap_vApplicationSleep(100);core=0;
  clock_us+=1000;watch_power_gate_snapshot(&r);
  assert(r.tickless==3 && r.api==2 && r.skipped==2 && r.short_alarm==1);
  assert(r.min_ticks==5 && r.max_ticks==100);
  assert(r.wifi_tsf_checks==10 && r.wifi_tsf_veto==7 && tsf_calls==11);
  watch_power_gate_report(&r);assert(logs==1);
  watch_power_gate_stop();behavior=1;__wrap_vApplicationSleep(200);
  tsf_active=true;assert(__wrap_esp_wifi_internal_is_tsf_active());
  watch_power_gate_snapshot(&r);assert(r.tickless==3 && r.api==2 && r.max_ticks==100);
  assert(r.wifi_tsf_checks==10 && r.wifi_tsf_veto==7 && tsf_calls==12);
  watch_power_gate_start();
  behavior=3;__wrap_vApplicationSleep(500); /* No skipped count may leak across window restart. */
  clock_us+=1000;watch_power_gate_snapshot(&r);assert(!r.skipped && !r.tickless && !r.wifi_tsf_checks);
 }
 reset();watch_power_gate_start();watch_power_gate_stats_t r;
 watch_power_gate_snapshot(&r);watch_power_gate_report(&r);assert(!logs);
 puts("SLEEP_GATE=PASS (200 cycles, exact SDK and Wi-Fi TSF callback forwarding, TSF veto counts, tickless/API distinction, per-core skip detection, short alarm candidates, window restart/stop, awake exclusion, empty report guard)");
}
'''

def main():
    native.BUILD=ROOT/'build/sleep-gate-host'
    build=native.BUILD;build.mkdir(parents=True,exist_ok=True)
    (build/'mock_gate.h').write_text(MOCK,encoding='utf-8')
    for name in ['esp_attr.h','esp_log.h','esp_timer.h','freertos/FreeRTOS.h']:
        p=build/name;p.parent.mkdir(parents=True,exist_ok=True)
        p.write_text('#include "mock_gate.h"\n',encoding='utf-8')
    result=native.compile_run('sleep_gate',TEST)
    cmake=(ROOT/'components/watch_power/CMakeLists.txt').read_text(encoding='utf-8')
    assert '--wrap=esp_pm_impl_switch_mode' not in cmake
    print(result)
    (ROOT/'output/watchface/sleep-gate-verification.txt').write_text(result,encoding='utf-8')

if __name__=='__main__':main()
