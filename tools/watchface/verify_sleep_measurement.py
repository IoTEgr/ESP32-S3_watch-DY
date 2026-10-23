"""Check production sleep measurement, rejected calls and offline report retention.

The SDK sleep API, time, USB and spinlocks are adapters. This is not a current
measurement and does not prove sleep residency on the watch.
"""
import verify_standby_power as native
from render_lvgl import ROOT

MOCK=r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include "components/watch_power/watch_power_gate.h"
#define DRAM_ATTR
#define IRAM_ATTR
typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=1,ESP_PM_CPU_FREQ_MAX,ESP_PM_APB_FREQ_MAX,ESP_PM_NO_LIGHT_SLEEP,ESP_PM_LOCK_MAX};
enum {ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION=0x102,ESP_ERR_SLEEP_REJECT=0x103,ESP_ERR_NOT_ALLOWED=0x10d};
typedef struct {size_t created,acquired;} esp_pm_lock_stats_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static int critical;
static void enter(void){assert(!critical);++critical;}
static void leave(void){assert(critical==1);--critical;}
#define portENTER_CRITICAL(p) enter()
#define portEXIT_CRITICAL(p) leave()
#define portENTER_CRITICAL_ISR(p) enter()
#define portEXIT_CRITICAL_ISR(p) leave()
static int64_t now,call_us;
static int real_result;
static bool usb;
static unsigned held_locks,logs,reported_ratio,reported_samples,reported_min,reported_max,reported_replay;
static uint64_t reported_off,reported_sleep,reported_ok,reported_rejected;
static unsigned reported_usb_polls,reported_battery_polls,reported_stats_errors,reported_usb_samples,reported_usb_min,reported_usb_max,reported_error;
static unsigned held_apb,reported_apb_min,reported_apb_max;
static uint64_t reported_short,reported_wake,reported_other;
static int stats_result;
static void watch_power_tasks_poll(bool off,bool usb){}
static void watch_power_tasks_report(void){}
void watch_power_gate_start(void){}
void watch_power_gate_stop(void){}
void watch_power_gate_snapshot(watch_power_gate_stats_t *out){memset(out,0,sizeof(*out));}
void watch_power_gate_sleep_attempt(void){}
void watch_power_gate_report(const watch_power_gate_stats_t *out){}
static int64_t esp_timer_get_time(void){return now;}
static bool usb_serial_jtag_is_connected(void){return usb;}
static int esp_pm_get_lock_stats_all(esp_pm_lock_stats_t *out){assert(!critical);
 memset(out,0,sizeof(*out)*ESP_PM_LOCK_MAX);out[ESP_PM_NO_LIGHT_SLEEP].acquired=held_locks;
 out[ESP_PM_APB_FREQ_MAX].acquired=held_apb;return stats_result;}
static void capture_log(const char *tag,const char *fmt,...){assert(!critical && usb);
 assert(strcmp(tag,"PowerSleep")==0 && strstr(fmt,"sleep_est="));++logs;
 va_list a;va_start(a,fmt);reported_off=va_arg(a,unsigned long long);reported_sleep=va_arg(a,unsigned long long);
 reported_ratio=va_arg(a,unsigned);reported_ok=va_arg(a,unsigned long long);reported_rejected=va_arg(a,unsigned long long);
 reported_samples=va_arg(a,unsigned);reported_min=va_arg(a,unsigned);reported_max=va_arg(a,unsigned);
 reported_replay=va_arg(a,unsigned);
 reported_usb_polls=va_arg(a,unsigned);reported_battery_polls=va_arg(a,unsigned);reported_stats_errors=va_arg(a,unsigned);
 reported_usb_samples=va_arg(a,unsigned);reported_usb_min=va_arg(a,unsigned);reported_usb_max=va_arg(a,unsigned);
 reported_short=va_arg(a,unsigned long long);reported_wake=va_arg(a,unsigned long long);reported_other=va_arg(a,unsigned long long);
 reported_error=va_arg(a,unsigned);reported_apb_min=va_arg(a,unsigned);reported_apb_max=va_arg(a,unsigned);va_end(a);
}
#define ESP_LOGI capture_log
'''

TEST=r'''
#include "mock_sleep.h"
#include <stdio.h>
esp_err_t __real_esp_light_sleep_start(void){assert(!critical);now+=call_us;return real_result;}
#include "components/watch_power/watch_power_sleep.c"
static void reset(void){
 s_success_us=s_success_calls=s_rejected_calls=0;s_off=s_pending=s_usb_previous=false;
 s_short_calls=s_wake_reject_calls=s_other_calls=0;s_last_error=0;
 s_usb_polls=s_battery_polls=s_stats_errors=s_usb_lock_samples=s_usb_locks_max=0;s_usb_locks_min=UINT_MAX;
 s_start_short=s_start_wake_reject=s_start_other=0;
 s_off_started=0;s_start_us=s_start_calls=s_start_rejected=0;s_lock_samples=0;s_locks_min=UINT_MAX;s_locks_max=0;
 s_apb_min=UINT_MAX;s_apb_max=0;
 memset(&s_last,0,sizeof(s_last));now=call_us=real_result=critical=0;
 memset(&s_reconnect_report,0,sizeof(s_reconnect_report));s_replays=0;s_next_replay=0;
 usb=false;held_locks=held_apb=logs=0;stats_result=0;
}
static void sleep_call(int64_t us,int result){call_us=us;real_result=result;assert(__wrap_esp_light_sleep_start()==result);}
int main(void){
 for(unsigned cycle=0;cycle<200;++cycle){reset();watch_power_sleep_set_screen_on(false);
  for(unsigned i=0;i<100;++i){sleep_call(80000,ESP_OK);now+=20000;
   held_locks=i<50?2:0;watch_power_sleep_poll();}
  for(unsigned i=0;i<5;++i)sleep_call(200,ESP_FAIL);
  assert(s_success_us==8000000 && s_success_calls==100 && s_rejected_calls==5 && !logs);
  watch_power_sleep_set_screen_on(true);watch_power_sleep_poll();assert(!logs && s_pending);
  now+=1000000;usb=true;watch_power_sleep_poll();
  assert(logs==1 && reported_off==10001 && reported_sleep==8000 && reported_ratio==79);
  assert(reported_ok==100 && reported_rejected==5 && reported_samples==100 && reported_min==0 && reported_max==2);
  assert(reported_battery_polls==100 && reported_usb_polls==0 && reported_other==5 && reported_error==ESP_FAIL);
  watch_power_sleep_poll();assert(logs==1); /* No repeated log spam. */
  now+=5000000;watch_power_sleep_poll();assert(logs==2 && reported_replay==1 && reported_sleep==8000);
 }
 reset();usb=true;watch_power_sleep_poll();watch_power_sleep_set_screen_on(false);
 now+=10000000;watch_power_sleep_set_screen_on(true);watch_power_sleep_poll();
 assert(logs==1 && reported_ratio==0 && reported_ok==0 && reported_samples==0);
 reset();watch_power_sleep_set_screen_on(false);sleep_call(4000000,ESP_OK);now+=1000000;
 watch_power_sleep_set_screen_on(false); /* Idempotent off cannot reset interval. */
 watch_power_sleep_poll();usb=true;watch_power_sleep_poll();
 assert(logs==1 && reported_off==5000 && reported_sleep==4000 && reported_ratio==80 && s_off);
 watch_power_sleep_poll();assert(logs==1);
 now+=1000000;watch_power_sleep_set_screen_on(true);watch_power_sleep_poll();
 assert(logs==2 && reported_off==6000 && reported_ratio==66);
 reset();watch_power_sleep_set_screen_on(false);watch_power_sleep_set_screen_on(true);
 usb=true;watch_power_sleep_poll();assert(!logs); /* No division by zero. */
 reset();watch_power_sleep_set_screen_on(false);sleep_call(1000000,ESP_FAIL);
 watch_power_sleep_set_screen_on(true);usb=true;watch_power_sleep_poll();
 assert(logs==1 && !reported_sleep && !reported_ok && reported_rejected==1 && !reported_ratio);
 for(int i=0;i<10;++i){now+=5000000;watch_power_sleep_poll();}
 assert(logs==6); /* Bounded replay, not permanent periodic debug traffic. */
 reset();usb=true;held_locks=1;watch_power_sleep_poll();watch_power_sleep_set_screen_on(false);
 for(int i=0;i<60;++i){now+=1000000;watch_power_sleep_poll();}
 watch_power_sleep_set_screen_on(true);watch_power_sleep_poll();
 assert(logs==1 && reported_usb_polls==60 && reported_battery_polls==0 && reported_samples==0);
 assert(reported_usb_samples==60 && reported_usb_min==1 && reported_usb_max==1 && !reported_error);
 reset();watch_power_sleep_set_screen_on(false);stats_result=ESP_FAIL;now+=1000;watch_power_sleep_poll();
 stats_result=ESP_OK;watch_power_sleep_poll();
 sleep_call(10,ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION);sleep_call(10,ESP_ERR_SLEEP_REJECT);
 sleep_call(10,ESP_ERR_NOT_ALLOWED);watch_power_sleep_set_screen_on(true);usb=true;watch_power_sleep_poll();
 assert(reported_rejected==3 && reported_short==1 && reported_wake==1 && reported_other==1);
 assert(reported_error==ESP_ERR_NOT_ALLOWED && reported_stats_errors==1 && reported_battery_polls==2 && reported_samples==1);
 /* Reject categories and last error must not leak into the next off window. */
 watch_power_sleep_set_screen_on(false);sleep_call(1000,ESP_OK);watch_power_sleep_set_screen_on(true);watch_power_sleep_poll();
 assert(reported_rejected==0 && reported_short==0 && reported_wake==0 && reported_other==0 && reported_error==0);
 reset();watch_power_sleep_set_screen_on(false);now+=1000;
 held_apb=3;watch_power_sleep_poll();held_apb=1;watch_power_sleep_poll();
 watch_power_sleep_set_screen_on(true);held_apb=9;usb=true;watch_power_sleep_poll();
 assert(reported_apb_min==1 && reported_apb_max==3 && reported_samples==2);
 puts("SLEEP_MEASURE=PASS (successful SDK calls only, unchanged returns, reject classification, USB/battery poll coverage, failed lock sampling, window reset, cached USB reconnect reports, zero-duration guard, idempotent transitions, 200 cycles)");
}
'''

def main():
    native.BUILD=ROOT/'build/power-sleep-host'
    build=native.BUILD;build.mkdir(parents=True,exist_ok=True)
    (build/'mock_sleep.h').write_text(MOCK,encoding='utf-8')
    for header in ['esp_attr.h','esp_err.h','esp_log.h','esp_pm.h','esp_sleep.h','esp_timer.h',
                   'driver/usb_serial_jtag.h','freertos/FreeRTOS.h']:
        p=build/header;p.parent.mkdir(parents=True,exist_ok=True)
        p.write_text('#include "mock_sleep.h"\n',encoding='utf-8')
    report=native.compile_run('sleep_measure',TEST)
    cmake=(ROOT/'components/watch_power/CMakeLists.txt').read_text(encoding='utf-8')
    assert '--wrap=esp_light_sleep_start' in cmake
    status=(ROOT/'components/system_status/system_status.cpp').read_text(encoding='utf-8')
    assert 'watch_power_poll();' in status
    print(report)
    (ROOT/'output/watchface/sleep-measurement-verification.txt').write_text(report,encoding='utf-8')

if __name__=='__main__':main()
