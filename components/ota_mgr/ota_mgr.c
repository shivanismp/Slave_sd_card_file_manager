#include "ota_mgr.h"
#include "mqtt.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include "cJSON.h"

static const char *TAG = "OTA_MGR";

typedef struct
{
    char version[32];
    char url[2048];
    bool force;
} ota_req_t;

static SemaphoreHandle_t s_ota_mutex = NULL;
static bool s_ota_in_progress = false;

bool __attribute__((weak)) app_ota_self_test(void)
{
    return true;
}

static int parse_semver4(const char *s, int out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (s == NULL || *s == '\0')
        return -1;

    int n = sscanf(s, "%d.%d.%d.%d", &out[0], &out[1], &out[2], &out[3]);
    return (n >= 1) ? 0 : -1;
}

static int semver_compare(const char *a, const char *b)
{
    int av[4] = {0}, bv[4] = {0};

    if (parse_semver4(a, av) != 0 || parse_semver4(b, bv) != 0)
    {
        /* fallback: lexical */
        return strcmp(a ? a : "", b ? b : "");
    }

    for (int i = 0; i < 4; i++)
    {
        if (av[i] < bv[i]) return -1;
        if (av[i] > bv[i]) return 1;
    }

    return 0;
}

static void ota_publish_status(const char *state,
                               const char *version,
                               const char *detail,
                               int progress)
{
    if (mqtt_topic_ota_status[0] == '\0')
        return;

    char payload[320];

    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"version\":\"%s\",\"detail\":\"%s\",\"progress\":%d}",
             state ? state : "",
             version ? version : "",
             detail ? detail : "",
             progress);

    mqtt_publish_text(mqtt_topic_ota_status, payload, 1, 0);
}

bool ota_is_in_progress(void)
{
    return s_ota_in_progress;
}

bool ota_mark_running_app_valid_if_needed(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(running, &state) != ESP_OK)
    {
        return true;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return true;
    }

    ESP_LOGI(TAG, "Running image is pending verify");

    if (app_ota_self_test())
    {
        ESP_LOGI(TAG, "Self test passed, marking app valid");
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        return true;
    }

    ESP_LOGE(TAG, "Self test failed, rolling back");
    ESP_ERROR_CHECK(esp_ota_mark_app_invalid_rollback_and_reboot());
    return false;
}

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ERROR:
            ESP_LOGE("OTA_HTTP", "HTTP_EVENT_ERROR");
            break;

        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI("OTA_HTTP", "HTTP_EVENT_ON_CONNECTED");
            break;

        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGI("OTA_HTTP", "HTTP_EVENT_HEADERS_SENT");
            break;

        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI("OTA_HTTP", "HEADER %s: %s",
                     evt->header_key ? evt->header_key : "",
                     evt->header_value ? evt->header_value : "");
            break;

        case HTTP_EVENT_REDIRECT:
            ESP_LOGW("OTA_HTTP", "HTTP_EVENT_REDIRECT");
            break;

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI("OTA_HTTP", "HTTP_EVENT_ON_FINISH");
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI("OTA_HTTP", "HTTP_EVENT_DISCONNECTED");
            break;

        default:
            break;
    }

    return ESP_OK;
}

static void ota_task(void *arg)
{
    ota_req_t *req = (ota_req_t *)arg;
    esp_err_t ret = ESP_FAIL;
    esp_https_ota_handle_t ota_handle = NULL;
    esp_app_desc_t new_app_info;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update  = esp_ota_get_next_update_partition(NULL);

    ESP_LOGI(TAG, "Running partition: label=%s subtype=%d addr=0x%lx",
             running ? running->label : "null",
             running ? running->subtype : -1,
             running ? (unsigned long)running->address : 0UL);

    ESP_LOGI(TAG, "Update partition: label=%s subtype=%d addr=0x%lx size=0x%lx",
             update ? update->label : "null",
             update ? update->subtype : -1,
             update ? (unsigned long)update->address : 0UL,
             update ? (unsigned long)update->size : 0UL);

    if (s_ota_mutex == NULL)
    {
        s_ota_mutex = xSemaphoreCreateMutex();
        configASSERT(s_ota_mutex != NULL);
    }

    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ota_publish_status("error", req->version, "ota_mutex_timeout", -1);
        free(req);
        vTaskDelete(NULL);
        return;
    }

    if (s_ota_in_progress)
    {
        xSemaphoreGive(s_ota_mutex);
        ota_publish_status("error", req->version, "ota_already_running", -1);
        free(req);
        vTaskDelete(NULL);
        return;
    }

    s_ota_in_progress = true;
    xSemaphoreGive(s_ota_mutex);

    const esp_app_desc_t *running_desc = esp_app_get_description();

    ESP_LOGI(TAG, "OTA version=%s", req->version);
    ESP_LOGI(TAG, "OTA url len=%d", (int)strlen(req->url));
    ESP_LOGI(TAG, "Current version: %s", running_desc->version);
    ESP_LOGI(TAG, "Requested version: %s", req->version);

    if (!req->force && semver_compare(req->version, running_desc->version) <= 0)
    {
        ota_publish_status("rejected", req->version, "version_not_newer", -1);
        goto cleanup;
    }

    ota_publish_status("starting", req->version, "begin_https_ota", 0);

    esp_http_client_config_t http_cfg = {
        .url = req->url,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .event_handler = ota_http_event_handler,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
        .partial_http_download = false,
    };

    ret = esp_https_ota_begin(&ota_cfg, &ota_handle);
    ESP_LOGI(TAG, "esp_https_ota_begin ret=%s", esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        ota_publish_status("error", req->version, "ota_begin_failed", -1);
        goto cleanup;
    }

    memset(&new_app_info, 0, sizeof(new_app_info));
    ret = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    ESP_LOGI(TAG, "esp_https_ota_get_img_desc ret=%s", esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        ota_publish_status("error", req->version, "img_desc_failed", -1);
        goto cleanup;
    }

    ESP_LOGI(TAG, "NEW IMG project=%s", new_app_info.project_name);
    ESP_LOGI(TAG, "NEW IMG version=%s", new_app_info.version);
    ESP_LOGI(TAG, "NEW IMG idf=%s", new_app_info.idf_ver);
    ESP_LOGI(TAG, "NEW IMG secure_version=%ld", new_app_info.secure_version);

    if (!req->force && semver_compare(new_app_info.version, running_desc->version) <= 0)
    {
        ESP_LOGW(TAG, "Downloaded image version is not newer");
        ota_publish_status("rejected", new_app_info.version, "downloaded_version_not_newer", -1);
        goto cleanup;
    }

    while (1)
    {
        ret = esp_https_ota_perform(ota_handle);

        if (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            int read_len = esp_https_ota_get_image_len_read(ota_handle);
            ESP_LOGI(TAG, "OTA in progress, image_len_read=%d", read_len);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        break;
    }

    ESP_LOGI(TAG, "esp_https_ota_perform final ret=%s", esp_err_to_name(ret));
    if (ret != ESP_OK)
    {
        ota_publish_status("error", req->version, "ota_perform_failed", -1);
        goto cleanup;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle))
    {
        ESP_LOGE(TAG, "Complete OTA image not received");
        ota_publish_status("error", req->version, "ota_incomplete", -1);
        goto cleanup;
    }

    ret = esp_https_ota_finish(ota_handle);
    ota_handle = NULL;   // finish consumes handle
    ESP_LOGI(TAG, "esp_https_ota_finish ret=%s", esp_err_to_name(ret));

    if (ret != ESP_OK)
    {
        ota_publish_status("error", req->version, "ota_finish_failed", -1);
        goto cleanup;
    }

    ota_publish_status("done", req->version, "restarting", 100);
    ESP_LOGI(TAG, "OTA successful, restarting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

cleanup:
    if (ota_handle != NULL)
    {
        esp_https_ota_abort(ota_handle);
        ota_handle = NULL;
    }

    if (s_ota_mutex)
    {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        s_ota_in_progress = false;
        xSemaphoreGive(s_ota_mutex);
    }

    free(req);
    vTaskDelete(NULL);
}



bool ota_request_from_json(const char *json, size_t len)
{
    if (json == NULL || len == 0)
    {
        ESP_LOGE(TAG, "OTA JSON empty");
        return false;
    }

    char *buf = calloc(1, len + 1);
    if (!buf)
    {
        ESP_LOGE(TAG, "OTA JSON alloc failed");
        return false;
    }

    memcpy(buf, json, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
    {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "cJSON parse failed near: %s", err ? err : "unknown");
        free(buf);
        return false;
    }

    cJSON *j_version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *j_url     = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *j_force   = cJSON_GetObjectItemCaseSensitive(root, "force");

    if (!cJSON_IsString(j_version) || !cJSON_IsString(j_url) ||
        j_version->valuestring == NULL || j_url->valuestring == NULL)
    {
        ESP_LOGE(TAG, "OTA JSON missing required fields");
        cJSON_Delete(root);
        free(buf);
        return false;
    }

    ota_req_t *req = calloc(1, sizeof(ota_req_t));
    if (!req)
    {
        cJSON_Delete(root);
        free(buf);
        return false;
    }

    strlcpy(req->version, j_version->valuestring, sizeof(req->version));
    strlcpy(req->url, j_url->valuestring, sizeof(req->url));
    req->force = cJSON_IsTrue(j_force);

    cJSON_Delete(root);
    free(buf);

    BaseType_t ok = xTaskCreate(ota_task, "ota_task", 18432, req, 5, NULL);
    if (ok != pdPASS)
    {
        free(req);
        return false;
    }

    return true;
}
