"""Verify retained battery-only task runtime samples; adapters are not hardware proof."""
import verify_standby_power as native
from render_lvgl import ROOT

MOCK=r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define configMAX_TASK_NAME_LEN 16
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef void *TaskHandle_t;
typedef unsigned UBaseType_t;
typedef struct {TaskHandle_t xHandle;const char *pcTaskName;unsigned xTaskNumber;uint64_t ulRunTimeCounter;} TaskStatus_t;
static TaskStatus_t tasks[64];static unsigned count,reads,logs,warnings,allocations;
static bool fail_alloc,fail_snapshot;static int64_t now;
static unsigned uxTaskGetSystemState(TaskStatus_t *out,unsigned max,void *total){
 ++reads;assert(max==64);if(fail_snapshot)return 0;memcpy(out,tasks,count*sizeof(*out));return count;}
static void *heap_caps_calloc(size_t n,size_t s,unsigned caps){
 ++allocations;assert(caps==3);return fail_alloc?NULL:calloc(n,s);}
static int64_t esp_timer_get_time(void){return now;}
#define ESP_LOGI(...) (++logs)
#define ESP_LOGW(...) (++warnings)
'''

TEST=r'''
#include "mock_tasks.h"
#include <stdio.h>
#include "components/watch_power/watch_power_tasks.c"
static void reset(void){free(s_profile);s_profile=NULL;s_allocation_warned=false;
 memset(tasks,0,sizeof(tasks));count=reads=logs=warnings=allocations=0;
 now=0;fail_alloc=fail_snapshot=false;}
static void task(unsigned i,unsigned id,const char *name,uint64_t run){
 tasks[i]=(TaskStatus_t){(void *)(uintptr_t)(i+1),name,id,run};}
int main(void){
 for(unsigned cycle=0;cycle<200;++cycle){reset();count=5;
  task(0,1,"IDLE0",1000);task(1,2,"IDLE1",1000);
  task(2,3,"taskLVGL",5000);task(3,4,"wrist_gesture",3000);task(4,5,"wifi",7000);
  watch_power_tasks_poll(true,true);watch_power_tasks_poll(false,false);assert(!allocations);
  watch_power_tasks_poll(true,false);assert(allocations==1 && reads==1 && s_profile->collecting && !logs);
  now=29000000;watch_power_tasks_poll(true,false);assert(reads==1);
  now=30000000;tasks[0].ulRunTimeCounter+=30000000;tasks[1].ulRunTimeCounter+=30000000;
  tasks[2].ulRunTimeCounter+=3000000;tasks[3].ulRunTimeCounter+=9000000;tasks[4].ulRunTimeCounter+=6000000;
  watch_power_tasks_poll(true,false);assert(reads==2 && s_profile->valid && !logs);
  assert(!strcmp(s_profile->top[0].name,"wrist_gesture") && s_profile->top[0].runtime==9000000);
  assert(!strcmp(s_profile->top[1].name,"wifi") && !strcmp(s_profile->top[2].name,"taskLVGL"));
  assert(s_profile->wall==30000000 && s_profile->task_count==5 && !s_profile->top[3].runtime);
  now=35000000;watch_power_tasks_poll(true,true);assert(!s_profile->collecting && reads==3);
  watch_power_tasks_report();assert(logs==4 && s_profile->wall==35000000);
  /* After reconnect, neither the USB-only off period nor awake time enters this sample. */
  now=45000000;watch_power_tasks_poll(true,true);watch_power_tasks_poll(false,true);
  assert(reads==3 && s_profile->wall==35000000);
  watch_power_tasks_report();assert(logs==8);
  /* A new battery window replaces the baseline, not the retained replay. */
  watch_power_tasks_poll(true,false);assert(reads==4 && allocations==1);
  now+=30000000;tasks[3].xTaskNumber=123;tasks[3].ulRunTimeCounter+=100000000;
  tasks[2].ulRunTimeCounter+=2000000;watch_power_tasks_poll(true,false);
  assert(!strcmp(s_profile->top[0].name,"taskLVGL") && s_profile->top[0].runtime==2000000);
  assert(!s_profile->top[1].runtime); /* Reused task handle cannot inherit old task runtime. */
  fail_snapshot=true;now+=1000000;watch_power_tasks_poll(false,false);
  assert(!s_profile->collecting && s_profile->wall==30000000);
 }
 reset();fail_alloc=true;for(unsigned i=0;i<3;++i)watch_power_tasks_poll(true,false);
 assert(warnings==1 && !s_profile);fail_alloc=false;watch_power_tasks_poll(true,false);assert(s_profile);
 assert(!s_profile->collecting); /* Zero tasks / capacity failure retries without valid sample. */
 count=1;task(0,1,"worker",10);watch_power_tasks_poll(true,false);
 watch_power_tasks_poll(false,false);assert(!s_profile->valid); /* Zero duration is safe. */
 reset();
 puts("STANDBY_TASKS=PASS (200 battery/USB cycles, idle exclusion, runtime deltas and ranking, 30s sampling, PSRAM buffer reuse, task identity reuse, failed snapshots, allocation retry, retained report, zero duration)");
}
'''

def main():
    native.BUILD=ROOT/'build/standby-tasks-host'
    build=native.BUILD;build.mkdir(parents=True,exist_ok=True)
    (build/'mock_tasks.h').write_text(MOCK,encoding='utf-8')
    for name in ['esp_heap_caps.h','esp_log.h','esp_timer.h','freertos/FreeRTOS.h','freertos/task.h']:
        p=build/name;p.parent.mkdir(parents=True,exist_ok=True)
        p.write_text('#include "mock_tasks.h"\n',encoding='utf-8')
    result=native.compile_run('standby_tasks',TEST)
    print(result)
    (ROOT/'output/watchface/standby-tasks-verification.txt').write_text(result,encoding='utf-8')

if __name__=='__main__':main()
