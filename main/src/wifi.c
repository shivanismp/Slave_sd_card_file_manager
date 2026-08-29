#include "wifi.h"
#include "esp_log_tags.h"

// static const char *TAG_WIFI = "WIFI_HTTP";
static int retry_num = 0;
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* Wi-Fi event handler */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (retry_num < MAX_RETRY)
        {
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG_WIFI, "Retrying WiFi connection...");
        }
        else
        {
            ESP_LOGI(TAG_WIFI, "Connect to AP failed");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_WIFI, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* HTTP event handler */
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            printf("%.*s", evt->data_len, (char *)evt->data);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* Wi-Fi init function */
void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    // ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");
}

// /* HTTP GET request */
// void http_get_task(void *pvParameters) {
//     xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
//                         false, true, portMAX_DELAY);

//     esp_http_client_config_t config = {
//         .url = "http://httpbin.org/get",
//         .event_handler = _http_event_handler,
//     };

//     esp_http_client_handle_t client = esp_http_client_init(&config);

//     ESP_ERROR_CHECK(esp_http_client_perform(client));
//     ESP_LOGI(TAG_WIFI, "HTTP GET Status = %d, content_length = %lld",
//              esp_http_client_get_status_code(client),
//              esp_http_client_get_content_length(client));

//     esp_http_client_cleanup(client);
//     vTaskDelete(NULL);
// }

void wifi_scan(void)
{
    // Configure scan type (active) and channel (all)
    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = true};

    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true)); // true = block until scan done

    uint16_t ap_num = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
    ESP_LOGI(TAG_WIFI, "Total APs found: %d", ap_num);

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap_list)
    {
        ESP_LOGE(TAG_WIFI, "Failed to malloc memory for AP list");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, ap_list));

    for (int i = 0; i < ap_num; i++)
    {
        ESP_LOGI(TAG_WIFI, "[%2d] SSID: %s, RSSI: %d dBm, Channel: %d",
                 i + 1,
                 (char *)ap_list[i].ssid,
                 ap_list[i].rssi,
                 ap_list[i].primary);
    }

    free(ap_list);
}

void app_wifi(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    wifi_scan();

    // xTaskCreate(&http_get_task, "http_get_task", 8192, NULL, 5, NULL);
}
