#include <esp_app_desc.h>
#include <strings.h>

#include "global.h"
#include "file_io.h"
#include "littlefs.h"
#include "file_manager.h"
#include "hmi.h"
#include "modbus_slave.h"
#include "modbus_master.h"
#include "mqtt.h"
#include "mqtt_file_uploader.h"
#include "machine_presence.h"
#include "wifi.h"

#include "wifi_mgr.h"
#include "web_cfg.h"
#include "esp_system.h"

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

#include "esp_heap_caps.h"

char hmi_head_text[50];
char machine_model[50];
char machine_capacity[50];

static volatile bool mqtt_read_certs_ok = false;
static volatile bool mqtt_serial_read_ok = false;
static volatile bool mqtt_broker_host_read_ok = false;
static volatile bool mqtt_topics_read_ok = false;

#define EXT_WD_PIN GPIO_NUM_0
#define EXT_WD_FEED_INTERVAL_MS 500
#define WIFI_SETUP_PASSWORD "shapet@1995"

typedef enum
{
    ACTIVE_NETWORK_NONE = 0,
    ACTIVE_NETWORK_ETHERNET,
    ACTIVE_NETWORK_WIFI
} active_network_t;

static void call_functions_on_network_connect(bool network_changed)
{
    static bool sntp_started = false;

    /*
     * SNTP does not depend on MQTT configuration.
     * Start it once whenever any network first becomes available.
     */
    if (!sntp_started)
    {
        ESP_LOGI("APP", "Starting SNTP");

        app_time_init_sntp();
        sntp_started = true;
    }

    /*
     * AWS TLS certificate validation requires valid system time.
     */
    if (!app_time_is_valid())
    {
        ESP_LOGW("APP", "Waiting for valid system time");
        return;
    }

    if (!(mqtt_read_certs_ok &&
          mqtt_serial_read_ok &&
          mqtt_broker_host_read_ok &&
          mqtt_topics_read_ok))
    {
        ESP_LOGE("APP", "MQTT configuration is incomplete");
        return;
    }

    if (!mqtt_is_started())
    {
        ESP_LOGI("APP", "Starting MQTT");

        mqtt_start_tls();
    }
    else if (network_changed || !mqtt_is_connected())
    {
        /*
         * A TCP/MQTT connection cannot automatically migrate between
         * Ethernet and Wi-Fi, so reconnect it after a route change.
         */
        ESP_LOGI("APP", "Reconnecting MQTT after network change");

        mqtt_force_reconnect();
    }
}

static const char *get_wifi_ap_prefix(void)
{
    if (strcasecmp(hmi_head_text,
                   "Prime-Melt Series") == 0)
    {
        return "Prime-Melt";
    }

    if (strcasecmp(hmi_head_text,
                   "Industrial") == 0)
    {
        return "PowerHeat";
    }

    ESP_LOGW("NET_MANAGER",
             "Unknown head text '%s'; using PowerHeat",
             hmi_head_text);

    return "PowerHeat";
}

static void network_manager_task(void *p)
{
    (void)p;

    active_network_t active_network = ACTIVE_NETWORK_NONE;

    char ap[20];
    char random_suffix[7] = {0};

    /*
     * Initialise the Wi-Fi manager.
     *
     * wifi_mgr_init() must tolerate ESP_ERR_INVALID_STATE when the
     * default ESP event loop already exists.
     */
    wifi_mgr_init();

    /*
     * Generate a six-digit random suffix.
     */
    for (size_t i = 0; i < 6; i++)
    {
        random_suffix[i] =
            (char)('0' + (esp_random() % 10U));
    }

    /*
     * Final SoftAP name example:
     *
     */
    const char *wifi_ap_prefix =
        get_wifi_ap_prefix();

    snprintf(ap,
             sizeof(ap),
             "%s-%s",
             wifi_ap_prefix,
             random_suffix);

    ESP_LOGI("NET_MANAGER",
             "Head text: %s | Generated Wi-Fi SSID: %s",
             hmi_head_text,
             ap);
    /*
     * Copy the actual ESP32 SoftAP credentials into hmi_data so
     * that they can be transferred to the Delta HMI.
     */
    if (hmi_data_lock(100))
    {
        snprintf((char *)hmi_data.wifi_ap_ssid,
                 sizeof(hmi_data.wifi_ap_ssid),
                 "%s",
                 ap);

        snprintf((char *)hmi_data.wifi_ap_pass,
                 sizeof(hmi_data.wifi_ap_pass),
                 "%s",
                 WIFI_SETUP_PASSWORD);

        hmi_data_unlock();

        ESP_LOGI("NET_MANAGER",
                 "Delta HMI Wi-Fi SSID: %s",
                 ap);

        ESP_LOGI("NET_MANAGER",
                 "Delta HMI Wi-Fi password: %s",
                 WIFI_SETUP_PASSWORD);
    }
    else
    {
        ESP_LOGW("NET_MANAGER",
                 "Could not update Wi-Fi credentials in HMI data");
    }

    /*
     * Start the ESP32 configuration SoftAP immediately.
     *
     * The Wi-Fi manager should configure AP+STA mode so the setup
     * access point and the router connection can operate together.
     */
    wifi_mgr_start_softap(
        ap,
        WIFI_SETUP_PASSWORD);

    /*
     * Start the Wi-Fi configuration webpage.
     */
    web_cfg_start();

    ESP_LOGI("NET_MANAGER",
             "Network manager started");

    ESP_LOGI("NET_MANAGER",
             "Ethernet has first priority");

    while (true)
    {
        const bool ethernet_link =
            ethernet_is_link_up();

        const bool ethernet_ip =
            ethernet_link && ethernet_has_ip();

        const bool wifi_ip =
            wifi_mgr_is_connected();

        active_network_t new_network;

        /*
         * Select the preferred internet interface.
         *
         * Ethernet always has priority whenever it has an IP.
         */
        if (ethernet_ip)
        {
            new_network = ACTIVE_NETWORK_ETHERNET;
        }
        else if (wifi_ip)
        {
            new_network = ACTIVE_NETWORK_WIFI;
        }
        else
        {
            new_network = ACTIVE_NETWORK_NONE;
        }

        /*
         * Detect a change between Ethernet, Wi-Fi and disconnected.
         */
        if (new_network != active_network)
        {
            active_network = new_network;

            switch (active_network)
            {
            case ACTIVE_NETWORK_ETHERNET:
            {
                ESP_LOGI("NET_MANAGER",
                         "Active internet: Ethernet");

                /*
                 * Force MQTT to recreate its socket so that it
                 * uses the preferred Ethernet route.
                 */
                call_functions_on_network_connect(true);
                break;
            }

            case ACTIVE_NETWORK_WIFI:
            {
                ESP_LOGI("NET_MANAGER",
                         "Active internet: Wi-Fi");

                /*
                 * Force MQTT to recreate its socket using Wi-Fi.
                 */
                call_functions_on_network_connect(true);
                break;
            }

            case ACTIVE_NETWORK_NONE:
            default:
            {
                ESP_LOGW("NET_MANAGER",
                         "No internet connection available");
                break;
            }
            }
        }

        /*
         * Ethernet is fully connected.
         *
         * Do not attempt a new router Wi-Fi connection. The
         * PowerHeat SoftAP remains available for configuration.
         */
        if (ethernet_ip)
        {
            call_functions_on_network_connect(false);

            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /*
         * Ethernet is unavailable, but station Wi-Fi is already
         * connected to a router.
         */
        if (wifi_ip)
        {
            call_functions_on_network_connect(false);

            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /*
         * Ethernet is unavailable and a Wi-Fi station connection
         * attempt is already running.
         */
        if (wifi_mgr_is_connecting())
        {
            ESP_LOGI("NET_MANAGER",
                     "Wi-Fi connection already in progress");

            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /*
         * Ethernet has lost its link/IP and router Wi-Fi is not
         * connected. Immediately try the best saved network.
         */
        ESP_LOGW("NET_MANAGER",
                 "Ethernet unavailable; connecting saved Wi-Fi");

        bool connected =
            wifi_mgr_try_connect_best_known(12000);

        if (connected)
        {
            ESP_LOGI("NET_MANAGER",
                     "Wi-Fi fallback connected");

            /*
             * On the next iteration, wifi_mgr_is_connected() will
             * select Wi-Fi and reconnect MQTT.
             */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /*
         * The PowerHeat SoftAP and configuration webpage are already
         * running, so the user can configure another router network.
         */
        ESP_LOGW("NET_MANAGER",
                 "No saved Wi-Fi network could be connected");

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

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
             mqtt_broker_host_read_ok,
             mqtt_topics_read_ok);

    if (!(mqtt_read_certs_ok &&
          mqtt_serial_read_ok &&
          mqtt_broker_host_read_ok &&
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
                 mqtt_broker_host,
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
        if (!app_time_is_valid())
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

static void print_ram_usage(void)
{
    multi_heap_info_t info;

    /*
     * Internal byte-addressable RAM used by malloc(), MQTT,
     * FreeRTOS tasks, TLS buffers, file uploader, etc.
     */
    heap_caps_get_info(
        &info,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    size_t total_ram =
        info.total_allocated_bytes +
        info.total_free_bytes;

    float used_percent = 0.0f;

    if (total_ram > 0)
    {
        used_percent =
            ((float)info.total_allocated_bytes * 100.0f) /
            (float)total_ram;
    }

    ESP_LOGI("RAM",
             "Total=%u bytes | Used=%u bytes (%.1f%%) | "
             "Free=%u bytes | Minimum Free=%u bytes | "
             "Largest Block=%u bytes",
             (unsigned int)total_ram,
             (unsigned int)info.total_allocated_bytes,
             used_percent,
             (unsigned int)info.total_free_bytes,
             (unsigned int)info.minimum_free_bytes,
             (unsigned int)info.largest_free_block);
}

void app_main(void)
{
    init_log_levels();

    html_file_manager_enabled = true;

    BaseType_t result = xTaskCreate(
        wdt_task,
        "external_wdt",
        wdt_task_stack_size_bytes,
        NULL,
        wdt_task_priority,
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

    if (!load_or_restore_mqtt_broker_host(mqtt_broker_host, sizeof(mqtt_broker_host)))
    {
        ESP_LOGE("APP", "4. MQTT aws endpoint missing or corrupted in LittleFS");
        mqtt_broker_host_read_ok = false;
    }
    else
    {
        ESP_LOGI("APP", "4. MQTT endpoint ok: %s", mqtt_broker_host);
        mqtt_broker_host_read_ok = true;
    }

    if (!mqtt_load_certs_from_littlefs(shapet_root_ca, sizeof(shapet_root_ca),
                                       device_cert, sizeof(device_cert),
                                       device_private_key, sizeof(device_private_key)))
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

    // ESP_LOGI("APP", "7. OTA first-boot validation");
    // ota_mark_running_app_valid_if_needed();
    // mqtt_register_update_handlers(
    //     ota_is_in_progress,
    //     ota_request_from_json,
    //     nextion_update_is_in_progress,
    //     nextion_update_request_from_json);

    // Notify the machine presence (ONLINE / OFFLINE) service to the backend if the serial number is valid.
    if (mqtt_serial_read_ok)
    {
        esp_err_t presence_err = machine_presence_init("MACHINE_STATUS");
        if (presence_err != ESP_OK)
        {
            ESP_LOGE("APP", "Machine presence setup failed: %s",
                     esp_err_to_name(presence_err));
        }
    }

    /*
     * Configure the retained ACTIVE message and the MQTT INACTIVE Last Will.
     * This must happen before start_network_services() can call mqtt_start_tls().
     */
    // if (mqtt_serial_read_ok)
    // {
    //     if (!machine_status_init(mqtt_serial_no))
    //     {
    //         ESP_LOGE("APP", "Machine status initialization failed");
    //     }
    //     else
    //     {
    //         ESP_LOGI("APP", "Machine status initialized: %s",
    //                  machine_status_get_topic());
    //     }
    // }

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
                    event_log_task_stack_size_bytes,
                    NULL,
                    event_log_task_priority,
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

    esp_err_t eth_err = ethernet_init();

    if (eth_err != ESP_OK)
    {
        ESP_LOGE("ETH_W6100",
                 "Ethernet init failed: %s",
                 esp_err_to_name(eth_err));

        /*
         * Continue running. The network manager will attempt Wi-Fi.
         */
    }
    else
    {
        ESP_LOGI("ETH_W6100",
                 "Ethernet driver initialized");
    }

    /*
     * Created this task—even when Ethernet initialization fails
     * or Ethernet does not receive an IP.
     */
    BaseType_t network_task_result = xTaskCreate(
        network_manager_task,
        "network_manager",
        network_manager_task_stack_size_bytes,
        NULL,
        network_manager_task_priority,
        NULL);

    if (network_task_result != pdPASS)
    {
        ESP_LOGE("APP",
                 "Failed to create network manager task");
    }

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

    while (1)
    {
        print_ram_usage();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
