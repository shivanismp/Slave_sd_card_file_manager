#ifndef __DATE_TIME_H__
#define __DATE_TIME_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "lwip/apps/sntp.h"

/*
 * POSIX TZ strings use the opposite sign convention from ISO-8601 offsets.
 * "IST-5:30" therefore means UTC+05:30 with no daylight-saving change.
 */
#ifndef APP_TIME_TZ
#define APP_TIME_TZ "IST-5:30"
#endif

#define APP_TIME_HMI_CALENDAR_WORDS 7U

typedef enum
{
    APP_TIME_SOURCE_NONE = 0,
    APP_TIME_SOURCE_HMI_RTC = 1,
    APP_TIME_SOURCE_SNTP = 2
} app_time_source_t;

/* Starts SNTP asynchronously; normal logging does not wait for the Internet. */
void app_time_init_sntp(void);

/* True after either a valid HMI RTC fallback or SNTP has set the wall clock. */
bool app_time_is_valid(void);

app_time_source_t app_time_get_source(void);

/*
 * Delta calendar word order:
 * year, month, date, weekday, hour, minute, second.
 * Weekday is ignored when importing and regenerated when exporting.
 */
bool app_time_hmi_calendar_to_unix(
    const uint16_t calendar[APP_TIME_HMI_CALENDAR_WORDS],
    time_t *unix_time);
bool app_time_set_from_hmi_calendar(
    const uint16_t calendar[APP_TIME_HMI_CALENDAR_WORDS]);
bool app_time_get_local_calendar(
    uint16_t calendar[APP_TIME_HMI_CALENDAR_WORDS]);

/* SNTP callback -> Delta Modbus task handoff. */
bool app_time_hmi_update_pending(void);
void app_time_mark_hmi_updated(void);

#endif
