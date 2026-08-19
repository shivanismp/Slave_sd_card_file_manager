#include "hmi_runtime.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NEXTION_UART_NUM LP_UART_NUM_0
#define NEXTION_BAUDRATE 9600

static const char *TAG = "HMI_RUNTIME";

static SemaphoreHandle_t s_update_mutex = NULL;
static volatile bool s_update_in_progress = false;

static void hmi_nextion_send_raw_cmd(const char *cmd);

static bool hmi_nextion_wait_for_0x05(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t b = 0;

    while (xTaskGetTickCount() < deadline)
    {
        int len = uart_read_bytes(NEXTION_UART_NUM, &b, 1, pdMS_TO_TICKS(50));

        if (len != 1)
            continue;

        if (b == 0x05)
        {
            ESP_LOGI(TAG, "Received 0x05 ready byte");
            return true;
        }

        if (b == 0xFF)
        {
            ESP_LOGW(TAG, "Ignoring 0xFF while waiting for 0x05");
            continue;
        }

        ESP_LOGW(TAG, "Ignoring byte 0x%02X while waiting for 0x05", b);
    }

    ESP_LOGE(TAG, "Timeout waiting for 0x05");
    return false;
}

static bool hmi_nextion_read_until_ff3(char *out, size_t out_sz, uint32_t timeout_ms)
{
    if (out == NULL || out_sz < 4)
        return false;

    size_t idx = 0;
    uint8_t b = 0;
    int ff_count = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline && idx < (out_sz - 1))
    {
        int len = uart_read_bytes(NEXTION_UART_NUM, &b, 1, pdMS_TO_TICKS(50));
        if (len != 1)
            continue;

        out[idx++] = (char)b;

        if (b == 0xFF)
        {
            ff_count++;
            if (ff_count >= 3)
            {
                out[idx] = '\0';
                return true;
            }
        }
        else
        {
            ff_count = 0;
        }
    }

    out[idx] = '\0';
    return false;
}

static bool hmi_nextion_prepare_for_upload(void)
{
    char resp[128];

    uart_flush_input(NEXTION_UART_NUM);
    uart_wait_tx_done(NEXTION_UART_NUM, pdMS_TO_TICKS(100));

    /* empty instruction */
    {
        const uint8_t end[3] = {0xFF, 0xFF, 0xFF};
        uart_write_bytes(NEXTION_UART_NUM, end, 3);
        uart_wait_tx_done(NEXTION_UART_NUM, pdMS_TO_TICKS(100));
    }

    hmi_nextion_send_raw_cmd("sleep=0");
    hmi_nextion_send_raw_cmd("connect");

    if (!hmi_nextion_read_until_ff3(resp, sizeof(resp), 1000))
    {
        ESP_LOGE(TAG, "No connect response from Nextion");
        return false;
    }

    ESP_LOGI(TAG, "Nextion connect response: %s", resp);

    if (strstr(resp, "comok") == NULL)
    {
        ESP_LOGE(TAG, "Nextion did not return comok");
        return false;
    }

    return true;
}

static void hmi_nextion_send_raw_cmd(const char *cmd)
{
    uart_write_bytes(NEXTION_UART_NUM, cmd, strlen(cmd));
    const uint8_t end[3] = {0xFF, 0xFF, 0xFF};
    uart_write_bytes(NEXTION_UART_NUM, end, 3);
    uart_wait_tx_done(NEXTION_UART_NUM, pdMS_TO_TICKS(100));
}

bool hmi_nextion_tft_upload_from_file(const char *path, uint32_t baud)
{
    if (path == NULL || path[0] == '\0')
        return false;

    if (s_update_mutex == NULL)
    {
        s_update_mutex = xSemaphoreCreateMutex();
        if (s_update_mutex == NULL)
            return false;
    }

    if (xSemaphoreTake(s_update_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return false;

    if (s_update_in_progress)
    {
        xSemaphoreGive(s_update_mutex);
        ESP_LOGW(TAG, "Nextion update already in progress");
        return false;
    }

    s_update_in_progress = true;
    xSemaphoreGive(s_update_mutex);

    bool ok = false;
    FILE *fp = NULL;
    uint8_t *buf = NULL;

    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        ESP_LOGE(TAG, "Failed to open TFT file: %s", path);
        goto cleanup;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        ESP_LOGE(TAG, "fseek failed");
        goto cleanup;
    }

    long file_size = ftell(fp);
    if (file_size <= 0)
    {
        ESP_LOGE(TAG, "Invalid TFT file size");
        goto cleanup;
    }

    rewind(fp);

    if (baud == 0)
        baud = 115200;

    ESP_LOGI(TAG, "Starting TFT upload, file=%s size=%ld baud=%lu",
             path, file_size, (unsigned long)baud);

    uint32_t current_baud = 0;
    uart_get_baudrate(NEXTION_UART_NUM, &current_baud);
    ESP_LOGI(TAG, "Current UART baud before whmi-wri = %lu",
             (unsigned long)current_baud);

    uart_flush_input(NEXTION_UART_NUM);
    uart_wait_tx_done(NEXTION_UART_NUM, pdMS_TO_TICKS(100));

    if (!hmi_nextion_prepare_for_upload())
    {
        ESP_LOGE(TAG, "Nextion pre-upload handshake failed");
        goto cleanup;
    }

    uart_flush_input(NEXTION_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(50));

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "whmi-wri %ld,%lu,A", file_size, (unsigned long)baud);

    uart_write_bytes(NEXTION_UART_NUM, cmd, strlen(cmd));
    {
        const uint8_t end[3] = {0xFF, 0xFF, 0xFF};
        uart_write_bytes(NEXTION_UART_NUM, end, 3);
    }
    uart_wait_tx_done(NEXTION_UART_NUM, pdMS_TO_TICKS(500));

    if (baud != NEXTION_BAUDRATE)
    {
        ESP_LOGI(TAG, "Switching UART from %lu to upload baud %lu",
                 (unsigned long)current_baud,
                 (unsigned long)baud);

        uart_set_baudrate(NEXTION_UART_NUM, baud);
        vTaskDelay(pdMS_TO_TICKS(20));

        ESP_LOGI(TAG, "UART switched to upload baud %lu", (unsigned long)baud);
    }

    ESP_LOGI(TAG, "Sent whmi-wri, waiting for initial 0x05 at upload baud...");

    if (!hmi_nextion_wait_for_0x05(10000))
    {
        ESP_LOGE(TAG, "Initial 0x05 not received");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Initial 0x05 received");

    buf = malloc(4096);
    if (buf == NULL)
    {
        ESP_LOGE(TAG, "No memory for TFT upload buffer");
        goto cleanup;
    }

    size_t sent_total = 0;
    while (1)
    {
        size_t n = fread(buf, 1, 4096, fp);
        if (n == 0)
            break;

        int written = uart_write_bytes(NEXTION_UART_NUM, (const char *)buf, n);
        if (written < 0 || (size_t)written != n)
        {
            ESP_LOGE(TAG, "UART write failed during TFT upload");
            goto cleanup;
        }

        uart_wait_tx_done(NEXTION_UART_NUM, pdMS_TO_TICKS(5000));
        sent_total += n;

        ESP_LOGI(TAG, "Sent chunk: %u / %ld", (unsigned int)sent_total, file_size);
        ESP_LOGI(TAG, "Waiting for chunk ACK 0x05...");

        if (!hmi_nextion_wait_for_0x05(10000))
        {
            ESP_LOGE(TAG, "Chunk ACK 0x05 not received at %u / %ld",
                     (unsigned int)sent_total, file_size);
            goto cleanup;
        }

        ESP_LOGI(TAG, "Chunk ACK 0x05 received for %u / %ld",
                 (unsigned int)sent_total, file_size);

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ESP_LOGI(TAG, "All chunks sent successfully");
    ESP_LOGI(TAG, "Waiting for Nextion reboot/ready...");

    if (baud != NEXTION_BAUDRATE)
    {
        uart_set_baudrate(NEXTION_UART_NUM, NEXTION_BAUDRATE);
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "TFT upload complete");
    ok = true;

cleanup:
    if (fp != NULL)
        fclose(fp);

    if (buf != NULL)
        free(buf);

    if (s_update_mutex != NULL)
    {
        xSemaphoreTake(s_update_mutex, portMAX_DELAY);
        s_update_in_progress = false;
        xSemaphoreGive(s_update_mutex);
    }

    return ok;
}
