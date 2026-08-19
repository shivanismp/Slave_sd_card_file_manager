/*
 * Browser-based file viewer/manager for ESP-IDF filesystems.
 *
 * The application should mount its filesystem before calling
 * file_manager_init() and set mount_spiffs=false. The component can mount a
 * SPIFFS partition itself for small standalone projects, but this project uses
 * the existing LittleFS mount.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "sd_card_file_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile bool html_file_manager_enabled;

typedef enum {
    FILE_MANAGER_FS_AUTO = 0,
    FILE_MANAGER_FS_SPIFFS,
    FILE_MANAGER_FS_LITTLEFS,
} file_manager_fs_t;

/* Backward-compatible name retained for existing application code. */
typedef sd_card_available_fn_t file_manager_sd_available_fn_t;

typedef struct {
    const char *mount_point;       /* Example: "/littlefs". */
    const char *partition_label;   /* Example: "storage"; NULL = default. */
    uint16_t port;                 /* Standalone HTTP data port. */
    uint16_t control_port;         /* Must differ from other httpd instances. */
    bool mount_spiffs;             /* Legacy option: mount SPIFFS internally. */
    bool format_if_mount_failed;   /* Used only when mount_spiffs is true. */
    bool allow_mutation;           /* Enables upload and delete endpoints. */
    file_manager_fs_t filesystem;  /* Selects the storage-info API. */
    httpd_handle_t server_handle;  /* NULL creates a dedicated server. */
    const char *sd_mount_point;    /* Example: "/sdcard"; NULL disables SD. */
    sd_card_available_fn_t sd_available; /* NULL uses mount-path stat. */
} file_manager_config_t;

#define FILE_MANAGER_DEFAULT_CONFIG()                                      \
    {                                                                       \
        .mount_point = "/spiffs", .partition_label = NULL, .port = 8080,  \
        .control_port = 32769, .mount_spiffs = true,                        \
        .format_if_mount_failed = false, .allow_mutation = false,           \
        .filesystem = FILE_MANAGER_FS_SPIFFS, .server_handle = NULL,        \
        .sd_mount_point = "/sdcard", .sd_available = NULL                  \
    }

/*
 * Starts the UI and API. Call once after the filesystem is mounted. Starting
 * before an interface receives an IP address is supported.
 */
esp_err_t file_manager_init(const file_manager_config_t *config);

/*
 * Stops a dedicated server. If handlers were attached to an existing server,
 * only the file-manager handlers are removed.
 */
void file_manager_stop(void);

bool file_manager_is_running(void);

#ifdef __cplusplus
}
#endif