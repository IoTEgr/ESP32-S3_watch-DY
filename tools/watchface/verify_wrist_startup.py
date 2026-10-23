"""Exercise production wrist startup and orientation loop with RTOS/I2C adapters.

Reproduces insufficient contiguous SRAM and startup face-down; physical sensor
and sleep/wake operation must still be tested on the watch.
"""
import re
from render_lvgl import ROOT
from verify_boot_voice import function
from verify_standby_power import BUILD, compile_run

HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <math.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_FALSE(cond,ret,...) do {if(!(cond))return ret;}while(0)
#define pdMS_TO_TICKS(x) (x)
#define atomic_exchange(p,v) ((p)->exchange(v))
#define atomic_store(p,v) ((p)->value=(v))
enum {ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_NO_MEM=2,ESP_ERR_INVALID_STATE=3,ESP_ERR_TIMEOUT=4,
 pdTRUE=1,pdPASS=1,pdFAIL=0,WRIST_MAX_LISTENERS=4,MALLOC_CAP_INTERNAL=1,MALLOC_CAP_SPIRAM=2,
 MALLOC_CAP_8BIT=4,QMI8658_DISABLE_ALL=0,QMI8658_ADDRESS_HIGH=107};
typedef int esp_err_t,TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *i2c_master_bus_handle_t;
typedef unsigned TickType_t;
typedef void (*TaskFunction_t)(void *);
typedef void (*wrist_listener_t)(bool,void *);
struct wrist_listener_entry_t {wrist_listener_t callback;void *user_data;};
struct qmi8658_dev_t {void *dev_handle;};
struct qmi8658_data_t {float accelX,accelY,accelZ;};
struct Flag {bool value;bool exchange(bool v){bool old=value;value=v;return old;}};
__CONSTANTS__
static qmi8658_dev_t s_imu;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_listeners_mutex,s_sensor_mutex;
static wrist_listener_entry_t s_listeners[WRIST_MAX_LISTENERS];
static bool s_initialized,s_enabled=true,s_screen_up=true;
static int s_sensor_profile=-1;
static Flag s_publish_state;
static unsigned internal_free,psram_free,created,stage,fail_at,live_mutexes,task_stack;
static bool sensor_on;
static SemaphoreHandle_t xSemaphoreCreateMutex(){if(++stage==fail_at)return 0;
 assert(internal_free>=96);internal_free-=96;++live_mutexes;return (void *)(uintptr_t)live_mutexes;}
static void vSemaphoreDelete(void *m){assert(m && live_mutexes);--live_mutexes;internal_free+=96;}
static int xSemaphoreTake(void *m,int){assert(m);return 1;}
static void xSemaphoreGive(void *m){assert(m);}
static int qmi8658_init(qmi8658_dev_t *d,void *,int){d->dev_handle=(void *)1;return ++stage==fail_at?ESP_ERR_TIMEOUT:ESP_OK;}
static void qmi8658_set_accel_unit_mps2(qmi8658_dev_t *,bool b){assert(b);}
static int apply_sensor_profile_locked(){if(++stage==fail_at)return ESP_ERR_TIMEOUT;
 s_sensor_profile=1;sensor_on=true;return 0;}
static int qmi8658_enable_sensors(qmi8658_dev_t *,int flags){assert(flags==0);sensor_on=false;return 0;}
static int i2c_master_bus_rm_device(void *){return 0;}
static void wrist_task(void *);
static int xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn,const char *name,unsigned stack,void *,
 unsigned priority,int *out,int core,unsigned caps){
 assert(s_initialized && fn==wrist_task && strcmp(name,"wrist_gesture")==0);
 assert(core==1 && priority==3 && stack==3072 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 // Log showed largest internal block of 1536 bytes: the old 3072-byte stack fails.
 assert(stack>1536 && internal_free>=512 && psram_free>=stack);
 if(++stage==fail_at)return pdFAIL;
 internal_free-=512;psram_free-=stack;task_stack=stack;*out=1;++created;return pdPASS;
}
__INIT__
__ADD_LISTENER__
static unsigned tick,started,duration,mode,attach_at,off_at,on_at,off_count,on_count,first_off;
static bool attached,fail_reads,noise;
static jmp_buf stopped;
static void callback(bool on,void *){if(on)++on_count;else {++off_count;if(!first_off)first_off=tick-started;}}
static unsigned xTaskGetTickCount(){return tick;}
static int wrist_gesture_read_accel(float *x,float *y,float *z){
 if(fail_reads)return ESP_ERR_TIMEOUT;
 *x=mode?7.0f:-0.1f;*y=mode?3.0f:8.5f;*z=-5.0f;return 0;
}
static void vTaskDelayUntil(unsigned *last,unsigned period){
 *last+=period;tick=*last;unsigned elapsed=tick-started;
 if(!attached && elapsed>=attach_at){attached=true;assert(wrist_gesture_add_listener(callback,0)==0);}
 if(off_at && elapsed>=off_at)mode=0;
 if(on_at && elapsed>=on_at)mode=1;
 if(noise && elapsed>=4000)mode=(elapsed/50)%2;
 if(elapsed>=duration)longjmp(stopped,1);
}
__DISPATCH__
__WORKER__
static void reset_init(){
 s_imu.dev_handle=0;s_task=0;s_listeners_mutex=s_sensor_mutex=0;s_initialized=false;
 s_sensor_profile=-1;internal_free=2463;psram_free=5*1024*1024;
 created=stage=fail_at=live_mutexes=task_stack=0;sensor_on=false;
}
static void loop(unsigned ms,unsigned initial_mode,unsigned attach,unsigned start=0){
 memset(s_listeners,0,sizeof(s_listeners));s_screen_up=true;s_enabled=true;s_publish_state.value=false;
 tick=started=start;duration=ms;mode=initial_mode;attach_at=attach;attached=false;
 off_count=on_count=first_off=0;
 if(attach==0){attached=true;assert(wrist_gesture_add_listener(callback,0)==0);}
 if(setjmp(stopped)==0)wrist_task(0);
}
int main(){
 for(unsigned f=1;f<=5;++f){reset_init();fail_at=f;
  assert(wrist_gesture_init((void *)1)!=0 && !s_initialized && !s_task && !created);
  assert(!s_imu.dev_handle && !s_sensor_mutex && !s_listeners_mutex && !live_mutexes && !sensor_on);
  assert(internal_free==2463 && psram_free==5*1024*1024);
  fail_at=stage=0;assert(wrist_gesture_init((void *)1)==0 && s_initialized && created==1);
 }
 reset_init();assert(wrist_gesture_init((void *)1)==0 && task_stack==3072);
 assert(internal_free==2463-2*96-512 && psram_free==5*1024*1024-3072);
 assert(wrist_gesture_init((void *)1)==0 && created==1);
 puts("WRIST_STARTUP=PASS (1.5KB largest SRAM block, PSRAM stack with internal TCB, every init failure cleanup/retry, idempotent start)");
 for(int i=0;i<200;++i){off_at=on_at=0;noise=fail_reads=false;
  loop(5000,0,0);assert(off_count==1 && first_off>=4000 && first_off<=4300 && !s_screen_up);
  loop(5500,0,5000);assert(off_count==1 && !on_count && first_off>=5000 && first_off<=5050);
 }
 off_at=6000;on_at=7000;loop(8000,1,0);assert(off_count==1 && on_count>=2 && s_screen_up);
 off_at=on_at=0;noise=true;loop(6000,1,0);assert(!off_count);noise=false;
 loop(5000,0,0,0xfffff000u);assert(off_count==1 && first_off>=4000);
 fail_reads=true;loop(5000,0,0);assert(!off_count && !on_count);fail_reads=false;
 puts("WRIST_ORIENTATION=PASS (boot face-down closes after grace, late listener receives current state, raise/lower, debounce, tick wrap, failed-read rejection, 200 cycles)");
}
'''

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    src=(ROOT/'components/wrist_gesture/wrist_gesture.c').read_text(encoding='utf-8')
    constants='\n'.join(re.findall(r'^#define (?:WRIST_\w+|RAD_TO_DEG)\s+.*$',src,re.M))
    source=HARNESS.replace('__CONSTANTS__',constants)
    for marker,signature in [('__INIT__','esp_err_t wrist_gesture_init('),
        ('__ADD_LISTENER__','esp_err_t wrist_gesture_add_listener('),
        ('__DISPATCH__','static void dispatch('),('__WORKER__','static void wrist_task(')]:
        source=source.replace(marker,function(src,signature))
    report=compile_run('wrist_startup',source,True)
    main=(ROOT/'main/main.cpp').read_text(encoding='utf-8')
    assert main.index('board_keys_init()')<main.index('brookesia::system_status::start(')
    assert main.index('wrist_gesture_init(')<main.index('brookesia::system_status::start(')
    cfg=(ROOT/'sdkconfig').read_text(encoding='utf-8')
    assert 'CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y' in cfg
    assert '# CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP is not set' in cfg
    print(report)
    (ROOT/'output/watchface/wrist-startup-verification.txt').write_text(report,encoding='utf-8')

if __name__=='__main__':main()
