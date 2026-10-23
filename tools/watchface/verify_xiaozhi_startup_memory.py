"""Exercise production queue/task allocation and queue cleanup under SRAM pressure."""
import re
import verify_standby_power as native
from verify_boot_voice import function
from render_lvgl import ROOT

HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef int esp_err_t;
enum {ESP_OK=0,ESP_ERR_NO_MEM=1,pdPASS=1,pdFAIL=0,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2};
struct Flag {bool value=false;void store(bool v){value=v;}};
struct Queue {bool live=false;size_t bytes=0;};
typedef Queue *QueueHandle_t;
typedef int *TaskHandle_t;
static Queue queues[3];static int task;
static size_t internal_free,psram_free,largest;
static unsigned attempts,fail_at,created_queues,created_tasks,errors;
static QueueHandle_t xQueueCreateWithCaps(size_t n,size_t size,unsigned caps){
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(++attempts==fail_at)return nullptr;
 const size_t bytes=n*size+96;if(bytes>psram_free)return nullptr;
 for(auto &q:queues)if(!q.live){q.live=true;q.bytes=bytes;psram_free-=bytes;++created_queues;return &q;}
 assert(false);return nullptr;
}
static void vQueueDeleteWithCaps(Queue *q){assert(q && q->live);psram_free+=q->bytes;q->live=false;}
static int xTaskCreatePinnedToCore(void (*fn)(void *),const char *,unsigned stack,void *,unsigned priority,TaskHandle_t *out,int core){
 assert(stack==12*1024 && priority==5 && core==0);
 if(++attempts==fail_at || stack>largest || stack+512>internal_free)return pdFAIL;
 assert(!created_tasks);internal_free-=stack+512;++created_tasks;*out=&task;return pdPASS;
}
static void logServiceInfoHeap(const char *){}
__CONSTANTS__
struct XiaozhiApp {
 __TYPES__
 __STACK__
 QueueHandle_t _control_queue=nullptr,_pcm_queue=nullptr,_playback_queue=nullptr;
 TaskHandle_t _control_task=nullptr;Flag _control_running;
 static void controlTaskEntry(void *){}
 void setError(int,const char *){++errors;}
 void updateNetworkState(){}
 bool init(){
 __QUEUE_CREATE__
 if(!_control_queue || !_pcm_queue || !_playback_queue)return false;
 __TASK_CREATE__
 void closeQueues(){__QUEUE_DELETE__}
 void stop(){if(_control_task){internal_free+=CONTROL_TASK_STACK_SIZE+512;_control_task=nullptr;created_tasks=0;}
  closeQueues();}
};
int main(){
 for(unsigned cycle=0;cycle<200;++cycle)for(unsigned fail=0;fail<=4;++fail){
  internal_free=32*1024;largest=16*1024;psram_free=2*1024*1024;
  attempts=created_queues=created_tasks=errors=0;fail_at=fail;
  for(auto &q:queues)assert(!q.live);
  XiaozhiApp app;const bool ok=app.init();assert(ok==(fail==0));
  /* About 31 KiB of control/playback payload would leave too little SRAM for
   * the unchanged 12 KiB stack; the production queues must leave it intact. */
  const size_t payload=CONTROL_QUEUE_LENGTH*sizeof(XiaozhiApp::ControlEvent)+
      PLAYBACK_QUEUE_LENGTH*sizeof(XiaozhiApp::AudioPacket);
  assert(payload>30*1024 && 32*1024-payload<12*1024);
  assert(internal_free==32*1024-(ok?12*1024+512:0));
  if(!ok){assert(!app._control_task && !app._control_running.value);
   fail_at=0;assert(app.init());} /* Partial creation and failed task creation can retry. */
  assert(created_queues==3 && created_tasks==1);
  app.stop();app.stop();assert(internal_free==32*1024 && psram_free==2*1024*1024);
 }
 puts("XIAOZHI_STARTUP_MEMORY=PASS (32KiB SRAM, 16KiB largest block, >30KiB payload offloaded, internal 12KiB task stack, each queue/task failure and retry, paired queue deletion, 1000 cycles without leaks)");
}
'''

def main():
    app=(ROOT/'components/XiaozhiApp/XiaozhiApp.cpp').read_text(encoding='utf-8')
    header=(ROOT/'components/XiaozhiApp/XiaozhiApp.hpp').read_text(encoding='utf-8')
    begin=app.index('    _control_queue =',app.index('bool XiaozhiApp::init()'))
    end=app.index('    if (!_state_mutex',begin)
    task_begin=app.index('    logServiceInfoHeap("before control task");',end)
    task_end=app.index('bool XiaozhiApp::deinit()',task_begin)
    deletion_begin=app.index('    if (_playback_queue)',task_end)
    deletion_end=app.index('    if (_control_exited)',deletion_begin)
    types='\n'.join(function(header,s)+';' for s in [
        '    enum class ControlEventType','    struct ControlEvent','    struct PcmFrame','    struct AudioPacket'])
    constants='\n'.join(re.findall(r'constexpr size_t (?:CONTROL|PCM|PLAYBACK)_QUEUE_LENGTH = .*?;',app))
    stack=re.search(r'    static constexpr int CONTROL_TASK_STACK_SIZE = .*?;',header).group(0)
    text=HARNESS.replace('__TYPES__',types).replace('__CONSTANTS__',constants).replace('__STACK__',stack)
    text=text.replace('__QUEUE_CREATE__',app[begin:end]).replace('__TASK_CREATE__',app[task_begin:task_end])
    text=text.replace('__QUEUE_DELETE__',app[deletion_begin:deletion_end])
    native.BUILD=ROOT/'build/xiaozhi-startup-memory-host';native.BUILD.mkdir(parents=True,exist_ok=True)
    result=native.compile_run('startup_memory',text,True)
    print(result)
    (ROOT/'output/watchface/xiaozhi-startup-memory-verification.txt').write_text(result,encoding='utf-8')

if __name__=='__main__':main()
