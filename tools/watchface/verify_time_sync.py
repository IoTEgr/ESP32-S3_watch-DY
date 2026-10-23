"""Run the firmware time service against deterministic network/RTC adapters."""
from pathlib import Path
import subprocess
from render_lvgl import ROOT, CLANG, LLD, VC, SDK

BUILD=ROOT/'build/time-sync-host'

MOCK=r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define I2C_ADDR_BIT_LEN_7 0
#define PCF85063A_ADDRESS 0x51
#define PCF85063A_RTC_SECOND_ADDR 4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct timeval {time_t tv_sec;long tv_usec;};
typedef struct {void *bus_handle,*dev_handle;} pcf85063a_dev_t;
typedef struct {int dev_addr_length,device_address,scl_speed_hz;} i2c_device_config_t;
typedef struct {uint16_t year;uint8_t month,day,dotw,hour,min,sec;} pcf85063a_datetime_t;
typedef struct {int unused;} esp_netif_t;
typedef struct {struct {uint32_t addr;} ip;} esp_netif_ip_info_t;
typedef struct {bool start,wait_for_sync;void (*sync_cb)(struct timeval *);const char *server;} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) {.start=true,.wait_for_sync=true,.server=s}
static bool link_up,oscillator_failed;
static uint32_t ip_addr;
static int64_t monotonic;
static time_t clock_epoch=1792540800;
static int restores,saves,opens,closes,initializations,requests,timezone_initializations;
static const char *server;
static void (*notification)(struct timeval *);
static esp_netif_t netif;
static void *bsp_i2c_get_handle(void) {return &netif;}
static int i2c_master_bus_add_device(void *bus,const i2c_device_config_t *c,void **device) {
 assert(bus && c->device_address==0x51);*device=&netif;++opens;return 0;}
static int i2c_master_bus_rm_device(void *device) {assert(device);++closes;return 0;}
static int pcf85063a_read_register(pcf85063a_dev_t *r,int address,uint8_t *out,int n) {
 assert(r->dev_handle && address==4 && n==1);*out=oscillator_failed?0x80:0;return 0;}
static int pcf85063a_get_time_date(pcf85063a_dev_t *r,pcf85063a_datetime_t *date) {
 *date=(pcf85063a_datetime_t){2026,10,21,3,12,34,56};return 0;}
static int pcf85063a_set_time_date(pcf85063a_dev_t *r,pcf85063a_datetime_t date) {
 assert(r->dev_handle && date.year==2026 && date.month==10);++saves;return 0;}
static int mock_settimeofday(const struct timeval *v,void *unused) {clock_epoch=v->tv_sec;++restores;return 0;}
static int mock_setenv(const char *k,const char *v,int replace) {
 assert(!strcmp(k,"TZ") && !strcmp(v,"CST-8"));++timezone_initializations;return _putenv("TZ=CST-8");}
static time_t mock_time(time_t *out) {if(out)*out=clock_epoch;return clock_epoch;}
static struct tm *mock_localtime_r(const time_t *v,struct tm *out) {*out=*localtime(v);return out;}
#define settimeofday mock_settimeofday
#define setenv mock_setenv
#define tzset _tzset
#define localtime_r mock_localtime_r
#define time mock_time
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {return &netif;}
static bool esp_netif_is_netif_up(esp_netif_t *n) {return link_up;}
static int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *out) {out->ip.addr=ip_addr;return 0;}
static int esp_netif_sntp_init(const esp_sntp_config_t *c) {
 assert(!c->start && !c->wait_for_sync);notification=c->sync_cb;++initializations;return 0;}
static void esp_sntp_setservername(int index,const char *name) {assert(index==0);server=name;}
static int esp_netif_sntp_start(void) {++requests;return 0;}
static int64_t esp_timer_get_time(void) {return monotonic;}
static const char *esp_err_to_name(int code) {return "ESP_OK";}
'''

TEST=r'''
#include "mock_sdk.h"
#include "components/system_status/time_sync.c"
static void verify_shanghai_calendar(void) {
 struct tm local;
 time_t utc=1792575900; /* 2026-10-21 09:45 UTC == Shanghai 17:45. */
 localtime_r(&utc,&local);
 assert(local.tm_year==126 && local.tm_mon==9 && local.tm_mday==21 && local.tm_wday==3);
 assert(local.tm_hour==17 && local.tm_min==45);
 /* Reproduce the previous component bug: adding +8h to UTC changes the date. */
 time_t shifted=utc+8*3600;localtime_r(&shifted,&local);
 assert(local.tm_mday==22 && local.tm_hour==1 && local.tm_min==45);
 utc=1792598399;localtime_r(&utc,&local);assert(local.tm_mday==21 && local.tm_hour==23 && local.tm_min==59 && local.tm_sec==59);
 utc=1792598400;localtime_r(&utc,&local);assert(local.tm_mday==22 && local.tm_hour==0 && local.tm_min==0);
 puts("SHANGHAI_TIME=PASS (UTC+8, weekday/date, midnight rollover, reproduce old double-offset 01:45 next day)");
}
int main(int argc,char **argv) {
 oscillator_failed=argc>1;
 time_sync_init();time_sync_init();assert(timezone_initializations==1 && !restores && !requests);
 verify_shanghai_calendar();
 time_sync_poll(false);assert(restores==(oscillator_failed?0:1));
 assert(timezone_initializations==1);
 if(!oscillator_failed)assert(clock_epoch==1792557296); /* Shanghai RTC 12:34:56 -> UTC 04:34:56. */
 time_sync_poll(true);assert(!requests && !initializations); /* no IP */
 ip_addr=1;time_sync_poll(true);assert(!requests); /* interface down */
 link_up=true;time_sync_poll(true);assert(initializations==1 && requests==1);
 assert(!strcmp(server,"ntp.aliyun.com"));
 monotonic=19000000;time_sync_poll(true);assert(requests==1);
 monotonic=20000000;time_sync_poll(true);assert(requests==2 && !strcmp(server,"ntp.tencent.com"));
 monotonic=40000000;time_sync_poll(true);assert(requests==3 && !strcmp(server,"cn.pool.ntp.org"));
 monotonic=100000000;time_sync_poll(true);assert(requests==4);
 notification(NULL);time_sync_poll(true);assert(saves==1);
 monotonic+=3600000000LL;time_sync_poll(true);assert(requests==4); /* let lwIP do hourly sync */
 time_sync_poll(false);time_sync_poll(true);assert(requests==5);
 ip_addr=2;time_sync_poll(true);assert(requests==6 && initializations==1);
 notification(NULL);time_sync_poll(true);assert(saves==2);
 assert(opens==closes);
 puts("TIME_SYNC=PASS (offline boot, missed IP event, server fallback, retry, reconnect, IP change, RTC restore/save, oscillator rejection)");
 return 0;
}
'''

def main():
    for name in ['sdkconfig','sdkconfig.defaults']:
        config=(ROOT/name).read_text(encoding='utf-8')
        assert 'CONFIG_XIAOZHI_SYNC_SYSTEM_TIME_FROM_SERVER=y' not in config
        assert '# CONFIG_XIAOZHI_SYNC_SYSTEM_TIME_FROM_SERVER is not set' in config
    startup=(ROOT/'main/main.cpp').read_text(encoding='utf-8')
    startup=startup[startup.index('extern "C" void app_main(void)'):]
    assert startup.index('time_sync_init();')<startup.index('bsp_display_start_with_config(')
    BUILD.mkdir(parents=True,exist_ok=True)
    (BUILD/'mock_sdk.h').write_text(MOCK,encoding='utf-8')
    for name in ['sys/time.h','bsp/esp-bsp.h','esp_log.h','esp_netif.h',
                 'esp_netif_sntp.h','esp_sntp.h','esp_timer.h','pcf85063a.h']:
        path=BUILD/name;path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text('#include "mock_sdk.h"\n',encoding='utf-8')
    (BUILD/'test.c').write_text(TEST,encoding='utf-8')
    flags=['--target=x86_64-pc-windows-msvc','-std=c11','-O1','-D_CRT_SECURE_NO_WARNINGS']
    for p in [BUILD,ROOT,ROOT/'components/system_status/include',VC/'include',SDK/'Include/shared',SDK/'Include/um']:
        flags+=['-I',str(p)]
    subprocess.run([str(CLANG),*flags,'-c',str(BUILD/'test.c'),'-o',str(BUILD/'test.obj')],check=True)
    subprocess.run([str(LLD),'-flavor','link',str(BUILD/'test.obj'),'/out:'+str(BUILD/'test.exe'),
        '/subsystem:console','/defaultlib:libcmt','/defaultlib:oldnames','/defaultlib:kernel32',
        '/libpath:'+str(VC/'lib/amd64'),'/libpath:'+str(SDK/'Lib/winv6.3/um/x64')],check=True)
    report=''
    for args in [[],['invalid-oscillator']]:
        p=subprocess.run([str(BUILD/'test.exe'),*args],capture_output=True,text=True,check=True)
        print(p.stdout.strip());report+=p.stdout
    (ROOT/'output/watchface/time-sync-verification.txt').write_text(report,encoding='utf-8')

if __name__=='__main__':main()
