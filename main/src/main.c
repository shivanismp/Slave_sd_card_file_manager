#include <esp_app_desc.h>
#include "global.h"
#include "file_io.h"
#include "littlefs.h"
#include "file_manager.h"
#include "hmi.h"
#include "modbus_slave.h"
#include "modbus_master.h"
#include "mqtt.h"
#include "mqtt_file_uploader.h"

#include "ota_mgr.h"
#include "nextion_update.h"

#include "date_time.h"

#include "event_log.h"
#include "esp_log_tags.h"

#include "sdcard.h"
#include "esp_err.h"
#include "ethernet.h"

#include "esp_event.h"
#include "esp_netif.h"
#include <time.h>

#include <driver/gpio.h>

char hmi_head_text[50];
char machine_model[50];
char machine_capacity[50];

static volatile bool mqtt_read_certs_ok = false;
static volatile bool mqtt_serial_read_ok = false;
static volatile bool mqtt_aws_endpoint_read_ok = false;
static volatile bool mqtt_topics_read_ok = false;

#define EXT_WD_PIN GPIO_NUM_0
#define EXT_WD_FEED_INTERVAL_MS 500

static void init_external_watchdog(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << EXT_WD_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* Establish a known initial state */
    ESP_ERROR_CHECK(gpio_set_level(EXT_WD_PIN, 0));
}

static bool system_time_is_valid(void)
{
    time_t now = 0;
    struct tm time_info = {0};

    time(&now);
    localtime_r(&now, &time_info);

    /*
     * tm_year counts from 1900.
     * Accept any realistic year from 2024 onward.
     */
    return (time_info.tm_year + 1900) >= 2024;
}

static void load_or_restore_machine_info(void)
{
    FILE *fp;
    char line[128];

    // Default values
    strcpy(hmi_head_text, "PRIME-MELT SERIES");
    strcpy(machine_model, "PrimeMelt-4");
    strcpy(machine_capacity, "4KG - 4 POT");

    fp = fopen(MACHINE_INFO_FILE_PATH, "r");
    if (fp == NULL)
    {
        ESP_LOGE("APP", "Failed to open machine info file");
        return;
    }

    int line_num = 0;
    while (fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) == 0)
            continue;

        if (strncmp(line, "MACHINE_TYPE: ", 14) == 0)
        {
            strncpy(hmi_head_text, line + 14, sizeof(hmi_head_text) - 1);
            hmi_head_text[sizeof(hmi_head_text) - 1] = '\0';
        }
        else if (strncmp(line, "MACHINE_MODEL: ", 15) == 0)
        {
            strncpy(machine_model, line + 15, sizeof(machine_model) - 1);
            machine_model[sizeof(machine_model) - 1] = '\0';
        }
        else if (strncmp(line, "CAPACITY: ", 10) == 0)
        {
            strncpy(machine_capacity, line + 10, sizeof(machine_capacity) - 1);
            machine_capacity[sizeof(machine_capacity) - 1] = '\0';
        }
        else if (strncmp(line, "THING_NAME: ", 12) == 0)
        {
            // skip
        }
        else
        {
            if (line_num == 0)
            {
                strncpy(hmi_head_text, line, sizeof(hmi_head_text) - 1);
                hmi_head_text[sizeof(hmi_head_text) - 1] = '\0';
            }
            else if (line_num == 1)
            {
                strncpy(machine_model, line, sizeof(machine_model) - 1);
                machine_model[sizeof(machine_model) - 1] = '\0';
            }
            else if (line_num == 2)
            {
                strncpy(machine_capacity, line, sizeof(machine_capacity) - 1);
                machine_capacity[sizeof(machine_capacity) - 1] = '\0';
            }
            line_num++;
        }
    }
    fclose(fp);

    ESP_LOGI("APP", "Machine Info - Type: %s, Model: %s, Capacity: %s", hmi_head_text, machine_model, machine_capacity);
}

static void hmi_init_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    load_or_restore_machine_info();

    strncpy((char *)hmi_data.hmi_head_text, hmi_head_text, sizeof(hmi_data.hmi_head_text) - 1);
    hmi_data.hmi_head_text[sizeof(hmi_data.hmi_head_text) - 1] = '\0';

    strncpy((char *)hmi_data.machine_model, machine_model, sizeof(hmi_data.machine_model) - 1);
    hmi_data.machine_model[sizeof(hmi_data.machine_model) - 1] = '\0';

    strncpy((char *)hmi_data.machine_capacity, machine_capacity, sizeof(hmi_data.machine_capacity) - 1);
    hmi_data.machine_capacity[sizeof(hmi_data.machine_capacity) - 1] = '\0';

    strcpy((char *)hmi_data.iot_id, (const char *)mqtt_serial_no);
    strcpy((char *)hmi_data.version, (const char *)app->version);

}

static void sd_card_init(void)
{
    esp_err_t err = sdcard_init();

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG_SDCARD, "SD card not mounted: %s", esp_err_to_name(err));
    }
}

static bool start_network_services(void)
{
    ESP_LOGI("APP",
             "MQTT prerequisites: certs=%d serial=%d endpoint=%d topics=%d",
             mqtt_read_certs_ok,
             mqtt_serial_read_ok,
             mqtt_aws_endpoint_read_ok,
             mqtt_topics_read_ok);

    if (!(mqtt_read_certs_ok &&
          mqtt_serial_read_ok &&
          mqtt_aws_endpoint_read_ok &&
          mqtt_topics_read_ok))
    {
        ESP_LOGE("APP", "MQTT not started: configuration is incomplete");
        return false;
    }

    ESP_LOGI("APP",
             "MQTT state before start: started=%d connected=%d",
             mqtt_is_started(),
             mqtt_is_connected());

    if (!mqtt_is_started())
    {
        ESP_LOGI("APP",
                 "Starting MQTT client: endpoint=%s client_id=%s",
                 mqtt_aws_endpoint,
                 mqtt_serial_no);

        mqtt_start_tls();

        ESP_LOGI("APP",
                 "mqtt_start_tls returned, started=%d connected=%d",
                 mqtt_is_started(),
                 mqtt_is_connected());
    }
    else if (!mqtt_is_connected())
    {
        ESP_LOGW("APP", "MQTT started but disconnected; requesting reconnect");
        mqtt_force_reconnect();
    }
    else
    {
        ESP_LOGI("APP", "MQTT is already connected");
    }

    return true;
}

static void ethernet_services_task(void *arg)
{
    bool sntp_started = false;

    ESP_LOGI("APP", "Ethernet network-services task started");

    while (true)
    {
        if (!ethernet_is_link_up())
        {
            ESP_LOGW("APP", "Ethernet link is down");
            sntp_started = false;

            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!ethernet_has_ip())
        {
            ESP_LOGW("APP", "Ethernet link is up but no IP address");

            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /*
         * Start SNTP once after Ethernet receives an IP.
         */
        if (!sntp_started)
        {
            ESP_LOGI("APP", "Starting SNTP over Ethernet");
            app_time_init_sntp();
            sntp_started = true;
        }

        /*
         * Do not start AWS TLS until the clock is valid.
         */
        if (!system_time_is_valid())
        {
            ESP_LOGW("APP", "Waiting for valid system time before MQTT TLS");

            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /*
         * This function starts MQTT, reconnects it, or reports that
         * MQTT is already connected.
         */
        start_network_services();

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void wdt_task(void *arg)
{
    init_external_watchdog();

    TickType_t last_wake_time = xTaskGetTickCount();
    int watchdog_level = 0;

    while (true)
    {
        watchdog_level = !watchdog_level;

        ESP_ERROR_CHECK(
            gpio_set_level(EXT_WD_PIN, watchdog_level));

        vTaskDelayUntil(
            &last_wake_time,
            pdMS_TO_TICKS(EXT_WD_FEED_INTERVAL_MS));
    }
}


void app_main(void)
{
    init_log_levels();

    html_file_manager_enabled = false;

    BaseType_t result = xTaskCreate(
        wdt_task,
        "external_wdt",
        2048,
        NULL,
        5,
        NULL);

    if (result != pdPASS)
    {
        ESP_LOGE("APP", "Failed to create external watchdog task");
        abort();
    }
    ESP_LOGI("APP", "1. app_main start");

    ESP_ERROR_CHECK(littlefs_init());
    ESP_LOGI("APP", "2. littlefs_init done");

    read_latest_machine_record();
    sd_card_init();

    if (!load_or_restore_mqtt_serial_no(mqtt_serial_no, sizeof(mqtt_serial_no)))
    {
        ESP_LOGE("APP", "3. MQTT serial missing or corrupted in LittleFS");
        mqtt_serial_read_ok = false;
    }
    else
    {
        ESP_LOGI("APP", "3. MQTT serial ok: %s", mqtt_serial_no);
        mqtt_serial_read_ok = true;

        snprintf(mqtt_topic_ota_cmd, sizeof(mqtt_topic_ota_cmd),
                 "%s/OTA_CMD", mqtt_serial_no);
        snprintf(mqtt_topic_ota_status, sizeof(mqtt_topic_ota_status),
                 "%s/OTA_STATUS", mqtt_serial_no);
    }

    if (!load_or_restore_mqtt_aws_endpoint(mqtt_aws_endpoint, sizeof(mqtt_aws_endpoint)))
    {
        ESP_LOGE("APP", "4. MQTT aws endpoint missing or corrupted in LittleFS");
        mqtt_aws_endpoint_read_ok = false;
    }
    else
    {
        ESP_LOGI("APP", "4. MQTT endpoint ok: %s", mqtt_aws_endpoint);
        mqtt_aws_endpoint_read_ok = true;
    }

    if (!mqtt_load_certs_from_littlefs(aws_root_ca, sizeof(aws_root_ca),
                                       hmi_card_test_cert, sizeof(hmi_card_test_cert),
                                       hmi_card_test_private, sizeof(hmi_card_test_private)))
    {
        ESP_LOGE("APP", "5. MQTT certs missing or corrupted in LittleFS");
        mqtt_read_certs_ok = false;
    }
    else
    {
        ESP_LOGI("APP", "5. MQTT certs loaded successfully");
        mqtt_read_certs_ok = true;
    }

    if (!load_or_restore_mqtt_topics(&g_mqtt_topics,
                                     (char *)mqtt_topic_mbm_req, sizeof(mqtt_topic_mbm_req),
                                     (char *)mqtt_topic_mbm_res, sizeof(mqtt_topic_mbm_res)))
    {
        ESP_LOGE("APP", "6. MQTT topics missing or corrupted in LittleFS");
        mqtt_topics_read_ok = false;
    }
    else
    {
        ESP_LOGI("APP", "6. MQTT MBM REQ: %s", mqtt_topic_mbm_req);
        ESP_LOGI("APP", "6. MQTT MBM RES: %s", mqtt_topic_mbm_res);
        ESP_LOGI("APP", "6. MQTT topic count: %d", g_mqtt_topics.count);
        mqtt_topics_read_ok = true;
    }

    if (mqtt_serial_read_ok)
    {
        esp_err_t uploader_err = mqtt_file_uploader_start();
        if (uploader_err != ESP_OK)
        {
            ESP_LOGE("APP",
                     "MQTT file uploader could not start: %s",
                     esp_err_to_name(uploader_err));
        }
    }

    ESP_LOGI("APP", "7. OTA first-boot validation");
    ota_mark_running_app_valid_if_needed();
    mqtt_register_update_handlers(
        ota_is_in_progress,
        ota_request_from_json,
        nextion_update_is_in_progress,
        nextion_update_request_from_json);

    mqtt_modbus_slave.modbus_slave_process_frame = modbus_slave_process_frame;

    hmi_data_mutex_init();

    event_log_cfg_t log_cfg;
    event_log_default_cfg(&log_cfg);

    log_cfg.rotate_bytes = 8U * 1024U;
    log_cfg.total_limit_bytes = 256U * 1024U;
    log_cfg.flush_period_ms = 3000U;
    log_cfg.publish_timeout_ms = 5000U;

    /* optional thresholds to avoid noisy min/max logging */
    log_cfg.voltage_step = 2U;
    log_cfg.current_step = 1U;
    log_cfg.power_step = 1U;
    log_cfg.pf_step = 5U;
    log_cfg.freq_step = 5U;
    log_cfg.temp_step = 5U;

    if (event_log_init(&log_cfg) == ESP_OK)
    {
        event_log_use_default_mqtt_publish();
        event_log_set_default_topic_from_serial();

        xTaskCreate(event_log_task,
                    "event_log_task",
                    4096,
                    NULL,
                    4,
                    NULL);

        event_log_note_text("APP", "INFO", "Event logger started");
    }

    hmi_init_info();
    app_modbus_master();
    app_modbus_slave(); /* Historical name: now starts ESP -> Delta HMI master. */
    app_hmi();

    /* Process-wide network services: call exactly once. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    file_manager_config_t fm_cfg = FILE_MANAGER_DEFAULT_CONFIG();
    fm_cfg.mount_point = LITTLEFS_BASE_PATH;
    fm_cfg.partition_label = LITTLEFS_PART_LABEL;
    fm_cfg.mount_spiffs = false;
    fm_cfg.filesystem = FILE_MANAGER_FS_LITTLEFS;
    fm_cfg.port = 8080;
    fm_cfg.control_port = 32769;
    fm_cfg.allow_mutation = true;
    fm_cfg.sd_mount_point = SDCARD_BASE_PATH;
    fm_cfg.sd_available = sdcard_is_mounted;
    esp_err_t fm_err = file_manager_init(&fm_cfg);
    if (fm_err != ESP_OK)
    {
        ESP_LOGE("APP", "File manager start failed: %s", esp_err_to_name(fm_err));
    }

    esp_err_t eth_err = ethernet_init();
    if (eth_err != ESP_OK)
    {
        ESP_LOGE("ETH_W6100", "Ethernet init failed: %s", esp_err_to_name(eth_err));
    }
    else
    {
        /* ... after ethernet_init() succeeds ... */
        esp_err_t net_err = ethernet_wait_for_ip(30000);

        if (net_err == ESP_ERR_TIMEOUT)
        {
            ESP_LOGW("APP", "No DHCP lease after 30 seconds");
            ethernet_log_status();
            ethernet_log_dhcp_status();

            /* One controlled retry. */
            if (ethernet_restart_dhcp() == ESP_OK)
            {
                net_err = ethernet_wait_for_ip(30000);
            }
        }

        if (net_err == ESP_OK)
        {
            ethernet_log_status();
            BaseType_t task_created = xTaskCreate(
                ethernet_services_task,
                "eth_services",
                6144,
                NULL,
                3,
                NULL);

            if (task_created != pdPASS)
            {
                ESP_LOGE("APP", "Failed to create Ethernet services task");
            }
            else
            {
                ESP_LOGI("APP", "Ethernet services task created");
            }
        }
    }

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
