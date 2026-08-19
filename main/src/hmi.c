#include "global.h"
#include "hmi.h"
#include "esp_log_tags.h"
#include "ethernet.h"

#include "esp_log.h"

#define MODBUS_NETWORK_STATE_DISCONNECTED 0U
#define MODBUS_NETWORK_STATE_FULL         4U

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

static uint8_t hmi_get_network_state(void)
{
    if (ethernet_has_ip())
        return MODBUS_NETWORK_STATE_FULL;

    return MODBUS_NETWORK_STATE_DISCONNECTED;
}

static void update_network_icon_modbus(void)
{
    uint8_t state = hmi_get_network_state();
    static uint8_t previous_state = UINT8_MAX;

    if (state == previous_state)
        return;

    /*
     * Update the cached state only after the shared HMI data was written.
     * If the mutex is temporarily unavailable, the next task cycle retries.
     */
    if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
    {
        hmi_data.wifi_rssi_state = state;
        hmi_data_unlock();
        previous_state = state;
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
                4096,
                NULL,
                5,
                NULL);
}
