#ifndef __LITTLEFS_H__
#define __LITTLEFS_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LITTLEFS_BASE_PATH
#define LITTLEFS_BASE_PATH       "/littlefs"
#endif

#ifndef LITTLEFS_PART_LABEL
#define LITTLEFS_PART_LABEL      "storage"
#endif

typedef struct {
    const char *base_path;          // e.g. "/littlefs"
    const char *partition_label;    // e.g. "storage"
    bool format_if_mount_failed;    // recommended true for dev, false for production
} littlefs_cfg_t;

esp_err_t littlefs_append_file(
    const char *path,
    const void *data,
    size_t len);

/**
 * @brief Mount LittleFS using defaults: base_path=/littlefs, label=storage
 */
esp_err_t littlefs_init(void);

/**
 * @brief Check if a file exists
 */
bool littlefs_exists(const char *path);

/**
 * @brief Create directories recursively, like mkdir -p
 */
esp_err_t littlefs_mkdirs(const char *dir_path);

/**
 * @brief Write entire file (atomic): write to temp then rename.
 * Creates parent directories if needed.
 *
 * @param path Full path
 * @param data Data bytes
 * @param len Length
 */
esp_err_t littlefs_write_file(const char *path, const void *data, size_t len);

/**
 * @brief Delete a file (ESP_OK if not found)
 */
esp_err_t littlefs_delete_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif
