#include "sd_card_file_manager.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_vfs_fat.h"

esp_err_t sd_card_file_manager_init(
    sd_card_file_manager_t *manager,
    const sd_card_file_manager_config_t *config)
{
    if (!manager || !config || !config->mount_point ||
        config->mount_point[0] != '/' ||
        strcmp(config->mount_point, "/") == 0 ||
        strlen(config->mount_point) >= sizeof(manager->mount_point)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(manager, 0, sizeof(*manager));
    snprintf(manager->mount_point, sizeof(manager->mount_point), "%s",
             config->mount_point);

    size_t length = strlen(manager->mount_point);
    while (length > 1 && manager->mount_point[length - 1] == '/') {
        manager->mount_point[--length] = '\0';
    }

    manager->available = config->available;
    manager->initialized = true;
    return ESP_OK;
}

void sd_card_file_manager_deinit(sd_card_file_manager_t *manager)
{
    if (manager) {
        memset(manager, 0, sizeof(*manager));
    }
}

bool sd_card_file_manager_is_available(
    const sd_card_file_manager_t *manager)
{
    if (!manager || !manager->initialized) {
        return false;
    }
    if (manager->available && !manager->available()) {
        return false;
    }

    struct stat st;
    return stat(manager->mount_point, &st) == 0 && S_ISDIR(st.st_mode);
}

const char *sd_card_file_manager_mount_point(
    const sd_card_file_manager_t *manager)
{
    return manager && manager->initialized ? manager->mount_point : NULL;
}

esp_err_t sd_card_file_manager_resolve_path(
    const sd_card_file_manager_t *manager,
    const char *relative,
    char *full_path,
    size_t full_path_size)
{
    if (!manager || !manager->initialized || !relative || !full_path ||
        full_path_size == 0 || relative[0] == '\0' || relative[0] == '/') {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(full_path, full_path_size, "%s/%s",
                           manager->mount_point, relative);
    if (written <= 0 || (size_t)written >= full_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t sd_card_file_manager_get_space(
    const sd_card_file_manager_t *manager,
    uint64_t *total_bytes,
    uint64_t *used_bytes,
    uint64_t *free_bytes)
{
    if (!manager || !manager->initialized || !total_bytes || !used_bytes ||
        !free_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_card_file_manager_is_available(manager)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0;
    uint64_t free_space = 0;
    esp_err_t err = esp_vfs_fat_info(manager->mount_point, &total, &free_space);
    if (err != ESP_OK) {
        return err;
    }

    *total_bytes = total;
    *free_bytes = free_space <= total ? free_space : total;
    *used_bytes = total - *free_bytes;
    return ESP_OK;
}