#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "littlefs.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_log_tags.h"

static bool s_mounted = false;

static esp_err_t littlefs_init_ex(const littlefs_cfg_t *cfg);

static esp_err_t mkdir_if_needed_one(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0') return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(dir_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return ESP_OK;
        return ESP_FAIL; // exists but not a directory
    }

    if (mkdir(dir_path, 0755) == 0) return ESP_OK;

    if (errno == EEXIST) return ESP_OK;
    return ESP_FAIL;
}

esp_err_t littlefs_mkdirs(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0') return ESP_ERR_INVALID_ARG;

    char tmp[256];
    size_t n = strnlen(dir_path, sizeof(tmp));
    if (n == sizeof(tmp)) return ESP_ERR_INVALID_SIZE;

    memcpy(tmp, dir_path, n);
    tmp[n] = '\0';

    if (n > 1 && tmp[n - 1] == '/') tmp[n - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            esp_err_t err = mkdir_if_needed_one(tmp);
            if (err != ESP_OK) {
                ESP_LOGE(TAG_LITTLEFS, "mkdirs failed at: %s (errno=%d)", tmp, errno);
                return err;
            }
            *p = '/';
        }
    }

    esp_err_t err = mkdir_if_needed_one(tmp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_LITTLEFS, "mkdirs failed at: %s (errno=%d)", tmp, errno);
    }
    return err;
}

static esp_err_t ensure_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        return ESP_OK;
    }

    size_t len = (size_t)(slash - path);
    if (len >= 256) return ESP_ERR_INVALID_SIZE;

    char parent[256];
    memcpy(parent, path, len);
    parent[len] = '\0';

    return littlefs_mkdirs(parent);
}

esp_err_t littlefs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    littlefs_cfg_t cfg = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = LITTLEFS_PART_LABEL,
        .format_if_mount_failed = true,
    };
    return littlefs_init_ex(&cfg);
}

static esp_err_t littlefs_init_ex(const littlefs_cfg_t *cfg)
{
    if (s_mounted) return ESP_OK;
    if (!cfg || !cfg->base_path || !cfg->partition_label) return ESP_ERR_INVALID_ARG;

    esp_vfs_littlefs_conf_t conf = {
        .base_path = cfg->base_path,
        .partition_label = cfg->partition_label,
        .format_if_mount_failed = cfg->format_if_mount_failed,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_LITTLEFS, "esp_vfs_littlefs_register failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    err = esp_littlefs_info(cfg->partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG_LITTLEFS, "Mounted LittleFS: label=%s base=%s total=%u used=%u",
                 cfg->partition_label, cfg->base_path,
                 (unsigned)total, (unsigned)used);
    } else {
        ESP_LOGW(TAG_LITTLEFS, "esp_littlefs_info failed: %s", esp_err_to_name(err));
    }

    s_mounted = true;
    return ESP_OK;
}

bool littlefs_exists(const char *path)
{
    struct stat st;
    return (path && (stat(path, &st) == 0));
}

esp_err_t littlefs_write_file(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ensure_parent_dir(path);
    if (err != ESP_OK) return err;

    char tmp_path[256];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG_LITTLEFS, "fopen write failed: %s (errno=%d)", tmp_path, errno);
        return ESP_FAIL;
    }

    if (len) {
        size_t wr = fwrite(data, 1, len, f);
        if (wr != len) {
            ESP_LOGE(TAG_LITTLEFS, "fwrite short: got=%u expected=%u path=%s",
                     (unsigned)wr, (unsigned)len, tmp_path);
            fclose(f);
            unlink(tmp_path);
            return ESP_FAIL;
        }
    }

    if (fflush(f) != 0) {
        ESP_LOGW(TAG_LITTLEFS, "fflush failed: %s (errno=%d)", tmp_path, errno);
    }
    fclose(f);

    (void)unlink(path);
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG_LITTLEFS, "rename failed: %s -> %s (errno=%d)", tmp_path, path, errno);
        unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t littlefs_append_file(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_mounted) {
        ESP_LOGE(TAG_LITTLEFS, "append failed; LittleFS is not mounted: %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    /* Create any requested subdirectory before opening the file. */
    esp_err_t err = ensure_parent_dir(path);
    if (err != ESP_OK) {
        return err;
    }

    /* "ab" creates the file when it does not exist and appends when it does. */
    errno = 0;
    FILE *f = fopen(path, "ab");
    if (!f) {
        ESP_LOGE(TAG_LITTLEFS,
                 "fopen append failed: %s (errno=%d: %s)",
                 path, errno, strerror(errno));
        return ESP_FAIL;
    }

    if (len != 0U) {
        size_t written = fwrite(data, 1, len, f);
        if (written != len) {
            int saved_errno = errno;
            ESP_LOGE(TAG_LITTLEFS,
                     "fwrite append short: got=%u expected=%u path=%s "
                     "(errno=%d: %s)",
                     (unsigned)written, (unsigned)len, path,
                     saved_errno, strerror(saved_errno));
            fclose(f);
            return ESP_FAIL;
        }
    }

    /* fclose() flushes the record and releases the VFS file descriptor. */
    if (fclose(f) != 0) {
        ESP_LOGE(TAG_LITTLEFS,
                 "fclose append failed: %s (errno=%d: %s)",
                 path, errno, strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t littlefs_delete_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!littlefs_exists(path)) return ESP_OK;

    if (unlink(path) == 0) return ESP_OK;

    ESP_LOGE(TAG_LITTLEFS, "unlink failed: %s (errno=%d)", path, errno);
    return ESP_FAIL;
}

