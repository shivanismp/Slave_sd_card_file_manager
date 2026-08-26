#include "global.h"
#include "mqtt_file_uploader.h"

#include "mqtt.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "MQTT_FILE_UPLOAD";

#define MQTT_FILE_PROTOCOL_VERSION          1U
// #define MQTT_FILE_CHUNK_DATA_BYTES          1024U
#define MQTT_FILE_CHUNK_DATA_BYTES          512U
#define MQTT_FILE_FRAME_HEADER_BYTES        12U
#define MQTT_FILE_ACK_JSON_BYTES            512U
#define MQTT_FILE_NAME_BYTES                64U
#define MQTT_FILE_PATH_BYTES                160U
#define MQTT_FILE_ACK_QUEUE_LENGTH          4U
#define MQTT_FILE_BROKER_ACK_TIMEOUT_MS     10000U
#define MQTT_FILE_BACKEND_READY_TIMEOUT_MS  15000U
#define MQTT_FILE_BACKEND_FINAL_TIMEOUT_MS  30000U
#define MQTT_FILE_RETRY_PERIOD_MS           10000U
#define MQTT_FILE_IDLE_SCAN_PERIOD_MS       2000U
#define MQTT_FILE_CHUNK_GAP_MS              100U
#define MQTT_FILE_CONNECTION_SETTLE_MS      3000U
// #define MQTT_FILE_TASK_STACK_BYTES          (8U * 1024U)
// #define MQTT_FILE_TASK_STACK_BYTES          (16U * 4095U)

typedef struct
{
    char text[MQTT_FILE_ACK_JSON_BYTES];
} mqtt_file_ack_message_t;

typedef struct
{
    char name[MQTT_FILE_NAME_BYTES];
    char pending_path[MQTT_FILE_PATH_BYTES];
    uint32_t size;
    uint32_t crc32;
} mqtt_file_info_t;

typedef enum
{
    MQTT_FILE_ACK_NONE = 0,
    MQTT_FILE_ACK_READY,
    MQTT_FILE_ACK_RECEIVED,
    MQTT_FILE_ACK_ERROR
} mqtt_file_ack_kind_t;

typedef struct
{
    mqtt_file_ack_kind_t kind;
    bool accepted;
    bool already_received;
    char file[MQTT_FILE_NAME_BYTES];
    char crc32[9];
} mqtt_file_ack_t;

static TaskHandle_t s_uploader_task = NULL;
static QueueHandle_t s_ack_queue = NULL;
static char s_topic_from_esp[MQTT_MAX_TOPIC_LEN];
static char s_topic_from_backend[MQTT_MAX_TOPIC_LEN];

/* MQTT callback reassembly state; accessed only by the MQTT event task. */
static bool s_ack_rx_active = false;
static int s_ack_rx_total = 0;
static char s_ack_rx_buffer[MQTT_FILE_ACK_JSON_BYTES];

static bool mqtt_file_topic_equals(const char *topic,
                                   int topic_len,
                                   const char *expected)
{
    if (topic == NULL || expected == NULL || topic_len < 0)
        return false;

    size_t expected_len = strlen(expected);
    return expected_len == (size_t)topic_len &&
           memcmp(topic, expected, expected_len) == 0;
}

static void mqtt_file_ack_rx_reset(void)
{
    s_ack_rx_active = false;
    s_ack_rx_total = 0;
    memset(s_ack_rx_buffer, 0, sizeof(s_ack_rx_buffer));
}

static void mqtt_file_data_observer(const char *topic,
                                    int topic_len,
                                    const uint8_t *data,
                                    int data_len,
                                    int total_data_len,
                                    int current_data_offset,
                                    void *context)
{
    (void)context;

    if (data == NULL || data_len <= 0 || total_data_len <= 0 ||
        current_data_offset < 0)
    {
        return;
    }

    if (current_data_offset == 0)
    {
        mqtt_file_ack_rx_reset();

        if (!mqtt_file_topic_equals(topic,
                                    topic_len,
                                    s_topic_from_backend))
        {
            return;
        }

        if (total_data_len >= (int)sizeof(s_ack_rx_buffer))
        {
            ESP_LOGW(TAG,
                     "Backend ACK too large: %d bytes",
                     total_data_len);
            return;
        }

        s_ack_rx_active = true;
        s_ack_rx_total = total_data_len;
    }

    if (!s_ack_rx_active || s_ack_rx_total != total_data_len ||
        current_data_offset > s_ack_rx_total ||
        data_len > (s_ack_rx_total - current_data_offset))
    {
        mqtt_file_ack_rx_reset();
        return;
    }

    memcpy(s_ack_rx_buffer + current_data_offset,
           data,
           (size_t)data_len);

    int received_end = current_data_offset + data_len;
    if (received_end < s_ack_rx_total)
        return;

    s_ack_rx_buffer[s_ack_rx_total] = '\0';

    if (s_ack_queue != NULL)
    {
        mqtt_file_ack_message_t message = {0};
        strlcpy(message.text,
                s_ack_rx_buffer,
                sizeof(message.text));

        if (xQueueSend(s_ack_queue, &message, 0) != pdTRUE)
        {
            mqtt_file_ack_message_t discarded;
            (void)xQueueReceive(s_ack_queue, &discarded, 0);
            (void)xQueueSend(s_ack_queue, &message, 0);
        }
    }

    mqtt_file_ack_rx_reset();
}

static uint32_t mqtt_file_crc32_update(uint32_t crc,
                                       const uint8_t *data,
                                       size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        crc ^= data[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit)
        {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }

    return crc;
}

static bool mqtt_file_measure(const char *path,
                              uint32_t *out_size,
                              uint32_t *out_crc32)
{
    if (path == NULL || out_size == NULL || out_crc32 == NULL)
        return false;

    struct stat file_stat;
    if (stat(path, &file_stat) != 0 || file_stat.st_size <= 0 ||
        (uint64_t)file_stat.st_size > UINT32_MAX)
    {
        return false;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return false;

    uint8_t buffer[MQTT_FILE_CHUNK_DATA_BYTES];
    uint32_t crc = UINT32_MAX;
    uint32_t total = 0U;
    bool ok = true;

    while (true)
    {
        size_t count = fread(buffer, 1U, sizeof(buffer), file);
        if (count != 0U)
        {
            crc = mqtt_file_crc32_update(crc, buffer, count);
            total += (uint32_t)count;
        }

        /*
        * SD and W6100 may share CPU/SPI resources.
        * Give Ethernet and MQTT time to run.
        */
        vTaskDelay(pdMS_TO_TICKS(2));

        if (count < sizeof(buffer))
        {
            if (ferror(file))
                ok = false;
            break;
        }
    }

    fclose(file);

    if (!ok || total != (uint32_t)file_stat.st_size)
        return false;

    *out_size = total;
    *out_crc32 = crc ^ UINT32_MAX;
    return true;
}

static bool mqtt_file_generated_name(const char *name)
{
    if (name == NULL || strlen(name) != 12U || name[0] != 'M' ||
        strcmp(name + 8, ".BIN") != 0)
    {
        return false;
    }

    for (size_t index = 1U; index <= 7U; ++index)
    {
        if (!isdigit((unsigned char)name[index]))
            return false;
    }

    return true;
}

static void mqtt_file_migrate_legacy_root_files(void)
{
    DIR *directory = opendir(SDCARD_BASE_PATH);
    if (directory == NULL)
        return;

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (!mqtt_file_generated_name(entry->d_name))
            continue;

        char old_path[MQTT_FILE_PATH_BYTES];
        char pending_path[MQTT_FILE_PATH_BYTES];

        int old_len = snprintf(old_path,
                               sizeof(old_path),
                               "%s/%s",
                               SDCARD_BASE_PATH,
                               entry->d_name);
        int pending_len = snprintf(pending_path,
                                   sizeof(pending_path),
                                   "%s/%s",
                                   MQTT_FILE_PENDING_DIR,
                                   entry->d_name);

        if (old_len <= 0 || old_len >= (int)sizeof(old_path) ||
            pending_len <= 0 || pending_len >= (int)sizeof(pending_path) ||
            sdcard_exists(pending_path))
        {
            continue;
        }

        if (rename(old_path, pending_path) == 0)
        {
            ESP_LOGI(TAG,
                     "Moved existing unsent file into PENDING: %s",
                     entry->d_name);
        }
    }

    closedir(directory);
}

static bool mqtt_file_find_next_pending(mqtt_file_info_t *info)
{
    if (info == NULL)
        return false;

    DIR *directory = opendir(MQTT_FILE_PENDING_DIR);
    if (directory == NULL)
        return false;

    char selected[MQTT_FILE_NAME_BYTES] = {0};
    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL)
    {
        if (!mqtt_file_generated_name(entry->d_name))
            continue;

        if (selected[0] == '\0' || strcmp(entry->d_name, selected) < 0)
        {
            strlcpy(selected, entry->d_name, sizeof(selected));
        }
    }

    closedir(directory);

    if (selected[0] == '\0')
        return false;

    memset(info, 0, sizeof(*info));
    strlcpy(info->name, selected, sizeof(info->name));

    int path_len = snprintf(info->pending_path,
                            sizeof(info->pending_path),
                            "%s/%s",
                            MQTT_FILE_PENDING_DIR,
                            selected);
    if (path_len <= 0 || path_len >= (int)sizeof(info->pending_path))
        return false;

    return mqtt_file_measure(info->pending_path,
                             &info->size,
                             &info->crc32);
}

static void mqtt_file_drain_ack_queue(void)
{
    if (s_ack_queue == NULL)
        return;

    mqtt_file_ack_message_t message;
    while (xQueueReceive(s_ack_queue, &message, 0) == pdTRUE)
    {
    }
}

static mqtt_file_ack_t mqtt_file_parse_ack(const char *text)
{
    mqtt_file_ack_t ack = {0};
    if (text == NULL)
        return ack;

    cJSON *root = cJSON_Parse(text);
    if (root == NULL)
        return ack;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(root, "file");
    const cJSON *crc32 = cJSON_GetObjectItemCaseSensitive(root, "crc32");
    const cJSON *accepted = cJSON_GetObjectItemCaseSensitive(root, "accepted");
    const cJSON *already =
        cJSON_GetObjectItemCaseSensitive(root, "already_received");

    if (cJSON_IsString(type) && type->valuestring != NULL)
    {
        if (strcmp(type->valuestring, "READY_ACK") == 0)
            ack.kind = MQTT_FILE_ACK_READY;
        else if (strcmp(type->valuestring, "FILE_RECEIVED") == 0)
            ack.kind = MQTT_FILE_ACK_RECEIVED;
        else if (strcmp(type->valuestring, "FILE_ERROR") == 0)
            ack.kind = MQTT_FILE_ACK_ERROR;
    }

    if (cJSON_IsString(file) && file->valuestring != NULL)
        strlcpy(ack.file, file->valuestring, sizeof(ack.file));

    if (cJSON_IsString(crc32) && crc32->valuestring != NULL)
        strlcpy(ack.crc32, crc32->valuestring, sizeof(ack.crc32));

    ack.accepted = cJSON_IsTrue(accepted);
    ack.already_received = cJSON_IsTrue(already);

    cJSON_Delete(root);
    return ack;
}

static bool mqtt_file_ack_matches(const mqtt_file_ack_t *ack,
                                  const mqtt_file_info_t *info)
{
    char expected_crc[9];
    snprintf(expected_crc, sizeof(expected_crc), "%08" PRIX32, info->crc32);

    return ack != NULL && info != NULL &&
           strcmp(ack->file, info->name) == 0 &&
           strcasecmp(ack->crc32, expected_crc) == 0;
}

static mqtt_file_ack_t mqtt_file_wait_for_ack(
    const mqtt_file_info_t *info,
    mqtt_file_ack_kind_t expected_kind,
    uint32_t timeout_ms)
{
    mqtt_file_ack_t no_ack = {0};
    if (s_ack_queue == NULL || info == NULL)
        return no_ack;

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start) <= timeout)
    {
        if (!mqtt_is_connected())
            return no_ack;

        mqtt_file_ack_message_t message;
        if (xQueueReceive(s_ack_queue,
                          &message,
                          pdMS_TO_TICKS(250)) != pdTRUE)
        {
            continue;
        }

        mqtt_file_ack_t ack = mqtt_file_parse_ack(message.text);
        if (!mqtt_file_ack_matches(&ack, info))
            continue;

        if (ack.kind == MQTT_FILE_ACK_ERROR || ack.kind == expected_kind)
            return ack;
    }

    return no_ack;
}

static bool mqtt_file_publish_and_wait(const void *payload,
                                       size_t payload_len)
{
    int message_id = -1;

    if (!mqtt_is_connected() ||
        !mqtt_publish_binary_ex(s_topic_from_esp,
                                payload,
                                payload_len,
                                1,
                                0,
                                &message_id))
    {
        return false;
    }

    return mqtt_wait_for_published(message_id,
                                   MQTT_FILE_BROKER_ACK_TIMEOUT_MS);
}

static void mqtt_file_put_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static void mqtt_file_put_u16_be(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8U);
    destination[1] = (uint8_t)value;
}

static bool mqtt_file_send_contents(const mqtt_file_info_t *info)
{
    FILE *file = fopen(info->pending_path, "rb");
    if (file == NULL)
        return false;

    uint8_t frame[MQTT_FILE_FRAME_HEADER_BYTES +
                  MQTT_FILE_CHUNK_DATA_BYTES];
    uint32_t offset = 0U;
    bool ok = true;

    while (offset < info->size)
    {
        size_t count = fread(frame + MQTT_FILE_FRAME_HEADER_BYTES,
                             1U,
                             MQTT_FILE_CHUNK_DATA_BYTES,
                             file);
        if (count == 0U)
        {
            ok = false;
            break;
        }

        memcpy(frame, "HMF1", 4U);
        mqtt_file_put_u32_be(frame + 4U, offset);
        mqtt_file_put_u16_be(frame + 8U, (uint16_t)count);
        mqtt_file_put_u16_be(frame + 10U, 0U);

        if (!mqtt_file_publish_and_wait(
                frame,
                MQTT_FILE_FRAME_HEADER_BYTES + count))
        {
            ok = false;
            break;
        }

        offset += (uint32_t)count;

        /* Allow MQTT, TLS and W6100 tasks to run. */
        vTaskDelay(pdMS_TO_TICKS(MQTT_FILE_CHUNK_GAP_MS));
    }

    if (ferror(file) || offset != info->size)
        ok = false;

    fclose(file);
    return ok;
}

static bool mqtt_file_mark_sent(const mqtt_file_info_t *info)
{
    char sent_path[MQTT_FILE_PATH_BYTES];
    int sent_len = snprintf(sent_path,
                            sizeof(sent_path),
                            "%s/%s",
                            MQTT_FILE_SENT_DIR,
                            info->name);
    if (sent_len <= 0 || sent_len >= (int)sizeof(sent_path))
        return false;

    if (sdcard_exists(sent_path))
    {
        uint32_t sent_size = 0U;
        uint32_t sent_crc = 0U;
        if (!mqtt_file_measure(sent_path, &sent_size, &sent_crc) ||
            sent_size != info->size || sent_crc != info->crc32)
        {
            ESP_LOGE(TAG,
                     "SENT name collision for %s; PENDING retained",
                     info->name);
            return false;
        }

        return unlink(info->pending_path) == 0;
    }

    if (rename(info->pending_path, sent_path) != 0)
    {
        ESP_LOGE(TAG,
                 "Cannot move %s to SENT: errno=%d (%s)",
                 info->name,
                 errno,
                 strerror(errno));
        return false;
    }

    ESP_LOGI(TAG,
             "Backend verified %s (%" PRIu32
             " bytes, CRC32=%08" PRIX32 "); moved to SENT",
             info->name,
             info->size,
             info->crc32);
    return true;
}

static bool mqtt_file_upload_one(const mqtt_file_info_t *info)
{
    char crc_text[9];
    char json[320];

    snprintf(crc_text, sizeof(crc_text), "%08" PRIX32, info->crc32);
    mqtt_file_drain_ack_queue();

    int json_length = snprintf(
        json,
        sizeof(json),
        "{\"type\":\"FILE_READY\",\"protocol\":%u,"
        "\"file\":\"%s\",\"size\":%" PRIu32 ","
        "\"crc32\":\"%s\",\"chunk_size\":%u}",
        MQTT_FILE_PROTOCOL_VERSION,
        info->name,
        info->size,
        crc_text,
        MQTT_FILE_CHUNK_DATA_BYTES);

    if (json_length <= 0 || json_length >= (int)sizeof(json) ||
        !mqtt_file_publish_and_wait(json, (size_t)json_length))
    {
        return false;
    }

    ESP_LOGI(TAG,
             "FILE_READY sent: %s, %" PRIu32 " bytes, CRC32=%s",
             info->name,
             info->size,
             crc_text);

    mqtt_file_ack_t ready = mqtt_file_wait_for_ack(
        info,
        MQTT_FILE_ACK_READY,
        MQTT_FILE_BACKEND_READY_TIMEOUT_MS);

    if (ready.kind != MQTT_FILE_ACK_READY || !ready.accepted)
    {
        ESP_LOGW(TAG,
                 "Backend did not accept %s; it remains PENDING",
                 info->name);
        return false;
    }

    if (ready.already_received)
        return mqtt_file_mark_sent(info);

    if (!mqtt_file_send_contents(info))
    {
        ESP_LOGW(TAG,
                 "Upload interrupted for %s; it remains PENDING",
                 info->name);
        return false;
    }

    json_length = snprintf(
        json,
        sizeof(json),
        "{\"type\":\"FILE_COMPLETE\",\"protocol\":%u,"
        "\"file\":\"%s\",\"size\":%" PRIu32 ","
        "\"crc32\":\"%s\"}",
        MQTT_FILE_PROTOCOL_VERSION,
        info->name,
        info->size,
        crc_text);

    if (json_length <= 0 || json_length >= (int)sizeof(json) ||
        !mqtt_file_publish_and_wait(json, (size_t)json_length))
    {
        return false;
    }

    mqtt_file_ack_t received = mqtt_file_wait_for_ack(
        info,
        MQTT_FILE_ACK_RECEIVED,
        MQTT_FILE_BACKEND_FINAL_TIMEOUT_MS);

    if (received.kind != MQTT_FILE_ACK_RECEIVED || !received.accepted)
    {
        ESP_LOGW(TAG,
                 "No verified FILE_RECEIVED for %s; it remains PENDING",
                 info->name);
        return false;
    }

    return mqtt_file_mark_sent(info);
}

// static void mqtt_file_uploader_task(void *argument)
// {
//     (void)argument;
//     bool legacy_migration_done = false;

//     while (true)
//     {
//         if (!sdcard_is_mounted())
//         {
//             (void)ulTaskNotifyTake(
//                 pdTRUE,
//                 pdMS_TO_TICKS(MQTT_FILE_IDLE_SCAN_PERIOD_MS));
//             continue;
//         }

//         if (sdcard_mkdirs(MQTT_FILE_PENDING_DIR) != ESP_OK ||
//             sdcard_mkdirs(MQTT_FILE_SENT_DIR) != ESP_OK)
//         {
//             (void)ulTaskNotifyTake(
//                 pdTRUE,
//                 pdMS_TO_TICKS(MQTT_FILE_RETRY_PERIOD_MS));
//             continue;
//         }

//         if (!legacy_migration_done)
//         {
//             mqtt_file_migrate_legacy_root_files();
//             legacy_migration_done = true;
//         }

//         if (!mqtt_is_connected())
//         {
//             (void)ulTaskNotifyTake(
//                 pdTRUE,
//                 pdMS_TO_TICKS(MQTT_FILE_IDLE_SCAN_PERIOD_MS));
//             continue;
//         }


//         mqtt_file_info_t info;
//         if (!mqtt_file_find_next_pending(&info))
//         {
//             (void)ulTaskNotifyTake(
//                 pdTRUE,
//                 pdMS_TO_TICKS(MQTT_FILE_IDLE_SCAN_PERIOD_MS));
//             continue;
//         }

//         if (mqtt_file_upload_one(&info))
//         {
//             /* Scan for the next pending file after a short pause. */
//             vTaskDelay(pdMS_TO_TICKS(500));

//             continue;
//         }

//         (void)ulTaskNotifyTake(
//             pdTRUE,
//             pdMS_TO_TICKS(MQTT_FILE_RETRY_PERIOD_MS));
//     }
// }

static void mqtt_file_uploader_task(void *argument)
{
    (void)argument;

    bool legacy_migration_done = false;
    bool mqtt_was_connected = false;

    while (true)
    {
        if (!sdcard_is_mounted())
        {
            mqtt_was_connected = false;

            (void)ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(MQTT_FILE_IDLE_SCAN_PERIOD_MS));

            continue;
        }

        if (sdcard_mkdirs(MQTT_FILE_PENDING_DIR) != ESP_OK ||
            sdcard_mkdirs(MQTT_FILE_SENT_DIR) != ESP_OK)
        {
            (void)ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(MQTT_FILE_RETRY_PERIOD_MS));

            continue;
        }

        if (!legacy_migration_done)
        {
            mqtt_file_migrate_legacy_root_files();
            legacy_migration_done = true;
        }

        /*
         * MQTT is currently disconnected.
         */
        if (!mqtt_is_connected())
        {
            mqtt_was_connected = false;

            (void)ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(MQTT_FILE_IDLE_SCAN_PERIOD_MS));

            continue;
        }

        /*
         * MQTT has just connected or reconnected.
         * Wait once before starting file transfer.
         */
        if (!mqtt_was_connected)
        {
            ESP_LOGI(TAG,
                     "MQTT connected; waiting %u ms before file upload",
                     MQTT_FILE_CONNECTION_SETTLE_MS);

            vTaskDelay(
                pdMS_TO_TICKS(MQTT_FILE_CONNECTION_SETTLE_MS));

            /*
             * MQTT may have disconnected during the delay.
             */
            if (!mqtt_is_connected())
            {
                mqtt_was_connected = false;
                continue;
            }

            mqtt_was_connected = true;

            ESP_LOGI(TAG,
                     "MQTT connection stable; uploader can start");
        }

        mqtt_file_info_t info;

        if (!mqtt_file_find_next_pending(&info))
        {
            (void)ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(MQTT_FILE_IDLE_SCAN_PERIOD_MS));

            continue;
        }

        ESP_LOGI(TAG,
                 "Starting upload: %s, size=%" PRIu32
                 ", CRC32=%08" PRIX32,
                 info.name,
                 info.size,
                 info.crc32);

        if (mqtt_file_upload_one(&info))
        {
            /*
             * Give MQTT, telemetry and Ethernet some time
             * before scanning and uploading the next file.
             */
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /*
         * Upload failed. If MQTT disconnected, the next loop
         * will reset mqtt_was_connected and wait after reconnect.
         */
        if (!mqtt_is_connected())
        {
            mqtt_was_connected = false;
        }

        (void)ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(MQTT_FILE_RETRY_PERIOD_MS));
    }
}



esp_err_t mqtt_file_uploader_start(void)
{
    if (s_uploader_task != NULL)
        return ESP_OK;

    if (mqtt_serial_no[0] == '\0')
        return ESP_ERR_INVALID_STATE;

    int outgoing_len = snprintf(s_topic_from_esp,
                                sizeof(s_topic_from_esp),
                                "%s/FROM_ESP32",
                                mqtt_serial_no);
    int incoming_len = snprintf(s_topic_from_backend,
                                sizeof(s_topic_from_backend),
                                "%s/FROM_BACKEND",
                                mqtt_serial_no);

    if (outgoing_len <= 0 || outgoing_len >= (int)sizeof(s_topic_from_esp) ||
        incoming_len <= 0 || incoming_len >= (int)sizeof(s_topic_from_backend))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_ack_queue == NULL)
    {
        s_ack_queue = xQueueCreate(MQTT_FILE_ACK_QUEUE_LENGTH,
                                   sizeof(mqtt_file_ack_message_t));
        if (s_ack_queue == NULL)
            return ESP_ERR_NO_MEM;
    }

    if (!mqtt_register_data_observer(s_topic_from_backend,
                                     mqtt_file_data_observer,
                                     NULL))
    {
        return ESP_FAIL;
    }

    BaseType_t task_result = xTaskCreate(mqtt_file_uploader_task,
                                         "mqtt_file_upload",
                                         mqtt_file_uploader_task_stack_size_bytes,
                                         NULL,
                                         mqtt_file_uploader_task_priority,
                                         &s_uploader_task);
    if (task_result != pdPASS)
    {
        s_uploader_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Uploader ready: publish=%s subscribe=%s",
             s_topic_from_esp,
             s_topic_from_backend);
    return ESP_OK;
}

void mqtt_file_uploader_notify_file_ready(void)
{
    if (s_uploader_task != NULL)
        xTaskNotifyGive(s_uploader_task);
}
