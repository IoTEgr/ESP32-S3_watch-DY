#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include "watch_power_sleep.h"
#include "watch_power_tasks.h"
#include "watch_power_gate.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

static const char *TAG="PowerSleep";
static DRAM_ATTR portMUX_TYPE s_stats_mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR uint64_t s_success_us,s_success_calls,s_rejected_calls;
static DRAM_ATTR uint64_t s_short_calls,s_wake_reject_calls,s_other_calls;
static DRAM_ATTR esp_err_t s_last_error;

/* GNU ld --wrap redirects existing SDK callers here. Count successful API
 * intervals, as IDF's PM implementation does; exclude rejected attempts.
 * These intervals include entry/exit overhead, so they estimate residency
 * and do not measure battery current or exact hardware sleep-only time. */
esp_err_t __real_esp_light_sleep_start(void);
esp_err_t IRAM_ATTR __wrap_esp_light_sleep_start(void)
{
    watch_power_gate_sleep_attempt();
    const int64_t start=esp_timer_get_time();
    const esp_err_t result=__real_esp_light_sleep_start();
    const int64_t elapsed=esp_timer_get_time()-start;
    portENTER_CRITICAL_ISR(&s_stats_mux);
    if(result==ESP_OK) {
        if(elapsed>0)s_success_us+=(uint64_t)elapsed;
        ++s_success_calls;
    } else {
        ++s_rejected_calls;
        if(result==ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION)++s_short_calls;
        else if(result==ESP_ERR_SLEEP_REJECT)++s_wake_reject_calls;
        else ++s_other_calls;
        s_last_error=result;
    }
    portEXIT_CRITICAL_ISR(&s_stats_mux);
    return result;
}

typedef struct {
    uint64_t off_us,sleep_call_us,successes,rejected;
    unsigned lock_samples,locks_min,locks_max;
    unsigned apb_min,apb_max;
    unsigned usb_polls,battery_polls,stats_errors,usb_lock_samples,usb_locks_min,usb_locks_max;
    uint64_t short_calls,wake_reject_calls,other_calls;
    esp_err_t last_error;
    watch_power_gate_stats_t gate;
    bool valid;
} sleep_report_t;

static bool s_off,s_pending,s_usb_previous;
static int64_t s_off_started;
static uint64_t s_start_us,s_start_calls,s_start_rejected;
static unsigned s_lock_samples,s_locks_min=UINT_MAX,s_locks_max;
static unsigned s_apb_min=UINT_MAX,s_apb_max;
static unsigned s_usb_polls,s_battery_polls,s_stats_errors,s_usb_lock_samples,s_usb_locks_min=UINT_MAX,s_usb_locks_max;
static uint64_t s_start_short,s_start_wake_reject,s_start_other;
static sleep_report_t s_last;
static sleep_report_t s_reconnect_report;
static unsigned s_replays;
static int64_t s_next_replay;

/* Called with the stats spinlock held. */
static sleep_report_t snapshot_locked(int64_t now)
{
    sleep_report_t r={0};
    if(!s_off || now<=s_off_started)return r;
    r.off_us=(uint64_t)(now-s_off_started);
    r.sleep_call_us=s_success_us-s_start_us;
    r.successes=s_success_calls-s_start_calls;
    r.rejected=s_rejected_calls-s_start_rejected;
    r.lock_samples=s_lock_samples;
    r.locks_min=s_lock_samples?s_locks_min:0;
    r.locks_max=s_locks_max;
    r.apb_min=s_lock_samples?s_apb_min:0;r.apb_max=s_apb_max;
    r.usb_polls=s_usb_polls;r.battery_polls=s_battery_polls;r.stats_errors=s_stats_errors;
    r.usb_lock_samples=s_usb_lock_samples;
    r.usb_locks_min=s_usb_lock_samples?s_usb_locks_min:0;r.usb_locks_max=s_usb_locks_max;
    r.short_calls=s_short_calls-s_start_short;
    r.wake_reject_calls=s_wake_reject_calls-s_start_wake_reject;
    r.other_calls=s_other_calls-s_start_other;
    r.last_error=r.rejected?s_last_error:ESP_OK;
    watch_power_gate_snapshot(&r.gate);
    r.valid=true;
    return r;
}

void watch_power_sleep_set_screen_on(bool on)
{
    const int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_stats_mux);
    if(!on && !s_off) {
        s_off=true;s_off_started=now;
        watch_power_gate_start();
        s_start_us=s_success_us;s_start_calls=s_success_calls;s_start_rejected=s_rejected_calls;
        s_lock_samples=0;s_locks_min=UINT_MAX;s_locks_max=0;
        s_apb_min=UINT_MAX;s_apb_max=0;
        s_usb_polls=s_battery_polls=s_stats_errors=s_usb_lock_samples=s_usb_locks_max=0;
        s_usb_locks_min=UINT_MAX;
        s_start_short=s_short_calls;s_start_wake_reject=s_wake_reject_calls;s_start_other=s_other_calls;
    } else if(on && s_off) {
        s_last=snapshot_locked(now);s_pending=s_last.valid;s_off=false;
        watch_power_gate_stop();
    }
    portEXIT_CRITICAL(&s_stats_mux);
}

void watch_power_sleep_poll(void)
{
    const bool usb=usb_serial_jtag_is_connected();
    esp_pm_lock_stats_t locks[ESP_PM_LOCK_MAX];
    const esp_err_t stats_result=esp_pm_get_lock_stats_all(locks);
    portENTER_CRITICAL(&s_stats_mux);
    if(s_off) {
        if(usb)++s_usb_polls;else ++s_battery_polls;
        if(stats_result!=ESP_OK)++s_stats_errors;
        else {
            const unsigned held=(unsigned)locks[ESP_PM_NO_LIGHT_SLEEP].acquired;
            // Keep USB-connected samples separate: USB adds its own sleep lock.
            if(usb) {
                if(held<s_usb_locks_min)s_usb_locks_min=held;
                if(held>s_usb_locks_max)s_usb_locks_max=held;
                ++s_usb_lock_samples;
            } else {
                const unsigned apb=(unsigned)locks[ESP_PM_APB_FREQ_MAX].acquired;
                if(held<s_locks_min)s_locks_min=held;
                if(held>s_locks_max)s_locks_max=held;
                if(apb<s_apb_min)s_apb_min=apb;
                if(apb>s_apb_max)s_apb_max=apb;
                ++s_lock_samples;
            }
        }
    }
    portEXIT_CRITICAL(&s_stats_mux);

    sleep_report_t report={0};
    bool replayed=false;
    const int64_t now=esp_timer_get_time();
    const bool reconnect=usb && !s_usb_previous;
    portENTER_CRITICAL(&s_stats_mux);
    const bool off=s_off;
    portEXIT_CRITICAL(&s_stats_mux);
    // Snapshot tasks outside the sleep/PM spinlocks, from this normal worker.
    watch_power_tasks_poll(off,usb);
    portENTER_CRITICAL(&s_stats_mux);
    if(usb) {
        if(reconnect && s_off)report=snapshot_locked(now);
        else if(s_pending)report=s_last;
        else if(reconnect && s_last.valid)report=s_last;
        if(reconnect && report.valid) {
            // USB SOF can resume before the terminal is opened. Retain this
            // battery measurement and repeat briefly, without writing Flash.
            s_reconnect_report=report;s_replays=5;s_next_replay=now+5000000;
        }
        if(!report.valid && s_replays && now>=s_next_replay) {
            report=s_reconnect_report;--s_replays;s_next_replay=now+5000000;replayed=true;
        }
        if(report.valid)s_pending=false;
    } else {
        s_replays=0;
    }
    portEXIT_CRITICAL(&s_stats_mux);
    s_usb_previous=usb;
    if(!report.valid)return;

    uint64_t ratio=report.sleep_call_us*100/report.off_us;
    if(ratio>100)ratio=100;
    ESP_LOGI(TAG,"off=%llums sleep_call=%llums sleep_est=%u%% ok=%llu rejected=%llu "
             "battery_lock_samples=%u no_sleep_locks=%u..%u replay=%u "
             "usb_polls=%u battery_polls=%u stats_errors=%u usb_lock_samples=%u usb_no_sleep_locks=%u..%u "
             "too_short=%llu wake_reject=%llu other_reject=%llu last_error=0x%x battery_apb_locks=%u..%u "
             "(lock ranges valid only with samples; successful calls include entry/exit overhead)",
             (unsigned long long)(report.off_us/1000),
             (unsigned long long)(report.sleep_call_us/1000),(unsigned)ratio,
             (unsigned long long)report.successes,(unsigned long long)report.rejected,
             report.lock_samples,report.locks_min,report.locks_max,(unsigned)replayed,
             report.usb_polls,report.battery_polls,report.stats_errors,report.usb_lock_samples,
             report.usb_locks_min,report.usb_locks_max,
             (unsigned long long)report.short_calls,(unsigned long long)report.wake_reject_calls,
             (unsigned long long)report.other_calls,(unsigned)report.last_error,report.apb_min,report.apb_max);
    watch_power_tasks_report();
    watch_power_gate_report(&report.gate);
}
