"""Production weather startup/fetch/cache with RTOS, HTTP, parser and NVS adapters.

No live weather endpoint is called. Real Wi-Fi/Flash behavior requires hardware.
"""
from render_lvgl import ROOT
from verify_boot_voice import function
from verify_standby_power import BUILD, compile_run

HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define snprintf _snprintf
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffu
#define atomic_store(p,v) ((p)->value=(v))
#define atomic_exchange(p,v) ((p)->exchange(v))
enum {ESP_OK=0,ESP_ERR_NO_MEM=1,ESP_ERR_INVALID_ARG=2,ESP_FAIL=3,pdPASS=1,pdFAIL=0,
 MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_INTERNAL=4,NVS_READWRITE=1,HTTP_EVENT_ON_DATA=1};
typedef int esp_err_t,nvs_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
struct Flag {bool value;bool exchange(bool v){bool old=value;value=v;return old;}};
#define WEATHER_CONDITION_LEN 24
#define WEATHER_ICON_LEN 12
#define WEATHER_TASK_STACK_BYTES (6 * 1024)
#define RESPONSE_BUFFER_SIZE (32 * 1024)
#define HTTP_TIMEOUT_MS 12000
#define WATCH_WEATHER_APPID "fixture-app"
#define WATCH_WEATHER_APPSECRET "fixture-secret"
__SNAPSHOT__
__FETCH_CONTEXT__
static weather_snapshot_t s_snapshot;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char s_city_id[16];
static bool s_refresh_now,internal_stack=true,fail_mutex,fail_task;
static Flag s_cache_dirty;
static const char *DEFAULT_CITY_ID="101200901",*WEATHER_URL="http://fixture.invalid",
 *NVS_NAMESPACE="wx",*NVS_CACHE_KEY="cache",*NVS_CITY_KEY="cityid";
static unsigned internal_free,psram_free,creates,loads,nvs_ops,fail_nvs,commits,handles;
static bool locked,race_commit;
static weather_snapshot_t persisted;
static char persisted_city[16];
static SemaphoreHandle_t xSemaphoreCreateMutex(){if(fail_mutex)return 0;internal_free-=96;return (void *)1;}
static void xSemaphoreTake(void *m,unsigned){assert(m && !locked);locked=true;}
static void xSemaphoreGive(void *m){assert(m && locked);locked=false;}
static unsigned heap_caps_get_free_size(unsigned caps){return caps&MALLOC_CAP_SPIRAM?psram_free:internal_free;}
static unsigned heap_caps_get_largest_free_block(unsigned){return 1536;}
static void weather_task(void *){}
static int xTaskCreateWithCaps(TaskFunction_t fn,const char *name,unsigned stack,void *,unsigned pri,void **out,unsigned caps){
 assert(fn==weather_task && strcmp(name,"weather")==0 && stack==6144 && pri==3);
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) && stack>1536 && internal_free>=512);
 if(fail_task)return pdFAIL;
 internal_free-=512;psram_free-=stack;*out=(void *)2;++creates;return pdPASS;
}
static void load_cache(){assert(internal_stack);++loads;s_snapshot.valid=true;s_snapshot.temp_c=16;strcpy(s_snapshot.city,"Yichang");}
__LOCKS__
static int nvs_op(){assert(internal_stack && !locked);return ++nvs_ops==fail_nvs?ESP_FAIL:ESP_OK;}
static int nvs_open(const char *,int,nvs_handle_t *h){int r=nvs_op();if(!r){*h=1;++handles;}return r;}
static int nvs_set_blob(int,const char *,const void *p,size_t size){assert(size==sizeof(persisted));
 int r=nvs_op();if(!r)memcpy(&persisted,p,size);return r;}
static int nvs_set_str(int,const char *,const char *p){int r=nvs_op();if(!r)strcpy(persisted_city,p);return r;}
static int nvs_set_u8(int,const char *,unsigned value){assert(value==1);return nvs_op();}
static int nvs_commit(int){int r=nvs_op();if(!r)++commits;
 if(race_commit){race_commit=false;s_snapshot.temp_c=21;atomic_store(&s_cache_dirty,true);}return r;}
static void nvs_close(int){assert(internal_stack && handles);--handles;}
__CACHE__
struct Allocation {void *p;size_t size;};
static Allocation allocs[4];static unsigned live_allocs,fail_alloc;static bool fail_buffer;
static void *test_malloc(size_t size){
 if(fail_alloc){--fail_alloc;return 0;}
 for(auto &a:allocs)if(!a.p){a.p=malloc(size);assert(a.p);a.size=size;++live_allocs;return a.p;}
 assert(false);return 0;
}
static void test_free(void *p){if(!p)return;for(auto &a:allocs)if(a.p==p){free(a.p);a.p=0;--live_allocs;return;}assert(false);}
static void *heap_caps_malloc(size_t size,unsigned caps){assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(fail_buffer && size==RESPONSE_BUFFER_SIZE)return 0;return test_malloc(size);}
#define malloc test_malloc
#define free test_free
struct esp_http_client_event_t {int event_id;void *user_data;char *data;int data_len;};
struct esp_http_client_config_t {const char *url;int timeout_ms;
 int (*event_handler)(esp_http_client_event_t *);void *user_data;};
typedef esp_http_client_config_t *esp_http_client_handle_t;
static esp_http_client_config_t http;
static bool fail_client,fail_http,fail_parse,overflow;
static int status=200,client_live;
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c){
 assert(!internal_stack && !locked && strstr(c->url,"cityid=101200901"));
 if(fail_client)return 0;http=*c;++client_live;return &http;}
static int esp_http_client_perform(esp_http_client_handle_t c){if(fail_http)return ESP_FAIL;
 char json[]="fixture";esp_http_client_event_t e={HTTP_EVENT_ON_DATA,c->user_data,json,overflow?RESPONSE_BUFFER_SIZE:7};
 return c->event_handler(&e);}
static int esp_http_client_get_status_code(esp_http_client_handle_t){return status;}
static void esp_http_client_cleanup(esp_http_client_handle_t){assert(client_live==1);--client_live;}
static const char *esp_err_to_name(int){return "mock";}
static int64_t esp_timer_get_time(){return 1000000;}
static bool parse_response(const char *,weather_snapshot_t *p){if(fail_parse)return false;
 p->valid=true;p->temp_c=19;strcpy(p->city,"Yichang");return true;}
__FETCH__
__START__
__PUBLIC__
static void reset(){
 s_task=s_lock=0;memset(&s_snapshot,0,sizeof(s_snapshot));s_cache_dirty.value=false;
 s_refresh_now=false;internal_stack=true;fail_mutex=fail_task=race_commit=false;
 internal_free=2463;psram_free=5*1024*1024;creates=loads=nvs_ops=fail_nvs=commits=handles=0;
 fail_alloc=0;assert(!live_allocs && !client_live);fail_buffer=fail_client=fail_http=fail_parse=overflow=false;status=200;
}
int main(){
 reset();fail_mutex=true;assert(weather_service_start()==ESP_ERR_NO_MEM && !s_task && !loads);
 reset();fail_task=true;assert(weather_service_start()==ESP_ERR_NO_MEM && !s_task && s_snapshot.valid);
 assert(internal_free==2463-96);fail_task=false;assert(weather_service_start()==0 && creates==1);
 reset();assert(weather_service_start()==0 && creates==1 && s_snapshot.temp_c==16);
 assert(internal_free==2463-96-512 && psram_free==5*1024*1024-6144);
 assert(weather_service_start()==0 && creates==1 && loads==1);
 puts("WEATHER_STARTUP=PASS (1.5KB largest SRAM block, 6KB PSRAM stack, cached data retained on task failure, successful retry/idempotent start)");
 for(int i=0;i<200;++i){internal_stack=false;assert(fetch_once());
  assert(s_snapshot.temp_c==19 && s_cache_dirty.value && !live_allocs && !client_live);
  unsigned previous=commits;internal_stack=true;assert(weather_service_flush_cache()==0 && commits==previous+1);
  assert(!s_cache_dirty.value && persisted.temp_c==19 && strcmp(persisted_city,"101200901")==0 && !handles);
 }
 for(unsigned f=1;f<=5;++f){internal_stack=false;save_cache();
  internal_stack=true;nvs_ops=0;fail_nvs=f;assert(weather_service_flush_cache()!=0 && s_cache_dirty.value && !handles);
  fail_nvs=0;assert(weather_service_flush_cache()==0 && !s_cache_dirty.value && !handles);
 }
 internal_stack=false;save_cache();internal_stack=true;race_commit=true;
 assert(weather_service_flush_cache()==0 && s_cache_dirty.value);
 assert(weather_service_flush_cache()==0 && persisted.temp_c==21 && !s_cache_dirty.value);
 internal_stack=false;assert(weather_service_set_city("101200901")==0 && s_refresh_now && s_cache_dirty.value);
 for(int f=0;f<7;++f){fail_alloc=f==0?2:0;fail_client=f==1;fail_http=f==2;fail_parse=f==3;
  overflow=f==4;status=f==5?500:200;fail_buffer=f==6;
  int old_temp=s_snapshot.temp_c;assert(!fetch_once());assert(s_snapshot.temp_c==old_temp && !live_allocs && !client_live);
 }
 fail_buffer=fail_client=fail_http=fail_parse=overflow=false;status=200;fail_alloc=1;
 assert(fetch_once() && !live_allocs && !client_live); // Context malloc fallback.
 puts("WEATHER_CACHE=PASS (fetch never writes Flash from PSRAM, internal-worker commits/retries, concurrent update retained, HTTP/parse/allocation failures preserve last good weather, 200 cycles)");
}
'''

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    src=(ROOT/'components/weather_service/weather_service.c').read_text(encoding='utf-8')
    header=(ROOT/'components/weather_service/include/weather_service.h').read_text(encoding='utf-8')
    snapshot=header[header.index('typedef struct {'):header.index('} weather_snapshot_t;')+len('} weather_snapshot_t;')]
    start=src.index('typedef struct {')
    context=src[start:src.index('} fetch_context_t;',start)+len('} fetch_context_t;')]
    source=HARNESS.replace('__SNAPSHOT__',snapshot).replace('__FETCH_CONTEXT__',context)
    sections={
        '__LOCKS__':['static void lock(', 'static void unlock(', 'static void copy_bounded('],
        '__CACHE__':['static void save_cache(', 'esp_err_t weather_service_flush_cache('],
        '__FETCH__':['static esp_err_t http_event_handler(', 'static bool fetch_once('],
        '__START__':['esp_err_t weather_service_start('],
        '__PUBLIC__':['esp_err_t weather_service_set_city(', 'bool weather_service_get('],
    }
    for marker,signatures in sections.items():
        source=source.replace(marker,''.join(function(src,s) for s in signatures))
    report=compile_run('weather_memory',source,True)
    main=(ROOT/'main/main.cpp').read_text(encoding='utf-8')
    assert 'ESP_UTILS_CHECK_ERROR_EXIT(weather_service_start()' not in main
    assert 'if(weather_result!=ESP_OK)' in main
    assert main.index('weather_service_start()')<main.index('brookesia::system_status::start(')
    status=(ROOT/'components/system_status/system_status.cpp').read_text(encoding='utf-8')
    assert 'weather_service_flush_cache()' in function(status,'void taskLoop(')
    assert 'xTaskCreate(taskEntry, "system_status", 4096' in status
    assert status.index('xTaskCreate(taskEntry')<status.index('result = applyWifiEnabledState(wifiDesiredRunning())')
    assert '_startup_ready.load()' in function(status,'void taskLoop(')
    print(report)
    (ROOT/'output/watchface/weather-memory-verification.txt').write_text(report,encoding='utf-8')

if __name__=='__main__':main()
