#pragma once
#include <stdint.h>

typedef struct {
    uint64_t tickless,api,skipped,short_alarm;
    uint64_t wifi_tsf_checks,wifi_tsf_veto;
    unsigned min_ticks,max_ticks;
} watch_power_gate_stats_t;

void watch_power_gate_start(void);
void watch_power_gate_stop(void);
void watch_power_gate_snapshot(watch_power_gate_stats_t *out);
void watch_power_gate_sleep_attempt(void);
void watch_power_gate_report(const watch_power_gate_stats_t *stats);
