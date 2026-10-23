#include "watch_power_gate.h"
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* Count scheduler/API calls without changing SDK policy or wake timers.
 * Do not infer lock residency from wrappers outside the SDK's mode spinlock:
 * concurrent calls can be delayed/reordered across a real sleep interval. */
static DRAM_ATTR portMUX_TYPE s_mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR unsigned s_generation;
static DRAM_ATTR uint64_t s_core_api[CONFIG_FREERTOS_NUMBER_OF_CORES];
static DRAM_ATTR watch_power_gate_stats_t s_window;
static DRAM_ATTR bool s_measuring;

/* Observe the exact callback registered by wifi_init.c. Preserve its result:
 * bypassing a TSF veto could interrupt an active radio transaction. The linker
 * wraps its external reference, including callback registration/unregistration.
 * This runs under the SDK's PM spinlock: no logging, allocation or driver APIs.
 * Counts describe decisions, not the fraction of wall time blocked by Wi-Fi. */
bool __real_esp_wifi_internal_is_tsf_active(void);
bool IRAM_ATTR __wrap_esp_wifi_internal_is_tsf_active(void)
{
    const bool active=__real_esp_wifi_internal_is_tsf_active();
    portENTER_CRITICAL_ISR(&s_mux);
    if(s_measuring) {
        ++s_window.wifi_tsf_checks;
        if(active)++s_window.wifi_tsf_veto;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
    return active;
}

void IRAM_ATTR watch_power_gate_sleep_attempt(void)
{
    const unsigned core=(unsigned)xPortGetCoreID();
    portENTER_CRITICAL_ISR(&s_mux);
    ++s_core_api[core];
    if(s_measuring)++s_window.api;
    portEXIT_CRITICAL_ISR(&s_mux);
}

void __real_vApplicationSleep(TickType_t);
void IRAM_ATTR __wrap_vApplicationSleep(TickType_t ticks)
{
    const unsigned core=(unsigned)xPortGetCoreID();
    // This API is already used by the SDK's tickless sleep implementation.
    const int64_t alarm_us=esp_timer_get_next_alarm_for_wake_up()-esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_mux);
    const unsigned generation=s_generation;
    const bool measuring=s_measuring;
    const uint64_t before=s_core_api[core];
    if(measuring) {
        ++s_window.tickless;
        if(ticks<s_window.min_ticks)s_window.min_ticks=(unsigned)ticks;
        if(ticks>s_window.max_ticks)s_window.max_ticks=(unsigned)ticks;
        if(alarm_us<(int64_t)configEXPECTED_IDLE_TIME_BEFORE_SLEEP*portTICK_PERIOD_MS*1000)
            ++s_window.short_alarm;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
    __real_vApplicationSleep(ticks);
    portENTER_CRITICAL_ISR(&s_mux);
    if(measuring && s_measuring && generation==s_generation && before==s_core_api[core])
        ++s_window.skipped;
    portEXIT_CRITICAL_ISR(&s_mux);
}

void watch_power_gate_start(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(&s_window,0,sizeof(s_window));s_window.min_ticks=UINT_MAX;
    ++s_generation;s_measuring=true;
    portEXIT_CRITICAL(&s_mux);
}

void watch_power_gate_stop(void)
{
    portENTER_CRITICAL(&s_mux);
    s_measuring=false;
    portEXIT_CRITICAL(&s_mux);
}

void watch_power_gate_snapshot(watch_power_gate_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out=s_window;
    if(!out->tickless)out->min_ticks=0;
    portEXIT_CRITICAL(&s_mux);
}

void watch_power_gate_report(const watch_power_gate_stats_t *s)
{
    if(!s->tickless && !s->api)return;
    ESP_LOGI("PowerGate","tickless=%llu api=%llu skipped=%llu short_alarm_at_entry=%llu idle_ticks=%u..%u "
             "wifi_tsf_checks=%llu wifi_tsf_veto=%llu "
             "(call counts; skipped is not a sleep error)",
             (unsigned long long)s->tickless,(unsigned long long)s->api,
             (unsigned long long)s->skipped,(unsigned long long)s->short_alarm,s->min_ticks,s->max_ticks,
             (unsigned long long)s->wifi_tsf_checks,(unsigned long long)s->wifi_tsf_veto);
}
