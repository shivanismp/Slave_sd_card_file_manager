#include "sdcard.h"
#include "sdmmc_cmd.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"

static const char *TAG_SDCARD = "SDCARD";

static bool                 s_mounted            = false;
static bool                 s_bus_owned_by_me    = false;
static sdmmc_card_t         *s_card              = NULL;

static esp_err_t sdcard_init_ex(const sdcard_cfg_t *cfg);

static esp_err_t mkdir_if_needed_one(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(dir_path, &st) == 0)
    {
        if (S_ISDIR(st.st_mode))
            return ESP_OK;
        return ESP_FAIL;
    }

    if (mkdir(dir_path, 0755) == 0)
        return ESP_OK;

    if (errno == EEXIST)
        return ESP_OK;

    return ESP_FAIL;
}

esp_err_t sdcard_mkdirs(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    char tmp[256];
    size_t n = strnlen(dir_path, sizeof(tmp));
    if (n == sizeof(tmp))
        return ESP_ERR_INVALID_SIZE;

    memcpy(tmp, dir_path, n);
    tmp[n] = '\0';

    if (n > 1 && tmp[n - 1] == '/')
        tmp[n - 1] = '\0';

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            esp_err_t err = mkdir_if_needed_one(tmp);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG_SDCARD, "mkdirs failed at: %s (errno=%d)", tmp, errno);
                return err;
            }
            *p = '/';
        }
    }

    esp_err_t err = mkdir_if_needed_one(tmp);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_SDCARD, "mkdirs failed at: %s (errno=%d)", tmp, errno);
    }
    return err;
}

esp_err_t sdcard_init(void)
{
    const sdcard_cfg_t cfg =
    {
        .base_path              = SDCARD_BASE_PATH,
        .spi_host               = SDCARD_SPI_HOST,
        .pin_cs                 = SDCARD_PIN_CS,
        .pin_mosi               = SDCARD_PIN_MOSI,
        .pin_miso               = SDCARD_PIN_MISO,
        .pin_sck                = SDCARD_PIN_SCK,
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
        .max_transfer_sz        = 4096,
    };

    return sdcard_init_ex(&cfg);
}

static esp_err_t sdcard_init_ex(const sdcard_cfg_t *cfg)
{
    if (s_mounted)
        return ESP_OK;

    if (!cfg || !cfg->base_path)
        return ESP_ERR_INVALID_ARG;

    spi_bus_config_t bus_cfg =
    {
        .mosi_io_num     = cfg->pin_mosi,
        .miso_io_num     = cfg->pin_miso,
        .sclk_io_num     = cfg->pin_sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = cfg->max_transfer_sz,
    };

    esp_err_t err = spi_bus_initialize(cfg->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK)
    {
        s_bus_owned_by_me = true;
    }
    else if (err == ESP_ERR_INVALID_STATE)
    {
        s_bus_owned_by_me = false;
        ESP_LOGW(TAG_SDCARD, "SPI bus already initialized, continuing");
    }
    else
    {
        ESP_LOGE(TAG_SDCARD, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = cfg->spi_host;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = cfg->spi_host;
    slot_config.gpio_cs = cfg->pin_cs;
    slot_config.gpio_cd = SDSPI_SLOT_NO_CD;
    slot_config.gpio_wp = SDSPI_SLOT_NO_WP;
#ifdef SDSPI_SLOT_NO_INT
    slot_config.gpio_int = SDSPI_SLOT_NO_INT;
#endif

    esp_vfs_fat_mount_config_t mount_config =
    {
        .format_if_mount_failed = cfg->format_if_mount_failed,
        .max_files              = (int)cfg->max_files,
        .allocation_unit_size   = cfg->allocation_unit_size,
    };

    err = esp_vfs_fat_sdspi_mount(cfg->base_path,
                                  &host,
                                  &slot_config,
                                  &mount_config,
                                  &s_card);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_SDCARD, "esp_vfs_fat_sdspi_mount failed: %s", esp_err_to_name(err));

        if (s_bus_owned_by_me)
        {
            spi_bus_free(cfg->spi_host);
            s_bus_owned_by_me = false;
        }

        s_card = NULL;
        return err;
    }

    s_mounted = true;

    ESP_LOGI(TAG_SDCARD,
             "SD card mounted: base=%s CS=%d MOSI=%d MISO=%d SCK=%d",
             cfg->base_path,
             (int)cfg->pin_cs,
             (int)cfg->pin_mosi,
             (int)cfg->pin_miso,
             (int)cfg->pin_sck);

    sdmmc_card_print_info(stdout, s_card);

    return ESP_OK;
}

bool sdcard_is_mounted(void)
{
    return s_mounted;
}

bool sdcard_exists(const char *path)
{
    struct stat st;
    return (path && (stat(path, &st) == 0));
}


esp_err_t sdcard_format(void)
{
    if (!s_mounted || s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG_SDCARD, "Formatting SD card. All SD-card files will be erased.");

    return esp_vfs_fat_sdcard_format(SDCARD_BASE_PATH, s_card);
}


esp_err_t sdcard_delete_file(const char *path)
{
    if (!path)
        return ESP_ERR_INVALID_ARG;

    if (!sdcard_exists(path))
        return ESP_OK;

    if (unlink(path) == 0)
        return ESP_OK;

    ESP_LOGE(TAG_SDCARD, "unlink failed: %s (errno=%d)", path, errno);
    return ESP_FAIL;
}
