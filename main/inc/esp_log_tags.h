#ifndef __ESP_LOG_TAGS_H__
#define __ESP_LOG_TAGS_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------
 * Central tag declarations
 * Define once in esp_log_tags.c, use everywhere via extern
 * --------------------------------------------------------- */
extern const char TAG_APP[];
extern const char TAG_HMI[];
extern const char TAG_MODBUS_MASTER[];
extern const char TAG_MODBUS_SLAVE[];
extern const char TAG_MQTT[];
extern const char TAG_OTA[];
extern const char TAG_SD[];
extern const char TAG_LITTLEFS[];
extern const char TAG_PWM[];
extern const char TAG_AI[];
extern const char TAG_SENSOR[];
extern const char TAG_CONTROL[];
extern const char TAG_NEXTION[];
extern const char TAG_TIME[];
extern const char TAG_EVENT_LOG[];
extern const char TAG_FILE_IO[];
extern const char TAG_THERMO_AI_SIM[];
extern const char TAG_SDCARD[];
extern const char TEMP_SIM[];
extern const char TAG_TEMP_PID_MBM[];
extern const char TAG_CUT_DECISION[];
extern const char TAG_TEMP_PID[];
extern const char TAG_WIFI[];


#ifdef __cplusplus
}
#endif

void init_log_levels(void);


#endif