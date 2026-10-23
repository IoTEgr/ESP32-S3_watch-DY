"""Exercise production audio-task creation/cleanup under fragmented SRAM."""
import re
import subprocess
from render_lvgl import ROOT, CLANG, LLD, VC, SDK

BUILD=ROOT/'build/xiaozhi-tasks-host'
APP=ROOT/'components/XiaozhiApp/XiaozhiApp.cpp'
AFE=ROOT/'components/XiaozhiApp/XiaozhiAudioProcessor.cpp'
HEADER=ROOT/'components/XiaozhiApp/XiaozhiApp.hpp'

HARNESS=r'''
#include <assert.h>
#include <stdio.h>
typedef int BaseType_t;
typedef void (*TaskFunction_t)(void *);
enum {pdPASS=1,pdFAIL=0,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_INTERNAL=4,
 ESP_ERR_INVALID_STATE=1,ESP_ERR_NO_MEM=2,PROCESS_TASK_STACK_SIZE=8192,PROCESS_TASK_PRIORITY=3};
#define ESP_UTILS_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
struct Task {bool live=false,running=false;unsigned stack=0;};
typedef Task *TaskHandle_t;
struct Flag {bool value=false;void store(bool v){value=v;}bool load(){return value;}};
static Task tasks[8];
static unsigned internal_free,psram_free,attempt,fail_on,deleted,afe_task_id;
static BaseType_t create(TaskFunction_t,const char *,unsigned stack,void *,unsigned,TaskHandle_t *out,unsigned caps) {
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(++attempt==fail_on)return pdFAIL;
 assert(stack<=psram_free && internal_free>=512);
 /* Only 1.5 KB is contiguous: no 6 KB/8 KB internal stack can fit. */
 assert(512<=1536);
 for(auto &task:tasks)if(!task.live) {
  task.live=task.running=true;task.stack=stack;internal_free-=512;psram_free-=stack;*out=&task;return pdPASS;
 }
 assert(false);return pdFAIL;
}
static BaseType_t xTaskCreateWithCaps(TaskFunction_t f,const char *n,unsigned s,void *a,unsigned p,TaskHandle_t *t,unsigned c) {
 return create(f,n,s,a,p,t,c);
}
static BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t f,const char *n,unsigned s,void *a,unsigned p,TaskHandle_t *t,int core,unsigned c) {
 assert(core==0);return create(f,n,s,a,p,t,c);
}
static void vTaskDeleteWithCaps(TaskHandle_t task) {
 assert(task && task->live && !task->running); /* Owner deletion; no helper task. */
 task->live=false;internal_free+=512;psram_free+=task->stack;++deleted;
}
struct Processor {
 TaskHandle_t processing_task_=nullptr;
 static void processingTaskEntry(void *) {}
 bool createTask() {__AFE_CREATE__ return created==pdPASS;}
 void shutdown() {
  for(auto &task:tasks)assert(&task==processing_task_ || !task.live);
  if(processing_task_){processing_task_->running=false;vTaskDeleteWithCaps(processing_task_);processing_task_=nullptr;}
 }
};
struct XiaozhiApp {
 __STACK_CONSTANTS__
 Flag _audio_tasks_running,_input_task_running,_encoder_task_running,_playback_task_running,_accept_playback;
 TaskHandle_t _input_task=nullptr,_encoder_task=nullptr,_playback_task=nullptr;
 Processor *_audio_processor=nullptr;
 static void inputTaskEntry(void *) {} static void encoderTaskEntry(void *) {} static void playbackTaskEntry(void *) {}
 void finishPreroll(int) {} void setAudioModes(bool,bool) {} void setError(int,const char *) {}
 bool startWorkers();bool shutdownAudio();
};
static XiaozhiApp *owner;
static void vTaskDelay(unsigned) {
 assert(owner && !owner->_audio_tasks_running.load());
 for(auto &task:tasks)task.running=false;
 owner->_input_task_running.store(false);owner->_encoder_task_running.store(false);owner->_playback_task_running.store(false);
}
__CREATE_WORKERS__
__CLEANUP_WORKERS__
static void one_cycle(unsigned fail_worker) {
 internal_free=3563;psram_free=200*1024;attempt=deleted=fail_on=0;
 for(auto &task:tasks)assert(!task.live);
 Processor processor;assert(processor.createTask());
 XiaozhiApp app;owner=&app;app._audio_processor=&processor;
 if(fail_worker)fail_on=attempt+fail_worker;
 const bool ok=app.startWorkers();assert(ok==!fail_worker);
 assert(app.shutdownAudio()); /* Also safe after creation-failure cleanup. */
 assert(!app._input_task && !app._encoder_task && !app._playback_task);
 assert(!app._audio_tasks_running.load());
 assert(internal_free==3563 && psram_free==200*1024);
 assert(deleted==(fail_worker?fail_worker:4));
 for(auto &task:tasks)assert(!task.live);
 owner=nullptr;
}
int main() {
 for(unsigned repeat=0;repeat<100;++repeat)for(unsigned fail=0;fail<=3;++fail)one_cycle(fail);
 puts("XIAOZHI_TASKS=PASS (3.5KB fragmented SRAM, PSRAM stacks, input/encoder/playback creation failures, owner cleanup, 400 repeat cycles without leaks)");
}
'''

def main():
    app=APP.read_text(encoding='utf-8')
    afe=AFE.read_text(encoding='utf-8')
    header=HEADER.read_text(encoding='utf-8')
    begin=app.index('    _audio_tasks_running.store(true);',app.index('bool XiaozhiApp::ensureAudioReady()'))
    end=app.index('bool XiaozhiApp::shutdownAudio()',begin)
    workers='bool XiaozhiApp::startWorkers() {\n'+app[begin:end]
    stop_start=end
    stop_end=app.index('    if (_opus_encoder)',stop_start)
    cleanup=app[stop_start:stop_end]+'    return true;\n}\n'
    afe_start=afe.index('    BaseType_t created = xTaskCreateWithCaps(')
    afe_end=afe.index('    if (created != pdPASS)',afe_start)
    constants='\n'.join(re.findall(r'    static constexpr int (?:INPUT|ENCODER|PLAYBACK)_TASK_STACK_SIZE = .*?;',header))
    assert len(constants.splitlines())==3
    # Flash/NVS work must retain its original internal-stack control task.
    assert re.search(r'xTaskCreatePinnedToCore\(\s*controlTaskEntry,',app)
    assert 'vTaskDeleteWithCaps(nullptr)' not in app
    for flag in ['_input_task_running','_encoder_task_running','_playback_task_running']:
        endings=re.findall(re.escape(flag)+r'\.store\(false\);\s*(vTask\w+)\(nullptr\);',app)
        assert endings and all(call=='vTaskSuspend' for call in endings)
    assert 'vTaskDeleteWithCaps(processing_task_)' in afe
    assert 'vTaskDelete(nullptr)' not in afe
    text=HARNESS.replace('__AFE_CREATE__',afe[afe_start:afe_end]).replace('__STACK_CONSTANTS__',constants)
    text=text.replace('__CREATE_WORKERS__',workers).replace('__CLEANUP_WORKERS__',cleanup)
    BUILD.mkdir(parents=True,exist_ok=True)
    (BUILD/'test.cpp').write_text(text,encoding='utf-8')
    flags=['--target=x86_64-pc-windows-msvc','-std=c++11','-O1','-D_CRT_SECURE_NO_WARNINGS']
    for path in [VC/'include',SDK/'Include/shared',SDK/'Include/um']:flags+=['-I',str(path)]
    subprocess.run([str(CLANG),*flags,'-c',str(BUILD/'test.cpp'),'-o',str(BUILD/'test.obj')],check=True)
    subprocess.run([str(LLD),'-flavor','link',str(BUILD/'test.obj'),'/out:'+str(BUILD/'test.exe'),
        '/subsystem:console','/defaultlib:libcmt','/defaultlib:oldnames','/defaultlib:kernel32',
        '/libpath:'+str(VC/'lib/amd64'),'/libpath:'+str(SDK/'Lib/winv6.3/um/x64')],check=True)
    result=subprocess.run([str(BUILD/'test.exe')],capture_output=True,text=True,check=True)
    print(result.stdout.strip())
    (ROOT/'output/watchface/xiaozhi-task-verification.txt').write_text(result.stdout,encoding='utf-8')

if __name__=='__main__':main()
