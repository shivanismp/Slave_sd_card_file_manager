#include "wifi_db.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WIFI_DB";
#define WIFI_DB_MAGIC 0x57494649u // "WIFI"
#define WIFI_DB_VER 1

typedef struct
{
    uint32_t magic;
    uint16_t ver;
    uint16_t count;
    uint32_t crc32;
} wifi_db_hdr_t;

static uint32_t crc32_simple(const uint8_t *data, size_t len)
{
    // Simple CRC32 (good enough for corruption detection)
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
        {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// bool wifi_db_load(wifi_cred_t *out_list, int *out_count)
// {
//     if (!out_list || !out_count) return false;

//     FILE *f = fopen(WIFI_DB_PATH, "rb");
//     if (!f) {
//         *out_count = 0;
//         return true; // no db yet is OK
//     }

//     wifi_db_hdr_t hdr = {0};
//     if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
//         fclose(f);
//         *out_count = 0;
//         return false;
//     }

//     printf("HELLO!!!!\n");
//     while (true)
//     {
//         vTaskDelay(pdMS_TO_TICKS(20));
//     }

//     if (hdr.magic != WIFI_DB_MAGIC || hdr.ver != WIFI_DB_VER || hdr.count > WIFI_DB_MAX_RECORDS) {
//         fclose(f);
//         *out_count = 0;
//         return false;
//     }

//     wifi_cred_t list[WIFI_DB_MAX_RECORDS] = {0};
//     size_t need = hdr.count * sizeof(wifi_cred_t);
//     if (need && fread(list, 1, need, f) != need)
//     {
//         fclose(f);
//         *out_count = 0;
//         return false;
//     }
//     fclose(f);

//     uint32_t calc = crc32_simple((const uint8_t*)list, need);
//     if (calc != hdr.crc32) {
//         ESP_LOGW(TAG, "DB CRC mismatch (file corrupted?)");
//         *out_count = 0;
//         return false;
//     }

//     memcpy(out_list, list, need);
//     *out_count = hdr.count;
//     return true;
// }

bool wifi_db_load(wifi_cred_t *out_list, int *out_count)
{
    if (!out_list || !out_count)
        return false;

    FILE *f = fopen(WIFI_DB_PATH, "rb");
    if (!f)
    {
        *out_count = 0;
        return true; // no db yet is OK
    }

    wifi_db_hdr_t hdr = {0};
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    {
        fclose(f);
        *out_count = 0;
        return false;
    }

    if (hdr.magic != WIFI_DB_MAGIC || hdr.ver != WIFI_DB_VER || hdr.count > WIFI_DB_MAX_RECORDS)
    {
        fclose(f);
        *out_count = 0;
        return false;
    }

    size_t need = hdr.count * sizeof(wifi_cred_t);

    // Clear output buffer first (optional)
    memset(out_list, 0, WIFI_DB_MAX_RECORDS * sizeof(wifi_cred_t));

    if (need)
    {
        if (fread(out_list, 1, need, f) != need)
        {
            fclose(f);
            *out_count = 0;
            return false;
        }
    }

    fclose(f);

    uint32_t calc = crc32_simple((const uint8_t *)out_list, need);
    if (calc != hdr.crc32)
    {
        ESP_LOGW(TAG, "DB CRC mismatch (file corrupted?)");
        *out_count = 0;
        return false;
    }

    *out_count = hdr.count;
    return true;
}

bool wifi_db_save_all(const wifi_cred_t *list, int count)
{
    if (!list || count < 0 || count > WIFI_DB_MAX_RECORDS)
        return false;

    FILE *f = fopen(WIFI_DB_PATH, "wb");
    if (!f)
        return false;

    wifi_db_hdr_t hdr = {
        .magic = WIFI_DB_MAGIC,
        .ver = WIFI_DB_VER,
        .count = (uint16_t)count,
        .crc32 = 0};

    size_t data_len = count * sizeof(wifi_cred_t);
    hdr.crc32 = crc32_simple((const uint8_t *)list, data_len);

    bool ok = true;
    if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        ok = false;
    if (ok && data_len && fwrite(list, 1, data_len, f) != data_len)
        ok = false;

    fclose(f);
    return ok;
}

bool wifi_db_add_or_update(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || !pass)
        return false;

    wifi_cred_t list[WIFI_DB_MAX_RECORDS] = {0};
    int count = 0;
    wifi_db_load(list, &count);

    // update if exists
    for (int i = 0; i < count; i++)
    {
        if (strncmp(list[i].ssid, ssid, WIFI_SSID_MAX) == 0)
        {
            strlcpy(list[i].pass, pass, sizeof(list[i].pass));
            return wifi_db_save_all(list, count);
        }
    }

    // add
    if (count >= WIFI_DB_MAX_RECORDS)
    {
        // simple policy: overwrite the last record
        count = WIFI_DB_MAX_RECORDS - 1;
    }

    strlcpy(list[count].ssid, ssid, sizeof(list[count].ssid));
    strlcpy(list[count].pass, pass, sizeof(list[count].pass));
    count++;

    return wifi_db_save_all(list, count);
}

bool wifi_db_delete(const char *ssid)
{
    if (!ssid || !ssid[0])
        return false;

    wifi_cred_t list[WIFI_DB_MAX_RECORDS] = {0};
    int count = 0;
    if (!wifi_db_load(list, &count))
        return false;

    int w = 0;
    for (int r = 0; r < count; r++)
    {
        if (strncmp(list[r].ssid, ssid, WIFI_SSID_MAX) != 0)
        {
            if (w != r)
                list[w] = list[r];
            w++;
        }
    }
    return wifi_db_save_all(list, w);
}
