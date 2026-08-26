#include "esp_log_tags.h"
#include "esp_log.h"

/* ---------------------------------------------------------
 * Central tag definitions
 * --------------------------------------------------------- */
const char TAG_APP[]           = "APP";
const char TAG_HMI[]           = "HMI";
const char TAG_MODBUS_MASTER[] = "MODBUS_MASTER";
const char TAG_MODBUS_SLAVE[]  = "MODBUS_SLAVE";
const char TAG_MQTT[]          = "MQTT";
const char TAG_OTA[]           = "OTA";
const char TAG_SD[]            = "SDCARD";
const char TAG_LITTLEFS[]      = "LITTLEFS";
const char TAG_PWM[]           = "PWM";
const char TAG_AI[]            = "AI";
const char TAG_SENSOR[]        = "SENSOR";
const char TAG_CONTROL[]       = "CONTROL";
const char TAG_NEXTION[]       = "NEXTION";
const char TAG_TIME[]          = "TIME_SYNC";
const char TAG_EVENT_LOG[]     = "EVENT_LOG";
const char TAG_FILE_IO[]       = "FILE_IO";
const char TAG_THERMO_AI_SIM[] = "THERMO_AI_SIM";
const char TAG_SDCARD[] = "SDCARD";
const char TEMP_SIM[] = "TEMP SIM";
const char TAG_TEMP_PID_MBM[] = "TEMP_PID_MBM";
const char TAG_CUT_DECISION[] = "CUT DECISION";
const char TAG_TEMP_PID[] = "TEMP_PID";
const char TAG_WIFI[] = "WIFI_HTTP";


void init_log_levels(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    // esp_log_level_set("MQTT_AWS", ESP_LOG_INFO);
    // esp_log_level_set("RAM", ESP_LOG_INFO);



    // esp_log_level_set("APP", ESP_LOG_INFO);

    // esp_log_level_set(TAG_SDCARD, ESP_LOG_INFO);

    esp_log_level_set("ETH_W6100", ESP_LOG_INFO);

    // esp_log_level_set("mbedtls", ESP_LOG_INFO);

    // esp_log_level_set("MQTT_AWS", ESP_LOG_INFO);
    // esp_log_level_set("mqtt_client", ESP_LOG_INFO);
    // esp_log_level_set("esp-tls", ESP_LOG_INFO);
    // esp_log_level_set("transport_base", ESP_LOG_INFO);

    // esp_log_level_set(TAG_TIME, ESP_LOG_INFO);

    // esp_log_level_set("FILE_IO", ESP_LOG_INFO);
    // esp_log_level_set("WIFI_CFG", ESP_LOG_INFO);
    // esp_log_level_set("WIFI_DB", ESP_LOG_INFO);
    // esp_log_level_set("WIFI_MGR", ESP_LOG_INFO);

    // esp_log_level_set("OTA_MGR", ESP_LOG_INFO);
    // esp_log_level_set("OTA_HTTP", ESP_LOG_INFO);
    // esp_log_level_set("MQTT_AWS", ESP_LOG_INFO);
    // esp_log_level_set("MQTT_AWS", ESP_LOG_INFO);

    // esp_log_level_set("NX_UPDATE", ESP_LOG_INFO);
    // esp_log_level_set("HMI_RUNTIME", ESP_LOG_INFO);

}