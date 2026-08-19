/*
 * SD-card storage backend used by the browser file manager.
 *
 * This module does not select SDMMC/SDSPI pins or mount the card. The
 * application remains responsible for mounting the card (normally at
 * /sdcard). Once mounted, this class detects it and exposes its root path and
 * capacity to file_manager.c.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CARD_FILE_MANAGER_MOUNT_POINT_MAX 64

/* Optional card-detect/mount-state callback supplied by the application. */
typedef bool (*sd_card_available_fn_t)(void);

typedef struct {
    const char *mount_point;                 /* Example: "/sdcard". */
    sd_card_available_fn_t available;        /* NULL checks the mount path. */
} sd_card_file_manager_config_t;

typedef struct {
    char mount_point[SD_CARD_FILE_MANAGER_MOUNT_POINT_MAX];
    sd_card_available_fn_t available;
    bool initialized;
} sd_card_file_manager_t;

#define SD_CARD_FILE_MANAGER_DEFAULT_CONFIG() \
    { .mount_point = "/sdcard", .available = NULL }

/* Configures a manager. The card may be absent when this is called. */
esp_err_t sd_card_file_manager_init(
    sd_card_file_manager_t *manager,
    const sd_card_file_manager_config_t *config);

void sd_card_file_manager_deinit(sd_card_file_manager_t *manager);

/* True only when the configured root currently exists as a directory. */
bool sd_card_file_manager_is_available(
    const sd_card_file_manager_t *manager);

/* Returns NULL when the manager has not been initialized. */
const char *sd_card_file_manager_mount_point(
    const sd_card_file_manager_t *manager);

/* Builds <mount point>/<relative path>; relative must already be validated. */
esp_err_t sd_card_file_manager_resolve_path(
    const sd_card_file_manager_t *manager,
    const char *relative,
    char *full_path,
    size_t full_path_size);

/* Returns FAT capacity when supported by the ESP-IDF version/filesystem. */
esp_err_t sd_card_file_manager_get_space(
    const sd_card_file_manager_t *manager,
    uint64_t *total_bytes,
    uint64_t *used_bytes,
    uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif