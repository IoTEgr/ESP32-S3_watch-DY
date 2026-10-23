#include "watch_power_tasks.h"
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TASK_LIMIT 64
#define TOP_LIMIT 5
#define SAMPLE_US (30LL * 1000000)

typedef struct {
    TaskHandle_t handle;
    UBaseType_t number;
    uint64_t runtime;
} baseline_t;
typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    uint64_t runtime;
} task_cost_t;
typedef struct {
    TaskStatus_t current[TASK_LIMIT];
    baseline_t baseline[TASK_LIMIT];
    task_cost_t top[TOP_LIMIT];
    unsigned baseline_count,task_count;
    int64_t started,last_sample,wall;
    bool collecting,valid;
} task_profile_t;

static task_profile_t *s_profile;
static bool s_allocation_warned;

static unsigned read_tasks(task_profile_t *p)
{
    // The SDK API returns zero if a concurrent task creation exceeds capacity.
    // Keep the previous valid measurement in that case and retry later.
    return uxTaskGetSystemState(p->current,TASK_LIMIT,NULL);
}

static void begin_sample(task_profile_t *p,int64_t now)
{
    const unsigned count=read_tasks(p);
    if(!count)return;
    for(unsigned i=0;i<count;++i) {
        p->baseline[i]=(baseline_t){p->current[i].xHandle,p->current[i].xTaskNumber,
                                   p->current[i].ulRunTimeCounter};
    }
    p->baseline_count=count;p->started=p->last_sample=now;p->collecting=true;
}

static void update_sample(task_profile_t *p,int64_t now)
{
    if(now<=p->started)return;
    const unsigned count=read_tasks(p);
    if(!count)return;
    memset(p->top,0,sizeof(p->top));
    for(unsigned i=0;i<count;++i) {
        const TaskStatus_t *t=&p->current[i];
        if(!strncmp(t->pcTaskName,"IDLE",4))continue; // Idle includes time asleep.
        uint64_t previous=0;
        bool matched=false;
        for(unsigned j=0;j<p->baseline_count;++j) {
            const baseline_t *b=&p->baseline[j];
            if(b->handle==t->xHandle && b->number==t->xTaskNumber) {
                previous=b->runtime;matched=true;break;
            }
        }
        // A task created during the interval has no baseline. Do not attribute
        // its entire lifetime to this window (handles can also be reused).
        if(!matched || t->ulRunTimeCounter<previous)continue;
        const uint64_t cost=t->ulRunTimeCounter-previous;
        for(unsigned j=0;j<TOP_LIMIT;++j) {
            if(cost<=p->top[j].runtime)continue;
            for(unsigned k=TOP_LIMIT-1;k>j;--k)p->top[k]=p->top[k-1];
            strncpy(p->top[j].name,t->pcTaskName,sizeof(p->top[j].name)-1);
            p->top[j].name[sizeof(p->top[j].name)-1]='\0';p->top[j].runtime=cost;
            break;
        }
    }
    p->wall=now-p->started;p->task_count=count;p->last_sample=now;p->valid=true;
}

void watch_power_tasks_poll(bool off,bool usb)
{
    if(!s_profile && off && !usb) {
        s_profile=heap_caps_calloc(1,sizeof(*s_profile),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!s_profile && !s_allocation_warned) {
            s_allocation_warned=true;
            ESP_LOGW("PowerTasks","PSRAM profiling buffer unavailable");
        }
    }
    task_profile_t *p=s_profile;
    if(!p)return;
    const int64_t now=esp_timer_get_time();
    const bool measure=off && !usb;
    if(!p->collecting) {
        if(measure)begin_sample(p,now);
    } else {
        if(!measure || now-p->last_sample>=SAMPLE_US)update_sample(p,now);
        if(!measure)p->collecting=false;
    }
}

void watch_power_tasks_report(void)
{
    const task_profile_t *p=s_profile;
    if(!p || !p->valid)return;
    ESP_LOGI("PowerTasks","battery sample=%llums tasks=%u (task runtime / wall time; idle excluded; exited/new tasks omitted)",
             (unsigned long long)(p->wall/1000),p->task_count);
    for(unsigned i=0;i<TOP_LIMIT && p->top[i].runtime;++i) {
        ESP_LOGI("PowerTasks","%s runtime=%llums one_core=%u.%u%%",p->top[i].name,
                 (unsigned long long)(p->top[i].runtime/1000),
                 (unsigned)(p->top[i].runtime*1000/(uint64_t)p->wall)/10,
                 (unsigned)(p->top[i].runtime*1000/(uint64_t)p->wall)%10);
    }
}
