/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_CONDITION_LEN   24
#define WEATHER_ICON_LEN        12

typedef struct {
    bool valid;
    int temp_c;                            /* rounded current temperature */
    int humidity;                          /* percent, -1 when unknown */
    char condition[WEATHER_CONDITION_LEN]; /* 多云 */
    char icon[WEATHER_ICON_LEN];           /* yiketianqi wea_img: qing/yun/... */
    char city[WEATHER_CONDITION_LEN];
    int64_t fetched_ms;                    /* esp_timer timestamp of the last success */
} weather_snapshot_t;

/**
 * Start the background fetcher. Safe to call once during boot.
 *
 * The service waits for the station to get an IP address, then polls the
 * Yiketianqi endpoint (the one used by IoTEgr/reportWeather) every 30 minutes.
 * The last good answer is cached in NVS so the face has data right after a
 * reboot, before the first request completes.
 */
esp_err_t weather_service_start(void);

/** Override the city id; persisted asynchronously in NVS. Default: 101200901 (Yichang). */
esp_err_t weather_service_set_city(const char *cityid);

/** Copy the latest snapshot. Returns false while nothing has been fetched yet. */
bool weather_service_get(weather_snapshot_t *out);

/** Force a refresh on the next worker iteration. */
void weather_service_request_refresh(void);

/** Flush pending cache writes. Call from the internal-stack system-status
 * worker only, outside other locks; never from the PSRAM weather worker. */
esp_err_t weather_service_flush_cache(void);

#ifdef __cplusplus
}
#endif
