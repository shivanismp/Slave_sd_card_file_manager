#ifndef __SDCARD_H__
#define __SDCARD_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SDCARD_BASE_PATH
#define SDCARD_BASE_PATH          "/sdcard"
#endif

#ifndef SDCARD_SPI_HOST
#define SDCARD_SPI_HOST           SPI2_HOST
#endif

#ifndef SDCARD_PIN_CS
#define SDCARD_PIN_CS             GPIO_NUM_22
#endif

#ifndef SDCARD_PIN_MOSI
#define SDCARD_PIN_MOSI           GPIO_NUM_23
#endif

#ifndef SDCARD_PIN_MISO
#define SDCARD_PIN_MISO           GPIO_NUM_18
#endif

#ifndef SDCARD_PIN_SCK
#define SDCARD_PIN_SCK            GPIO_NUM_19
#endif

typedef struct
{
    const char       *base_path;              // e.g. "/sdcard"
    spi_host_device_t spi_host;               // e.g. SPI2_HOST
    gpio_num_t        pin_cs;
    gpio_num_t        pin_mosi;
    gpio_num_t        pin_miso;
    gpio_num_t        pin_sck;
    bool              format_if_mount_failed; // keep false in production
    size_t            max_files;              // VFS max open files
    size_t            allocation_unit_size;   // e.g. 16*1024
    int               max_transfer_sz;        // e.g. 4096 or 16*1024
} sdcard_cfg_t;

esp_err_t sdcard_init(void);

bool sdcard_is_mounted(void);
bool sdcard_exists(const char *path);
esp_err_t sdcard_format(void);

esp_err_t sdcard_mkdirs(const char *dir_path);
esp_err_t sdcard_delete_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif
