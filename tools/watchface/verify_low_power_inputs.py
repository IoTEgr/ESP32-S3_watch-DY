"""Native adapters for the production interrupt key worker and shared IMU policy.

This checks event sequencing, not physical GPIO wake or sensor accuracy/current.
"""
import re
from verify_boot_voice import function
from verify_standby_power import compile_run, BUILD
from render_lvgl import ROOT

KEYS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#define IRAM_ATTR
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define portYIELD_FROM_ISR() ((void)0)
#define atomic_exchange(p,v) ((p)->exchange(v))
typedef int gpio_num_t,BaseType_t,esp_err_t;
typedef unsigned TickType_t;
enum {ESP_OK=0,BOARD_KEY_BOOT=0,BOARD_KEY_PWR=1,BOARD_KEY_MAX=2,
 BOARD_KEY_SHORT_PRESS,BOARD_KEY_LONG_PRESS,GPIO_INTR_LOW_LEVEL=4,GPIO_INTR_HIGH_LEVEL=5};
typedef int board_key_t;
__KEY_CONSTANTS__
__KEY_STATE__
struct Atomic {unsigned value;unsigned exchange(unsigned v){unsigned old=value;value=v;return old;}};
static Atomic s_haptic_request_ms;
static bool s_haptic_enabled=true,s_interrupt_ready=true;
static int s_task=1;
static key_state_t s_keys[2];
static bool pressed[2],irq[2],wake[2];
static unsigned now,notifications,idle_waits,scans,shorts[2],longs[2],haptics;
static bool fail_arm,inject_arm;
static jmp_buf stopped;
struct Event {unsigned at;int key;bool pressed;};
static Event events[20];static int count,cursor;
static unsigned end_time;
static int index_of(int pin){assert(pin==0 || pin==10);return pin==0?0:1;}
static void key_isr(void *);
static int gpio_intr_disable(int pin){irq[index_of(pin)]=false;return 0;}
static int gpio_wakeup_disable(int pin){wake[index_of(pin)]=false;return 0;}
static int gpio_wakeup_enable(int pin,int level){int k=index_of(pin);
 assert(level==(k==0?GPIO_INTR_LOW_LEVEL:GPIO_INTR_HIGH_LEVEL));
 if(fail_arm){fail_arm=false;return 1;}wake[k]=true;return 0;}
static int gpio_intr_enable(int pin){int k=index_of(pin);irq[k]=true;
 if(inject_arm && k==0){inject_arm=false;pressed[0]=true;}
 if(pressed[k])key_isr((void *)(uintptr_t)pin);return 0;}
static void vTaskNotifyGiveFromISR(int task,int *higher){assert(task==1);++notifications;*higher=1;}
static int64_t esp_timer_get_time(){return (int64_t)now*1000;}
static void dispatch_event(int k,int ev){if(ev==BOARD_KEY_SHORT_PRESS)++shorts[k];else ++longs[k];}
static void board_keys_haptic_pulse(unsigned){++haptics;}
static void pulse_haptic(){++haptics;}
static bool read_pressed(key_state_t *key){++scans;return pressed[index_of(key->gpio)];}
static void advance(unsigned target){
 while(cursor<count && events[cursor].at<=target){Event e=events[cursor++];now=e.at;pressed[e.key]=e.pressed;
  if(irq[e.key] && e.pressed)key_isr((void *)(uintptr_t)s_keys[e.key].gpio);}
 now=target;if(now>=end_time)longjmp(stopped,1);
}
static void vTaskDelay(unsigned ticks){advance(now+ticks);}
static unsigned ulTaskNotifyTake(int,unsigned ticks){
 if(ticks==portMAX_DELAY){++idle_waits;
  if(!notifications){
   while(cursor<count && !notifications)advance(events[cursor].at);
   if(!notifications)advance(end_time);
  }
 }else advance(now+ticks);
 unsigned n=notifications;notifications=0;return n;
}
__KEY_METHODS__
static void reset(){
 memset(s_keys,0,sizeof(s_keys));memset(pressed,0,sizeof(pressed));memset(irq,0,sizeof(irq));memset(wake,0,sizeof(wake));
 memset(shorts,0,sizeof(shorts));memset(longs,0,sizeof(longs));
 s_keys[0].gpio=0;s_keys[0].active_low=true;s_keys[1].gpio=10;
 s_keys[0].candidate_samples=s_keys[1].candidate_samples=KEY_DEBOUNCE_SAMPLES;
 now=notifications=idle_waits=scans=haptics=0;count=cursor=0;end_time=3000;
 s_interrupt_ready=true;fail_arm=inject_arm=false;s_haptic_request_ms.value=0;
}
static void add(unsigned at,int key,bool down){events[count++]=(Event){at,key,down};}
static void run(){if(setjmp(stopped)==0)key_task(0);}
int main(){
 reset();end_time=60000;run();assert(idle_waits==1 && scans==2 && !shorts[0]);
 reset();add(100,0,true);add(110,0,false);run();assert(!shorts[0] && !longs[0]);
 reset();add(100,0,true);add(300,0,false);run();assert(shorts[0]==1 && !longs[0] && idle_waits>=2);
 reset();add(100,0,true);add(1700,0,false);run();assert(longs[0]==1 && !shorts[0]);
 reset();add(100,1,true);add(300,1,false);run();assert(shorts[1]==1 && !longs[1]);
 reset();add(100,1,true);add(2100,1,false);run();assert(longs[1]==1 && !shorts[1]);
 reset();inject_arm=true;add(250,0,false);run();assert(shorts[0]==1);
 reset();fail_arm=true;add(100,0,true);add(300,0,false);run();assert(!s_interrupt_ready && shorts[0]==1);
 reset();s_haptic_request_ms.value=40;run();assert(haptics==1 && idle_waits==1);
 for(int i=0;i<200;++i){reset();add(100,0,true);add(300,0,false);run();assert(shorts[0]==1 && !longs[0]);}
 puts("KEY_IRQ=PASS (no periodic idle scans, BOOT/PWR taps/holds, bounce, arm-race press, held-level suppression, polling fallback, haptic wake, 200 cycles)");
}
'''

IMU=r'''
#include <assert.h>
#include <stdio.h>
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
enum {ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_ERR_INVALID_ARG=2,ESP_ERR_TIMEOUT=3,
 QMI8658_DISABLE_ALL=0,QMI8658_ENABLE_ACCEL=1,QMI8658_ACCEL_RANGE_4G=4,
 QMI8658_ACCEL_ODR_500HZ=5,QMI8658_ACCEL_ODR_125HZ=6,QMI8658_ACCEL_ODR_LOWPOWER_21HZ=13,pdTRUE=1};
typedef int esp_err_t,qmi8658_accel_odr_t;
static bool screen_on=true,s_initialized=true,s_enabled=true,s_app_active=false;
static int s_imu,s_sensor_mutex=1,s_sensor_profile=-1,locked,flags,odr,range,reads,call,fail_at;
static bool watch_power_is_screen_on(){return screen_on;}
static int xSemaphoreTake(int,int){assert(!locked);locked=1;return 1;}
static void xSemaphoreGive(int){assert(locked);locked=0;}
static int op(){assert(locked);return ++call==fail_at?ESP_ERR_TIMEOUT:0;}
static int qmi8658_enable_sensors(int *,int f){int r=op();if(!r)flags=f;return r;}
static int qmi8658_set_accel_range(int *,int r){assert(flags==0);int e=op();if(!e)range=r;return e;}
static int qmi8658_set_accel_odr(int *,int o){assert(flags==0);int e=op();if(!e)odr=o;return e;}
static int qmi8658_read_accel(int *,float *x,float *y,float *z){assert(locked && flags==1 && range==4);
 ++reads;*x=1;*y=2;*z=9.8f;return 0;}
__IMU_METHODS__
static void sample(){float x,y,z;assert(wrist_gesture_read_accel(&x,&y,&z)==0 && x==1 && !locked);}
int main(){
 for(int i=0;i<200;++i){screen_on=true;s_enabled=true;sample();assert(odr==6 && flags==1);
  screen_on=false;sample();assert(odr==13 && flags==1);
  assert(wrist_gesture_set_app_active(true)==0);sample();assert(odr==13);
  screen_on=true;sample();assert(odr==5 && flags==1);
  assert(wrist_gesture_set_app_active(false)==0);sample();assert(odr==6);
  s_enabled=false;float x,y,z;assert(wrist_gesture_read_accel(&x,&y,&z)==ESP_ERR_INVALID_STATE && flags==0);
  assert(wrist_gesture_set_app_active(true)==0);sample();assert(odr==5);
  assert(wrist_gesture_set_app_active(false)==0 && flags==0);s_enabled=true;sample();
 }
 for(int f=1;f<=4;++f){screen_on=false;s_sensor_profile=-1;call=0;fail_at=f;
  float x,y,z;assert(wrist_gesture_read_accel(&x,&y,&z)!=0 && !locked && s_sensor_profile==-1);
  fail_at=0;sample();assert(odr==13 && flags==1);}
 screen_on=true;s_sensor_profile=-1;call=0;fail_at=1;
 assert(wrist_gesture_set_app_active(true)!=0 && !s_app_active && !locked);
 fail_at=0;sample();assert(odr==6);
 assert(wrist_gesture_read_accel(0,0,0)==ESP_ERR_INVALID_ARG);
 puts("IMU_POWER=PASS (gyro always off, accel-only reads, 125Hz awake / 21Hz dark / 500Hz app, fixed scale, disabled-wrist/app coexistence, failed-mode retry, 200 cycles)");
}
'''

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    keys=(ROOT/'components/board_keys/board_keys.c').read_text(encoding='utf-8')
    constants='\n'.join(re.findall(r'^#define (?:KEY_\w+|BOOT_LONG_MS)\s+\d+',keys,re.M))
    state=keys[keys.index('typedef struct {'):keys.index('} key_state_t;')+len('} key_state_t;')]
    methods=''.join(function(keys,s) for s in ['static void IRAM_ATTR key_isr(',
        'static void set_key_wakeup(', 'static void scan_key(', 'static void key_task('])
    report=compile_run('key_irq',KEYS.replace('__KEY_CONSTANTS__',constants).replace('__KEY_STATE__',state).replace('__KEY_METHODS__',methods),True)
    imu=(ROOT/'components/wrist_gesture/wrist_gesture.c').read_text(encoding='utf-8')
    methods=''.join(function(imu,s) for s in ['static esp_err_t apply_sensor_profile_locked(',
        'esp_err_t wrist_gesture_read_accel(', 'esp_err_t wrist_gesture_set_app_active('])
    report+=compile_run('imu_power',IMU.replace('__IMU_METHODS__',methods),True)
    request=function(keys,'void board_keys_haptic_request(')
    assert 'xTaskNotifyGive(s_task)' in request
    init=function(keys,'esp_err_t board_keys_init(')
    assert init.index('gpio_isr_handler_add')<init.index('xTaskCreatePinnedToCore')
    assert 'esp_sleep_enable_gpio_wakeup()' in init
    gravity=(ROOT/'components/Gravitysphere/Gravitysphere.cpp').read_text(encoding='utf-8')
    assert 'qmi8658_init(' not in gravity and 'qmi8658_read_sensor_data(' not in gravity
    assert 'wrist_gesture_set_app_active(true)' in function(gravity,'bool Gravitysphere::run(')
    assert 'wrist_gesture_set_app_active(false)' in function(gravity,'void Gravitysphere::stopWorker(')
    print(report)
    (ROOT/'output/watchface/low-power-input-verification.txt').write_text(report,encoding='utf-8')

if __name__=='__main__':main()
