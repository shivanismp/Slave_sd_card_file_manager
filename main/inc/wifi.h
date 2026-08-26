#ifndef __WIFI_H__
#define __WIFI_H__

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"

#define WIFI_SSID      "Shapet R&D"
#define WIFI_PASS      "shapet@123"
#define MAX_RETRY      5

void wifi_scan(void);
void app_wifi(void);

#endif