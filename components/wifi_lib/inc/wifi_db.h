#ifndef __WIFI_DB_H__
#define __WIFI_DB_H__

#include <stdint.h>
#include <stdbool.h>

// #define WIFI_DB_PATH          "/spiffs/wifi_db.bin"
#define WIFI_DB_PATH          "/littlefs/wifi_db.bin"
#define WIFI_DB_MAX_RECORDS   20
#define WIFI_SSID_MAX         32
#define WIFI_PASS_MAX         64

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    char pass[WIFI_PASS_MAX + 1];
} wifi_cred_t;

bool wifi_db_load(wifi_cred_t *out_list, int *out_count);
bool wifi_db_save_all(const wifi_cred_t *list, int count);
bool wifi_db_add_or_update(const char *ssid, const char *pass);
bool wifi_db_delete(const char *ssid);

#endif