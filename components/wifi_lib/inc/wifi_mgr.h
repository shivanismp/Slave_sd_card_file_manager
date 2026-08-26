#ifndef __WIFI_MGR_H__
#define __WIFI_MGR_H__
#include <stdbool.h>
#include <esp_err.h>

#define WIFI_SCAN_MAX_SSIDS   20
#define WIFI_SSID_STR_LEN     33   // 32 + null

typedef struct
{
    char ssids[WIFI_SCAN_MAX_SSIDS][WIFI_SSID_STR_LEN];
    int count;
} wifi_scan_list_t;

extern volatile int wifi_mgr_current_event_id;

bool wifi_mgr_is_connecting(void);
bool wifi_mgr_is_connected(void);
void wifi_mgr_init(void);
bool wifi_mgr_try_connect_best_known(int timeout_ms);
bool sta_connect_to(const char *ssid, const char *pass, int timeout_ms);
void wifi_mgr_start_softap(const char *ap_ssid, const char *ap_pass);
esp_err_t wifi_scan_get_ssids(wifi_scan_list_t *list);


#endif