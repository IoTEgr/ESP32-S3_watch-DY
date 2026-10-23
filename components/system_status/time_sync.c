#include "time_sync.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "pcf85063a.h"

static const char *TAG="TimeSync";
static const char *SERVERS[]={"ntp.aliyun.com","ntp.tencent.com","cn.pool.ntp.org"};
static atomic_bool s_received;
static bool s_timezone_ready,s_clock_ready,s_initialized,s_online,s_synced;
static uint32_t s_ip;
static unsigned s_attempt;
static int64_t s_retry_at;

void time_sync_init(void)
{
    if(s_timezone_ready)return;
    /* POSIX TZ uses the opposite sign: CST-8 means Shanghai / UTC+8.
     * settimeofday/SNTP always hold UTC; convert only through localtime_r. */
    if(setenv("TZ","CST-8",1)!=0) {
        ESP_LOGW(TAG,"Unable to initialize Shanghai timezone; will retry");
        return;
    }
    tzset();s_timezone_ready=true;
    ESP_LOGI(TAG,"Timezone: Asia/Shanghai (UTC+8, TZ=CST-8)");
}

static esp_err_t rtc_open(pcf85063a_dev_t *rtc)
{
    rtc->bus_handle=bsp_i2c_get_handle();
    if(!rtc->bus_handle)return ESP_ERR_INVALID_STATE;
    const i2c_device_config_t config={
        .dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=PCF85063A_ADDRESS,
        .scl_speed_hz=400000,
    };
    return i2c_master_bus_add_device(rtc->bus_handle,&config,&rtc->dev_handle);
}
static void restore_rtc(void)
{
    pcf85063a_dev_t rtc={0};
    if(rtc_open(&rtc)!=ESP_OK)return;
    uint8_t seconds=0;
    pcf85063a_datetime_t date={0};
    const bool ok=pcf85063a_read_register(&rtc,PCF85063A_RTC_SECOND_ADDR,&seconds,1)==ESP_OK &&
        !(seconds&0x80) && pcf85063a_get_time_date(&rtc,&date)==ESP_OK;
    i2c_master_bus_rm_device(rtc.dev_handle);
    if(!ok || date.year<2020 || date.year>2069 || date.month<1 || date.month>12 ||
       date.day<1 || date.day>31 || date.hour>23 || date.min>59 || date.sec>59)return;
    struct tm tm={.tm_sec=date.sec,.tm_min=date.min,.tm_hour=date.hour,
        .tm_mday=date.day,.tm_mon=date.month-1,.tm_year=date.year-1900,.tm_isdst=-1};
    const time_t epoch=mktime(&tm);
    if(epoch==(time_t)-1 || tm.tm_mday!=date.day || tm.tm_mon!=date.month-1)return;
    const struct timeval value={.tv_sec=epoch};
    settimeofday(&value,NULL);
    ESP_LOGI(TAG,"Restored battery-backed RTC (CST-8)");
}
static void save_rtc(void)
{
    const time_t now=time(NULL);struct tm tm;
    if(!localtime_r(&now,&tm) || tm.tm_year<120 || tm.tm_year>169)return;
    pcf85063a_dev_t rtc={0};
    if(rtc_open(&rtc)!=ESP_OK)return;
    const pcf85063a_datetime_t date={.year=tm.tm_year+1900,.month=tm.tm_mon+1,
        .day=tm.tm_mday,.dotw=tm.tm_wday,.hour=tm.tm_hour,.min=tm.tm_min,.sec=tm.tm_sec};
    const esp_err_t result=pcf85063a_set_time_date(&rtc,date);
    i2c_master_bus_rm_device(rtc.dev_handle);
    if(result!=ESP_OK)ESP_LOGW(TAG,"RTC save failed: %s",esp_err_to_name(result));
}
static void received(struct timeval *value)
{
    (void)value;
    /* lwIP owns this callback: defer I2C and logging to the resident task. */
    atomic_store(&s_received,true);
}
void time_sync_poll(bool wifi_connected)
{
    if(!s_clock_ready) {
        time_sync_init();
        if(!s_timezone_ready)return;
        restore_rtc();s_clock_ready=true;
    }
    if(atomic_exchange(&s_received,false)) {
        s_synced=true;save_rtc();
        const time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);
        ESP_LOGI(TAG,"SNTP synchronized: %04d-%02d-%02d %02d:%02d:%02d (UTC+8)",
            tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec);
    }
    esp_netif_t *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip={0};
    const bool online=wifi_connected && netif && esp_netif_is_netif_up(netif) &&
        esp_netif_get_ip_info(netif,&ip)==ESP_OK && ip.ip.addr;
    if(!online) {s_online=false;return;}
    if(!s_initialized) {
        esp_sntp_config_t config=ESP_NETIF_SNTP_DEFAULT_CONFIG(SERVERS[0]);
        config.start=false;config.wait_for_sync=false;config.sync_cb=received;
        const esp_err_t result=esp_netif_sntp_init(&config);
        if(result!=ESP_OK) {ESP_LOGW(TAG,"SNTP init failed: %s",esp_err_to_name(result));return;}
        s_initialized=true;
    }
    const int64_t now=esp_timer_get_time();
    if(!s_online || s_ip!=ip.ip.addr) {
        s_online=true;s_ip=ip.ip.addr;s_synced=false;s_attempt=0;s_retry_at=0;
    }
    if(!s_synced && now>=s_retry_at) {
        const char *server=SERVERS[s_attempt%3];
        esp_sntp_setservername(0,server);
        const esp_err_t result=esp_netif_sntp_start();
        ESP_LOGI(TAG,"Network time request: %s (%s)",server,esp_err_to_name(result));
        ++s_attempt;
        s_retry_at=now+(s_attempt<3?20LL:60LL)*1000000;
    }
}
