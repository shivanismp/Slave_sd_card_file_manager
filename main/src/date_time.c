#include "date_time.h"
#include "esp_log_tags.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"

#include <stdlib.h>

static bool sntp_started = false;
static app_time_source_t s_time_source = APP_TIME_SOURCE_NONE;
static bool s_hmi_update_pending = false;
static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;

static void app_time_configure_timezone(void)
{
    setenv("TZ", APP_TIME_TZ, 1);
    tzset();
}

static void app_time_set_source(app_time_source_t source,
                                bool request_hmi_update)
{
    portENTER_CRITICAL(&s_time_lock);
    s_time_source = source;
    if (request_hmi_update)
        s_hmi_update_pending = true;
    portEXIT_CRITICAL(&s_time_lock);
}

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;

    /* Never perform Modbus communication in the lwIP callback. */
    app_time_set_source(APP_TIME_SOURCE_SNTP, true);
    ESP_LOGI(TAG_TIME,
             "Time synchronized from SNTP; Delta RTC update queued");
}

void app_time_init_sntp(void)
{
    bool already_started;

    app_time_configure_timezone();

    portENTER_CRITICAL(&s_time_lock);
    already_started = sntp_started;
    if (!already_started)
        sntp_started = true;
    portEXIT_CRITICAL(&s_time_lock);

    if (already_started)
        return;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = time_sync_notification_cb;
    config.smooth_sync = true;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK)
    {
        portENTER_CRITICAL(&s_time_lock);
        sntp_started = false;
        portEXIT_CRITICAL(&s_time_lock);

        ESP_LOGE(TAG_TIME,
                 "Could not start SNTP: %s",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG_TIME, "SNTP started asynchronously");
}

bool app_time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);

    return (timeinfo.tm_year + 1900) >= 2024;
}

app_time_source_t app_time_get_source(void)
{
    app_time_source_t source;

    portENTER_CRITICAL(&s_time_lock);
    source = s_time_source;
    portEXIT_CRITICAL(&s_time_lock);

    return source;
}

bool app_time_hmi_calendar_to_unix(
    const uint16_t calendar[APP_TIME_HMI_CALENDAR_WORDS],
    time_t *unix_time)
{
    if (calendar == NULL || unix_time == NULL)
        return false;

    const uint16_t year = calendar[0];
    const uint16_t month = calendar[1];
    const uint16_t day = calendar[2];
    const uint16_t hour = calendar[4];
    const uint16_t minute = calendar[5];
    const uint16_t second = calendar[6];

    if (year < 2024U || year > 2099U ||
        month < 1U || month > 12U ||
        day < 1U || day > 31U ||
        hour > 23U || minute > 59U || second > 59U)
    {
        return false;
    }

    app_time_configure_timezone();

    struct tm requested = {0};
    requested.tm_year = (int)year - 1900;
    requested.tm_mon = (int)month - 1;
    requested.tm_mday = (int)day;
    requested.tm_hour = (int)hour;
    requested.tm_min = (int)minute;
    requested.tm_sec = (int)second;
    requested.tm_isdst = -1;

    time_t candidate = mktime(&requested);
    if (candidate == (time_t)-1)
        return false;

    /* Reject normalized impossible dates, for example 31 February. */
    struct tm verified = {0};
    localtime_r(&candidate, &verified);

    if ((verified.tm_year + 1900) != (int)year ||
        (verified.tm_mon + 1) != (int)month ||
        verified.tm_mday != (int)day ||
        verified.tm_hour != (int)hour ||
        verified.tm_min != (int)minute ||
        verified.tm_sec != (int)second)
    {
        return false;
    }

    *unix_time = candidate;
    return true;
}

bool app_time_set_from_hmi_calendar(
    const uint16_t calendar[APP_TIME_HMI_CALENDAR_WORDS])
{
    /* Once SNTP succeeds, HMI time may never overwrite it. */
    if (app_time_get_source() == APP_TIME_SOURCE_SNTP)
        return false;

    time_t hmi_time = 0;
    if (!app_time_hmi_calendar_to_unix(calendar, &hmi_time))
        return false;

    struct timeval tv = {
        .tv_sec = hmi_time,
        .tv_usec = 0
    };

    if (settimeofday(&tv, NULL) != 0)
        return false;

    app_time_set_source(APP_TIME_SOURCE_HMI_RTC, false);
    ESP_LOGI(TAG_TIME,
             "System clock initialized from Delta HMI RTC: %lld",
             (long long)hmi_time);

    return true;
}

bool app_time_get_local_calendar(
    uint16_t calendar[APP_TIME_HMI_CALENDAR_WORDS])
{
    if (calendar == NULL || !app_time_is_valid())
        return false;

    app_time_configure_timezone();

    time_t now = 0;
    struct tm local_time = {0};
    time(&now);
    localtime_r(&now, &local_time);

    calendar[0] = (uint16_t)(local_time.tm_year + 1900);
    calendar[1] = (uint16_t)(local_time.tm_mon + 1);
    calendar[2] = (uint16_t)local_time.tm_mday;
    calendar[3] = (uint16_t)local_time.tm_wday;
    calendar[4] = (uint16_t)local_time.tm_hour;
    calendar[5] = (uint16_t)local_time.tm_min;
    calendar[6] = (uint16_t)local_time.tm_sec;

    return true;
}

bool app_time_hmi_update_pending(void)
{
    bool pending;

    portENTER_CRITICAL(&s_time_lock);
    pending = s_hmi_update_pending;
    portEXIT_CRITICAL(&s_time_lock);

    return pending;
}

void app_time_mark_hmi_updated(void)
{
    portENTER_CRITICAL(&s_time_lock);
    s_hmi_update_pending = false;
    portEXIT_CRITICAL(&s_time_lock);
}
