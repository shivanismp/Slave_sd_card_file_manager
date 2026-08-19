#include "nextion_update.h"
#include "mqtt.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "hmi_runtime.h"
#include <unistd.h>

static const char *TAG = "NX_UPDATE";

typedef struct
{
    char version[32];
    char url[2048];
    int baud;
    bool force;
} nextion_update_req_t;

static SemaphoreHandle_t s_nx_mutex = NULL;
static bool s_nx_in_progress = false;

bool nextion_update_is_in_progress(void)
{
    return s_nx_in_progress;
}

static void nextion_publish_status(const char *state,
                                   const char *version,
                                   const char *detail,
                                   int progress)
{
    if (mqtt_topic_nx_update_status[0] == '\0')
        return;

    char payload[320];

    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"version\":\"%s\",\"detail\":\"%s\",\"progress\":%d}",
             state ? state : "",
             version ? version : "",
             detail ? detail : "",
             progress);

    mqtt_publish_text(mqtt_topic_nx_update_status, payload, 1, 0);
}


static bool nextion_download_tft_to_file(const char *url,
                                         const char *path,
                                         const char *version)
{
    static const char *TAG_DL = "NX_DL";

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL)
    {
        ESP_LOGE(TAG_DL, "esp_http_client_init failed");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DL, "esp_http_client_open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int fetch_ret = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    long long content_length = esp_http_client_get_content_length(client);

    ESP_LOGI(TAG_DL, "fetch_headers=%d status=%d content_length=%lld",
             fetch_ret, status_code, content_length);

    if (status_code != 200)
    {
        char body[256];
        int r = esp_http_client_read(client, body, sizeof(body) - 1);
        if (r > 0)
        {
            body[r] = '\0';
            ESP_LOGE(TAG_DL, "HTTP error body: %s", body);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    FILE *fp = fopen(path, "wb");
    if (fp == NULL)
    {
        ESP_LOGE(TAG_DL, "Failed to open output file: %s", path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    uint8_t *buf = malloc(4096);
    if (buf == NULL)
    {
        ESP_LOGE(TAG_DL, "No memory for download buffer");
        fclose(fp);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    long long total_read = 0;
    int last_progress = -1;
    bool ok = false;

    while (1)
    {
        int r = esp_http_client_read(client, (char *)buf, 4096);
        if (r < 0)
        {
            ESP_LOGE(TAG_DL, "esp_http_client_read failed");
            break;
        }
        if (r == 0)
        {
            ok = true;
            break;
        }

        size_t written = fwrite(buf, 1, (size_t)r, fp);
        if (written != (size_t)r)
        {
            ESP_LOGE(TAG_DL, "File write failed");
            break;
        }

        total_read += r;

        if (content_length > 0)
        {
            int progress = (int)((total_read * 100) / content_length);
            if (progress != last_progress && (progress % 5) == 0)
            {
                last_progress = progress;
                nextion_publish_status("downloading", version, "downloading_tft", progress);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    free(buf);
    fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok)
    {
        unlink(path);
        return false;
    }

    nextion_publish_status("downloading", version, "download_complete", 100);
    return true;
}



static void nextion_update_task(void *arg)
{
    nextion_update_req_t *req = (nextion_update_req_t *)arg;
    const char *tft_path = "/littlefs/nextion.tft";

    ESP_LOGI(TAG, "nextion_update_task entered");

    if (s_nx_mutex == NULL)
    {
        s_nx_mutex = xSemaphoreCreateMutex();
        configASSERT(s_nx_mutex != NULL);
    }

    if (xSemaphoreTake(s_nx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        nextion_publish_status("error", req->version, "nx_mutex_timeout", -1);
        free(req);
        vTaskDelete(NULL);
        return;
    }

    if (s_nx_in_progress)
    {
        xSemaphoreGive(s_nx_mutex);
        nextion_publish_status("error", req->version, "nx_already_running", -1);
        free(req);
        vTaskDelete(NULL);
        return;
    }

    s_nx_in_progress = true;
    xSemaphoreGive(s_nx_mutex);

    ESP_LOGI(TAG, "Nextion update version=%s", req->version);
    ESP_LOGI(TAG, "Nextion update baud=%d", req->baud);
    ESP_LOGI(TAG, "Nextion URL len=%d", (int)strlen(req->url));

    nextion_publish_status("starting", req->version, "nx_update_begin", 0);

    unlink(tft_path);

    if (!nextion_download_tft_to_file(req->url, tft_path, req->version))
    {
        ESP_LOGE(TAG, "Failed to download TFT file");
        nextion_publish_status("error", req->version, "tft_download_failed", -1);
        goto cleanup;
    }

    nextion_publish_status("uploading", req->version, "starting_uart_upload", 0);

    if (!hmi_nextion_tft_upload_from_file(tft_path, (uint32_t)req->baud))
    {
        ESP_LOGE(TAG, "Nextion TFT upload failed");
        nextion_publish_status("error", req->version, "tft_upload_failed", -1);
        goto cleanup;
    }

    nextion_publish_status("done", req->version, "restarting HMI", 100);
    ESP_LOGI(TAG, "Nextion TFT update successful");
    nextion_publish_status("restarting", req->version, "nextion_update_complete", 100);
    esp_restart();

cleanup:
    unlink(tft_path);

    if (s_nx_mutex)
    {
        xSemaphoreTake(s_nx_mutex, portMAX_DELAY);
        s_nx_in_progress = false;
        xSemaphoreGive(s_nx_mutex);
    }

    free(req);
    vTaskDelete(NULL);
}



bool nextion_update_request_from_json(const char *json, size_t len)
{
    if (json == NULL || len == 0)
    {
        ESP_LOGE(TAG, "NX JSON empty");
        return false;
    }

    char *buf = calloc(1, len + 1);
    if (!buf)
    {
        ESP_LOGE(TAG, "NX JSON alloc failed");
        return false;
    }

    memcpy(buf, json, len);
    buf[len] = '\0';

    ESP_LOGI(TAG, "NX JSON RX len=%d", (int)len);

    cJSON *root = cJSON_Parse(buf);
    if (!root)
    {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "NX cJSON parse failed near: %s", err ? err : "unknown");
        free(buf);
        return false;
    }

    cJSON *j_version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *j_url     = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *j_baud    = cJSON_GetObjectItemCaseSensitive(root, "baud");
    cJSON *j_force   = cJSON_GetObjectItemCaseSensitive(root, "force");

    if (!cJSON_IsString(j_version) || !cJSON_IsString(j_url) ||
        j_version->valuestring == NULL || j_url->valuestring == NULL)
    {
        ESP_LOGE(TAG, "NX JSON missing required fields");
        cJSON_Delete(root);
        free(buf);
        return false;
    }

    nextion_update_req_t *req = calloc(1, sizeof(nextion_update_req_t));
    if (!req)
    {
        cJSON_Delete(root);
        free(buf);
        return false;
    }

    
    strlcpy(req->version, j_version->valuestring, sizeof(req->version));
    strlcpy(req->url, j_url->valuestring, sizeof(req->url));
    req->baud = cJSON_IsNumber(j_baud) ? j_baud->valueint : 115200;
    req->force = cJSON_IsTrue(j_force);
    
    cJSON_Delete(root);
    free(buf);
    
    ESP_LOGI(TAG, "NX JSON RX len=%d", (int)len);
    ESP_LOGI(TAG, "NX version=%s", req->version);
    ESP_LOGI(TAG, "NX url len=%d", (int)strlen(req->url));
    ESP_LOGI(TAG, "NX baud=%d", req->baud);
    ESP_LOGI(TAG, "Creating nextion_update_task...");
    BaseType_t ok = xTaskCreate(nextion_update_task,
                                "nextion_update_task",
                                16384,
                                req,
                                5,
                                NULL);

    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create nextion_update_task");
        free(req);
        return false;
    }

    ESP_LOGI(TAG, "nextion_update_task created");
    return true;
}
