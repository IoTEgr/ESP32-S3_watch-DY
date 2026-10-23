#include "watch_power.h"
#include "watch_power_sleep.h"
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG="WatchPower";
static SemaphoreHandle_t s_mutex;
static esp_pm_lock_handle_t s_cpu,s_awake;
static atomic_bool s_screen_on=true;
static int64_t s_screen_since_us;
static unsigned s_audio_users;
static bool s_ready,s_locked;
static int s_wifi_reported_mode=-1;
static esp_err_t s_wifi_last_error=ESP_OK;

/* Task context, serialized with screen/audio changes. Wi-Fi PS is volatile:
 * this path never changes STA credentials or writes NVS. */
static void update_wifi_locked(void)
{
    wifi_mode_t mode=WIFI_MODE_NULL;
    esp_err_t ret=esp_wifi_get_mode(&mode);
    if(ret==ESP_ERR_WIFI_NOT_INIT) {
        s_wifi_reported_mode=-1;s_wifi_last_error=ESP_OK;return;
    }
    if(ret==ESP_OK && mode!=WIFI_MODE_STA && mode!=WIFI_MODE_APSTA)return;
    const wifi_ps_type_t desired=s_audio_users?WIFI_PS_NONE:
        (atomic_load(&s_screen_on)?WIFI_PS_MIN_MODEM:WIFI_PS_MAX_MODEM);
    wifi_ps_type_t actual=WIFI_PS_NONE;
    if(ret==ESP_OK)ret=esp_wifi_get_ps(&actual);
    if(ret==ESP_OK && actual!=desired)ret=esp_wifi_set_ps(desired);
    if(ret!=ESP_OK) {
        if(s_wifi_last_error!=ret)ESP_LOGW(TAG,"WiFiPower: update failed: %s",esp_err_to_name(ret));
        s_wifi_last_error=ret;return;
    }
    s_wifi_last_error=ESP_OK;
    if(s_wifi_reported_mode!=(int)desired) {
        s_wifi_reported_mode=(int)desired;
        ESP_LOGI(TAG,"WiFiPower: ps=%s screen=%s audio=%u (STA listen interval=%u beacons)",
                 desired==WIFI_PS_NONE?"NONE":desired==WIFI_PS_MIN_MODEM?"MIN_MODEM":"MAX_MODEM",
                 atomic_load(&s_screen_on)?"on":"off",s_audio_users,WATCH_POWER_WIFI_LISTEN_INTERVAL);
    }
}

static void update_locked(void)
{
    const bool busy=atomic_load(&s_screen_on) || s_audio_users!=0;
    if(busy==s_locked){update_wifi_locked();return;}
    if(busy) {
        ESP_ERROR_CHECK(esp_pm_lock_acquire(s_cpu));
        ESP_ERROR_CHECK(esp_pm_lock_acquire(s_awake));
    } else {
        ESP_ERROR_CHECK(esp_pm_lock_release(s_awake));
        ESP_ERROR_CHECK(esp_pm_lock_release(s_cpu));
    }
    s_locked=busy;
    update_wifi_locked();
}

esp_err_t watch_power_init(void)
{
    if(s_ready)return ESP_OK;
    s_mutex=xSemaphoreCreateMutex();
    if(!s_mutex)return ESP_ERR_NO_MEM;
    s_screen_since_us=esp_timer_get_time();
    esp_err_t ret=esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX,0,"watch_active",&s_cpu);
    if(ret==ESP_OK)ret=esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP,0,"watch_awake",&s_awake);
    if(ret==ESP_OK) {
        update_locked();
        const esp_pm_config_t cfg={.max_freq_mhz=240,.min_freq_mhz=20,.light_sleep_enable=true};
        ret=esp_pm_configure(&cfg);
    }
    if(ret!=ESP_OK) {
        if(s_locked) {esp_pm_lock_release(s_awake);esp_pm_lock_release(s_cpu);s_locked=false;}
        if(s_awake)esp_pm_lock_delete(s_awake);
        if(s_cpu)esp_pm_lock_delete(s_cpu);
        s_cpu=NULL;s_awake=NULL;
        vSemaphoreDelete(s_mutex);s_mutex=NULL;
        return ret;
    }
    s_ready=true;
    ESP_LOGI(TAG,"Policy ready: active=240MHz, idle minimum=20MHz + automatic Light-sleep");
    return ESP_OK;
}

void watch_power_set_screen_on(bool on)
{
    if(!s_ready){atomic_store(&s_screen_on,on);return;}
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    const bool changed=atomic_exchange(&s_screen_on,on)!=on;
    if(changed)s_screen_since_us=esp_timer_get_time();
    update_locked();
#if WATCH_DEBUG_ENABLED
    if(changed)watch_power_sleep_set_screen_on(on);
#endif
    const unsigned users=s_audio_users;
    xSemaphoreGive(s_mutex);
    if(changed)ESP_LOGI(TAG,"Screen %s: audio owners=%u, policy=%s",on?"on":"off",users,
                        on||users?"240MHz / awake":"20MHz minimum / auto Light-sleep allowed");
}

bool watch_power_is_screen_on(void){return atomic_load(&s_screen_on);}

watch_power_screen_state_t watch_power_get_screen_state(void)
{
    if(!s_ready)return (watch_power_screen_state_t){atomic_load(&s_screen_on),0};
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    const watch_power_screen_state_t state={atomic_load(&s_screen_on),s_screen_since_us};
    xSemaphoreGive(s_mutex);
    return state;
}

void watch_power_poll(void)
{
    // Wi-Fi may initialize after this module, or be restarted from Settings.
    // Reconcile its actual driver mode using the existing 1s status worker.
    if(s_ready) {
        xSemaphoreTake(s_mutex,portMAX_DELAY);
        update_wifi_locked();
        xSemaphoreGive(s_mutex);
    }
#if WATCH_DEBUG_ENABLED
    watch_power_sleep_poll();
#endif
}

void watch_power_audio_begin(void)
{
    if(!s_ready)return;
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    ++s_audio_users;update_locked();
    xSemaphoreGive(s_mutex);
}

void watch_power_audio_end(void)
{
    if(!s_ready)return;
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    if(s_audio_users)--s_audio_users;
    update_locked();
    xSemaphoreGive(s_mutex);
}
