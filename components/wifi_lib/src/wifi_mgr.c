#include "wifi_mgr.h"
#include "wifi_db.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"

volatile int wifi_mgr_current_event_id = -1;

static const char *TAG = "WIFI_MGR";

static EventGroupHandle_t s_wifi_eg = NULL;
static bool s_wifi_inited = false;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry = 0;
#define TRY_RETRY_PER_SSID 6

static volatile bool s_sta_connecting = false;

bool wifi_mgr_is_connecting(void)
{
    return s_sta_connecting;
}

bool wifi_mgr_is_connected(void)
{
    if (s_wifi_eg == NULL)
        return false;

    EventBits_t bits = xEventGroupGetBits(s_wifi_eg);
    return ((bits & WIFI_CONNECTED_BIT) != 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    wifi_mgr_current_event_id = event_id;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        s_sta_connecting = false;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_sta_connecting = true;
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);

        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;

        if (s_retry < TRY_RETRY_PER_SSID)
        {
            s_retry++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d, reason=%d",
                     s_retry, TRY_RETRY_PER_SSID, disc ? disc->reason : -1);

            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Retry esp_wifi_connect failed: %s", esp_err_to_name(err));
            }
        }
        else
        {
            s_sta_connecting = false;
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_retry = 0;
        s_sta_connecting = false;
        xEventGroupClearBits(s_wifi_eg, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

void wifi_mgr_init(void)
{
    if (s_wifi_inited)
        return;

    s_wifi_eg = xEventGroupCreate();
    configASSERT(s_wifi_eg != NULL);

    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t err;

    err = esp_netif_init();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }

    err = esp_event_loop_create_default();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_inited = true;
    ESP_LOGI(TAG, "Wi-Fi manager initialized");
}

bool sta_connect_to(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t cfg = {0};

    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    cfg.sta.scan_method = WIFI_FAST_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "Trying SSID: %s", ssid);

    s_retry = 0;
    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t err;

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return false;
    }
    s_sta_connecting = true;
    err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_connect returned: %s", esp_err_to_name(err));
        s_sta_connecting = false;
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to %s", ssid);
        return true;
    }

    ESP_LOGW(TAG, "Failed SSID: %s", ssid);
    return false;
}

bool wifi_mgr_try_connect_best_known(int timeout_ms)
{
    wifi_cred_t known[WIFI_DB_MAX_RECORDS] = {0};
    int known_n = 0;

    if (!wifi_db_load(known, &known_n) || known_n == 0)
    {
        ESP_LOGW(TAG, "No known networks in DB");
        return false;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE};

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t ap_num = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
    if (ap_num == 0)
    {
        ESP_LOGW(TAG, "No APs found in scan");
        return false;
    }

    wifi_ap_record_t *aps = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!aps)
        return false;

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, aps));

    typedef struct
    {
        int k;
        int rssi;
    } cand_t;

    cand_t cands[WIFI_DB_MAX_RECORDS] = {0};
    int ccount = 0;

    for (int k = 0; k < known_n; k++)
    {
        int best_rssi = -999;

        for (int i = 0; i < ap_num; i++)
        {
            if (strncmp((char *)aps[i].ssid, known[k].ssid, WIFI_SSID_MAX) == 0)
            {
                if (aps[i].rssi > best_rssi)
                    best_rssi = aps[i].rssi;
            }
        }

        if (best_rssi > -999)
        {
            cands[ccount].k = k;
            cands[ccount].rssi = best_rssi;
            ccount++;
        }
    }

    free(aps);

    if (ccount == 0)
    {
        ESP_LOGW(TAG, "No known SSIDs found in current scan");
        return false;
    }

    for (int a = 0; a < ccount; a++)
    {
        for (int b = a + 1; b < ccount; b++)
        {
            if (cands[b].rssi > cands[a].rssi)
            {
                cand_t t = cands[a];
                cands[a] = cands[b];
                cands[b] = t;
            }
        }
    }

    for (int c = 0; c < ccount; c++)
    {
        const wifi_cred_t *cred = &known[cands[c].k];
        if (sta_connect_to(cred->ssid, cred->pass, timeout_ms))
            return true;
    }

    return false;
}

void wifi_mgr_start_softap(const char *ap_ssid, const char *ap_pass)
{
    wifi_config_t ap_cfg = {0};

    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (!ap_pass || !ap_pass[0])
    {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    else
    {
        strlcpy((char *)ap_cfg.ap.password, ap_pass, sizeof(ap_cfg.ap.password));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    ESP_LOGI(TAG, "SoftAP started SSID=%s", ap_ssid);
}

esp_err_t wifi_scan_get_ssids(wifi_scan_list_t *list)
{
    if (list == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(list, 0, sizeof(wifi_scan_list_t));

    wifi_scan_config_t scan_cfg = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE};

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK)
    {
        return err;
    }

    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK)
    {
        return err;
    }

    if (ap_num == 0)
    {
        list->count = 0;
        return ESP_OK;
    }

    wifi_ap_record_t *aps = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (aps == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&ap_num, aps);
    if (err != ESP_OK)
    {
        free(aps);
        return err;
    }

    int count = (ap_num > WIFI_SCAN_MAX_SSIDS) ? WIFI_SCAN_MAX_SSIDS : ap_num;

    for (int i = 0; i < count; i++)
    {
        strncpy(list->ssids[i], (char *)aps[i].ssid, WIFI_SSID_STR_LEN - 1);
        list->ssids[i][WIFI_SSID_STR_LEN - 1] = '\0';
    }

    list->count = count;

    free(aps);
    return ESP_OK;
}