/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "weather_service.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "Weather";

// Optional local credentials are deliberately excluded from the public repository.
#if __has_include("weather_config.local.h")
#include "weather_config.local.h"
#endif
#ifndef WATCH_WEATHER_APPID
#define WATCH_WEATHER_APPID ""
#endif
#ifndef WATCH_WEATHER_APPSECRET
#define WATCH_WEATHER_APPSECRET ""
#endif
static const char *WEATHER_URL =
    "http://v1.yiketianqi.com/api?unescape=1&version=v9"
    "&appid=" WATCH_WEATHER_APPID "&appsecret=" WATCH_WEATHER_APPSECRET;
static const char *DEFAULT_CITY_ID = "101200901";  // Yichang
static const char *NVS_NAMESPACE = "wx";
static const char *NVS_CITY_KEY = "cityid";
static const char *NVS_CACHE_KEY = "cache";

#define REFRESH_INTERVAL_MS     (30 * 60 * 1000)
#define RETRY_INTERVAL_MS       (60 * 1000)
#define NO_NET_INTERVAL_MS      (5 * 1000)
#define HTTP_TIMEOUT_MS         12000
#define RESPONSE_BUFFER_SIZE    (32 * 1024)
#define WEATHER_TASK_STACK_BYTES (6 * 1024)

static weather_snapshot_t s_snapshot;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static char s_city_id[16];
static volatile bool s_refresh_now = false;
static atomic_bool s_cache_dirty;

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void copy_bounded(char *dst, size_t size, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, size, "%s", src);
}

static void load_cache(void)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    /* Migrate existing watches once; changing only the default leaves the
     * previous Shenzhen city and weather snapshot in NVS. */
    uint8_t city_revision = 0;
    nvs_get_u8(handle, "city_rev", &city_revision);
    if (city_revision != 1) {
        nvs_close(handle);
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_str(handle, NVS_CITY_KEY, DEFAULT_CITY_ID);
            nvs_set_u8(handle, "city_rev", 1);
            nvs_erase_key(handle, NVS_CACHE_KEY);
            nvs_commit(handle);
            nvs_close(handle);
        }
        return;
    }
    weather_snapshot_t cached;
    size_t length = sizeof(cached);
    if (nvs_get_blob(handle, NVS_CACHE_KEY, &cached, &length) == ESP_OK &&
            length == sizeof(cached) && cached.valid) {
        lock();
        s_snapshot = cached;
        unlock();
        ESP_LOGI(TAG, "Cached %d C, %s (%s)", cached.temp_c, cached.condition, cached.icon);
    }

    char city[sizeof(s_city_id)];
    length = sizeof(city);
    if (nvs_get_str(handle, NVS_CITY_KEY, city, &length) == ESP_OK && city[0] != '\0') {
        copy_bounded(s_city_id, sizeof(s_city_id), city);
    }
    nvs_close(handle);
}

static void save_cache(void)
{
    // Fetching runs on a PSRAM stack. Defer Flash writes to the existing
    // system-status worker, whose internal stack can safely use NVS.
    atomic_store(&s_cache_dirty,true);
}

esp_err_t weather_service_flush_cache(void)
{
    if(!atomic_exchange(&s_cache_dirty,false))return ESP_OK;
    weather_snapshot_t cached;
    char city[sizeof(s_city_id)];
    lock();
    cached=s_snapshot;
    copy_bounded(city,sizeof(city),s_city_id);
    unlock();

    nvs_handle_t handle = 0;
    esp_err_t result=nvs_open(NVS_NAMESPACE,NVS_READWRITE,&handle);
    if(result==ESP_OK) {
        result=nvs_set_blob(handle,NVS_CACHE_KEY,&cached,sizeof(cached));
        if(result==ESP_OK)result=nvs_set_str(handle,NVS_CITY_KEY,city);
        if(result==ESP_OK)result=nvs_set_u8(handle,"city_rev",1);
        if(result==ESP_OK)result=nvs_commit(handle);
        nvs_close(handle);
    }
    if(result!=ESP_OK)atomic_store(&s_cache_dirty,true);
    return result;
}

static bool station_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return false;
    }
    return info.ip.addr != 0;
}

typedef struct {
    char *buffer;
    size_t length;
    size_t capacity;
    bool overflow;
} fetch_context_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    fetch_context_t *context = (fetch_context_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && context != NULL) {
        if (context->length + event->data_len < context->capacity) {
            memcpy(context->buffer + context->length, event->data, event->data_len);
            context->length += event->data_len;
            context->buffer[context->length] = '\0';
        } else {
            context->overflow = true;
        }
    }
    return ESP_OK;
}

static void map_icon(const char *wea_img, char *out, size_t size)
{
    if (wea_img == NULL) {
        copy_bounded(out, size, "yin");
        return;
    }
    // The API uses: qing, yun, yin, yu, lei, xue, wu, sha, bing.
    if (strcmp(wea_img, "qing") == 0) {
        copy_bounded(out, size, "qing");
    } else if (strcmp(wea_img, "yun") == 0) {
        copy_bounded(out, size, "yun");
    } else if (strcmp(wea_img, "yu") == 0) {
        copy_bounded(out, size, "yu");
    } else if (strcmp(wea_img, "lei") == 0 || strcmp(wea_img, "leiyu") == 0) {
        copy_bounded(out, size, "lei");
    } else if (strcmp(wea_img, "xue") == 0) {
        copy_bounded(out, size, "xue");
    } else if (strcmp(wea_img, "wu") == 0 || strcmp(wea_img, "wumai") == 0 ||
               strcmp(wea_img, "sha") == 0) {
        copy_bounded(out, size, "wu");
    } else if (strcmp(wea_img, "bing") == 0) {
        copy_bounded(out, size, "xue");
    } else {
        copy_bounded(out, size, "yin");
    }
}

static bool parse_response(const char *json, weather_snapshot_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "Response is not valid JSON");
        return false;
    }

    bool ok = false;
    if (cJSON_GetObjectItem(root, "errcode") != NULL) {
        cJSON *message = cJSON_GetObjectItem(root, "errmsg");
        ESP_LOGW(TAG, "API error: %s", cJSON_IsString(message) ? message->valuestring : "?");
        cJSON_Delete(root);
        return false;
    }

    cJSON *city = cJSON_GetObjectItem(root, "city");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *today = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
    if (today != NULL) {
        cJSON *tem = cJSON_GetObjectItem(today, "tem");
        cJSON *wea = cJSON_GetObjectItem(today, "wea");
        cJSON *img = cJSON_GetObjectItem(today, "wea_img");
        cJSON *humidity = cJSON_GetObjectItem(today, "humidity");

        if (cJSON_IsString(tem)) {
            out->temp_c = (int)lroundf(strtof(tem->valuestring, NULL));
            out->valid = true;
            ok = true;
        }
        copy_bounded(out->condition, sizeof(out->condition),
                     cJSON_IsString(wea) ? wea->valuestring : "");
        map_icon(cJSON_IsString(img) ? img->valuestring : NULL,
                 out->icon, sizeof(out->icon));
        out->humidity = cJSON_IsString(humidity) ? atoi(humidity->valuestring) : -1;
    }
    copy_bounded(out->city, sizeof(out->city),
                 cJSON_IsString(city) ? city->valuestring : "");

    cJSON_Delete(root);
    return ok;
}

static bool fetch_once(void)
{
    if (!WATCH_WEATHER_APPID[0] || !WATCH_WEATHER_APPSECRET[0]) {
        ESP_LOGW(TAG, "Weather credentials are not configured; keeping cached weather");
        return false;
    }
    fetch_context_t *context = (fetch_context_t *)heap_caps_malloc(
                                   sizeof(fetch_context_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context == NULL) {
        context = (fetch_context_t *)malloc(sizeof(fetch_context_t));
    }
    if (context == NULL) {
        return false;
    }

    char *buffer = (char *)heap_caps_malloc(
                       RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        free(context);
        ESP_LOGW(TAG, "No PSRAM for the response buffer");
        return false;
    }

    context->buffer = buffer;
    context->length = 0;
    context->capacity = RESPONSE_BUFFER_SIZE - 1;
    context->overflow = false;

    char url[192];
    lock();
    snprintf(url, sizeof(url), "%s&cityid=%s", WEATHER_URL, s_city_id);
    unlock();

    esp_http_client_config_t config = {0};
    config.url = url;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.event_handler = http_event_handler;
    config.user_data = context;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(buffer);
        free(context);
        return false;
    }

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        const int status = esp_http_client_get_status_code(client);
        if (status == 200 && context->length > 0 && !context->overflow) {
            weather_snapshot_t parsed = {0};
            if (parse_response(buffer, &parsed)) {
                parsed.fetched_ms = esp_timer_get_time() / 1000;
                lock();
                s_snapshot = parsed;
                unlock();
                save_cache();
                ESP_LOGI(TAG, "%d C, %s (%s), %s, humidity %d%%",
                         parsed.temp_c, parsed.condition, parsed.icon, parsed.city,
                         parsed.humidity);
                ok = true;
            }
        } else {
            ESP_LOGW(TAG, "HTTP %d, %u bytes%s", status,
                     (unsigned)context->length,
                     context->overflow ? " (truncated)" : "");
        }
    } else {
        ESP_LOGW(TAG, "Request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(buffer);
    free(context);
    return ok;
}

static void weather_task(void *arg)
{
    (void)arg;
    // Give the station a moment to come up before the first attempt.
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (true) {
        uint32_t wait_ms = NO_NET_INTERVAL_MS;

        if (!station_has_ip()) {
            wait_ms = NO_NET_INTERVAL_MS;
        } else if (fetch_once()) {
            wait_ms = REFRESH_INTERVAL_MS;
        } else {
            wait_ms = RETRY_INTERVAL_MS;
        }

        if (s_refresh_now) {
            s_refresh_now = false;
            wait_ms = 1000;
        }

        // Sleep in one second slices so a manual refresh takes effect quickly.
        while (wait_ms > 0) {
            const uint32_t slice = (wait_ms > 1000) ? 1000 : wait_ms;
            vTaskDelay(pdMS_TO_TICKS(slice));
            wait_ms -= slice;
            if (s_refresh_now) {
                s_refresh_now = false;
                break;
            }
        }
    }
}

esp_err_t weather_service_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    copy_bounded(s_city_id, sizeof(s_city_id), DEFAULT_CITY_ID);
    load_cache();

    if (xTaskCreateWithCaps(weather_task, "weather", WEATHER_TASK_STACK_BYTES, NULL, 3, &s_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG,"Create weather task failed: internal free=%u largest=%u PSRAM free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Started, city %s (stack=%u in PSRAM)", s_city_id, WEATHER_TASK_STACK_BYTES);
    return ESP_OK;
}

esp_err_t weather_service_set_city(const char *cityid)
{
    if (cityid == NULL || cityid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    copy_bounded(s_city_id, sizeof(s_city_id), cityid);
    unlock();
    save_cache();
    s_refresh_now = true;
    return ESP_OK;
}

bool weather_service_get(weather_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }
    lock();
    *out = s_snapshot;
    unlock();
    return s_snapshot.valid;
}

void weather_service_request_refresh(void)
{
    s_refresh_now = true;
}
