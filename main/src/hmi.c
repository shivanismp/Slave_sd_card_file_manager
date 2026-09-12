#include "global.h"
#include "hmi.h"
#include "esp_log_tags.h"
#include "ethernet.h"
#include "wifi_mgr.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#define MODBUS_NETWORK_STATE_DISCONNECTED   0U
#define MODBUS_NETWORK_STATE_FULL           4U
#define MODBUS_NETWORK_LAN_STATE_CONNECTED  5U


static SemaphoreHandle_t g_hmi_data_mutex = NULL;

volatile hmi_data_t hmi_data;

void hmi_data_mutex_init(void)
{
    if (g_hmi_data_mutex == NULL)
        g_hmi_data_mutex = xSemaphoreCreateMutex();
}

bool hmi_data_lock(TickType_t timeout_ms)
{
    
    if (g_hmi_data_mutex == NULL)
        return false;

    return (xSemaphoreTake(g_hmi_data_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

void hmi_data_unlock(void)
{
    if (g_hmi_data_mutex != NULL)
        xSemaphoreGive(g_hmi_data_mutex);
}

bool hmi_data_get_snapshot(hmi_data_t *out, TickType_t timeout)
{
    if (out == NULL)
        return false;

    if (!hmi_data_lock(timeout))
        return false;

    memcpy(out, (const void *)&hmi_data, sizeof(hmi_data_t));
    hmi_data_unlock();
    return true;
}


bool file_write_melter_set_temp(volatile uint16_t value)
{
    FILE *melter_set_temp_file = fopen("/littlefs/melter_set_temp.txt", "w");

    if (melter_set_temp_file == NULL)
    {
        ESP_LOGE(TAG_LITTLEFS, "Error writing file /littlefs/melter_set_temp.txt");
        return false;
    }
    else
    {
        fprintf(melter_set_temp_file, "%hd", value);

        ESP_LOGI(TAG_LITTLEFS, "Created melter_set_temp.txt with set value %d",
                value);

        fclose(melter_set_temp_file);
    }

    return true;
}

bool file_read_melter_set_temp(void)
{
    FILE *melter_set_temp_file = fopen("/littlefs/melter_set_temp.txt", "r");

    if (melter_set_temp_file == NULL)
    {
        melter_set_temp_file = fopen("/littlefs/melter_set_temp.txt", "w");

        if (melter_set_temp_file == NULL)
        {
            ESP_LOGE(TAG_LITTLEFS, "Error writing file /littlefs/melter_set_temp.txt");
            return false;
        }
        else
        {
            hmi_data.melter_set_temp = 100;

            fprintf(melter_set_temp_file, "%hd", hmi_data.melter_set_temp);

            ESP_LOGI(TAG_LITTLEFS, "Created melter_set_temp.txt with default value %d",
                     hmi_data.melter_set_temp);

            fclose(melter_set_temp_file);
        }
    }
    else
    {
        fscanf(melter_set_temp_file, "%hd", &hmi_data.melter_set_temp);

        ESP_LOGI(TAG_LITTLEFS, "Read melter_set_temp = %d", hmi_data.melter_set_temp);

        fclose(melter_set_temp_file);
    }

    return true;
}

// static uint8_t hmi_get_network_state(void)
// {
//     if (ethernet_has_ip())
//         return MODBUS_NETWORK_STATE_FULL;

//     return MODBUS_NETWORK_STATE_DISCONNECTED;
// }

// static uint8_t hmi_get_network_state(void)
// {
//     if (ethernet_has_ip() ||
//         wifi_mgr_is_connected())
//     {
//         return MODBUS_NETWORK_STATE_FULL;
//     }

//     return MODBUS_NETWORK_STATE_DISCONNECTED;
// }


static uint8_t hmi_get_network_state(char *ssid, size_t ssid_size,
                                     char *ip, size_t ip_size)
{
    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t ip_info = {0};
    uint8_t state = MODBUS_NETWORK_STATE_DISCONNECTED;

    ssid[0] = '\0';
    ip[0] = '\0';

    /* Match network_manager_task(): LAN with an IP takes priority. */
    if (ethernet_is_link_up() && ethernet_has_ip())
    {
        state = MODBUS_NETWORK_LAN_STATE_CONNECTED;
        snprintf(ssid, ssid_size, "LAN");
        /* Default key used by ESP_NETIF_DEFAULT_ETH(). */
        netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    }
    else if (wifi_mgr_is_connected())
    {
        wifi_ap_record_t ap_info = {0};

        state = MODBUS_NETWORK_STATE_FULL;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            /* Wi-Fi SSIDs contain at most 32 bytes; bound the read. */
            snprintf(ssid, ssid_size, "%.*s", 32,
                     (const char *)ap_info.ssid);
        }
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }

    /* Never use the SoftAP IP or the other interface's cached address. */
    if (netif != NULL && esp_netif_is_netif_up(netif) &&
        esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0U)
    {
        snprintf(ip, ip_size, IPSTR, IP2STR(&ip_info.ip));
    }

    return state;
}

static void update_network_icon_modbus(void)
{
    char ssid[sizeof(hmi_data.sta_ssid)] = {0};
    char ip[sizeof(hmi_data.sta_ip)] = {0};
    uint8_t state = hmi_get_network_state(ssid, sizeof(ssid),
                                          ip, sizeof(ip));

    /*
     * Refresh even if the icon did not change: DHCP or the connected SSID
     * can change on the same interface. Publish the three fields together.
     * A failed lock is retried on the next task cycle.
     */
    if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
    {
        hmi_data.wifi_rssi_state = state;
        memcpy((void *)hmi_data.sta_ssid, ssid, sizeof(ssid));
        memcpy((void *)hmi_data.sta_ip, ip, sizeof(ip));
        hmi_data_unlock();
    }
}

static void update_network_icon_task_modbus(void *arg)
{
    (void)arg;

    while (true)
    {
        update_network_icon_modbus();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_hmi(void)
{
    file_read_melter_set_temp();

    xTaskCreate(update_network_icon_task_modbus,
                "update_network_icon_modbus",
                update_network_icon_task_modbus_stack_size_bytes,
                NULL,
                update_network_icon_task_modbus_priority,
                NULL);
}
