#include "file_io.h"
#include "global.h"

#include "esp_log.h"
#include "esp_log_tags.h"

static void trim_in_place(char *s);
static bool string_is_valid(const char *str, uint8_t min_len, uint8_t max_len);
static bool pem_text_is_valid(const char *text, const char *must_contain_1, const char *must_contain_2);
static bool load_or_restore_text_file(const char *main_path,
                                      const char *backup_path,
                                      char *out,
                                      size_t out_size,
                                      const char *must_contain_1,
                                      const char *must_contain_2);

static bool file_write_and_verify(const char *path,
                                  const char *data,
                                  char *verify_buf,
                                  size_t verify_buf_size);
static bool load_file_text(const char *path, char *out, size_t out_size);




static void trim_in_place(char *s)
{
    char *start;
    size_t len;

    if (s == NULL)
        return;

    start = s;
    while (*start && isspace((unsigned char)*start))
        start++;

    if (start != s)
        memmove(s, start, strlen(start) + 1U);

    len = strlen(s);
    while (len > 0U && isspace((unsigned char)s[len - 1U]))
    {
        s[len - 1U] = '\0';
        len--;
    }
}

static bool string_is_valid(const char *str, uint8_t min_len, uint8_t max_len)
{
    size_t len;

    if (str == NULL)
        return false;

    len = strlen(str);

    if (len < min_len || len > max_len)
        return false;

    return true;
}

static bool pem_text_is_valid(const char *text, const char *must_contain_1, const char *must_contain_2)
{
    if (text == NULL || text[0] == '\0')
        return false;

    if (must_contain_1 != NULL && strstr(text, must_contain_1) == NULL)
        return false;

    if (must_contain_2 != NULL && strstr(text, must_contain_2) == NULL)
        return false;

    return true;
}

static bool load_or_restore_text_file(const char *main_path,
                                      const char *backup_path,
                                      char *out,
                                      size_t out_size,
                                      const char *must_contain_1,
                                      const char *must_contain_2)
{
    char *buf_main = NULL;
    char *buf_backup = NULL;
    char *verify_buf = NULL;
    bool ok = false;

    if (main_path == NULL || backup_path == NULL || out == NULL || out_size < 2U)
        return false;

    memset(out, 0, out_size);

    buf_main   = calloc(1, out_size);
    buf_backup = calloc(1, out_size);
    verify_buf = calloc(1, out_size);

    if (buf_main == NULL || buf_backup == NULL || verify_buf == NULL)
    {
        ESP_LOGE(TAG_FILE_IO, "No memory for text file restore buffers");
        goto cleanup;
    }

    if (load_file_text(main_path, buf_main, out_size))
    {
        if (pem_text_is_valid(buf_main, must_contain_1, must_contain_2))
        {
            strncpy(out, buf_main, out_size - 1U);
            out[out_size - 1U] = '\0';
            ok = true;
            goto cleanup;
        }

        ESP_LOGW(TAG_FILE_IO, "Main file invalid: %s", main_path);
    }
    else
    {
        ESP_LOGW(TAG_FILE_IO, "Main file not readable: %s", main_path);
    }

    if (load_file_text(backup_path, buf_backup, out_size))
    {
        if (pem_text_is_valid(buf_backup, must_contain_1, must_contain_2))
        {
            if (!file_write_and_verify(main_path, buf_backup, verify_buf, out_size))
            {
                ESP_LOGE(TAG_FILE_IO, "Backup valid but failed to restore main: %s", main_path);
                goto cleanup;
            }

            strncpy(out, buf_backup, out_size - 1U);
            out[out_size - 1U] = '\0';
            ok = true;
            goto cleanup;
        }

        ESP_LOGW(TAG_FILE_IO, "Backup file invalid: %s", backup_path);
    }
    else
    {
        ESP_LOGW(TAG_FILE_IO, "Backup file not readable: %s", backup_path);
    }

cleanup:
    free(buf_main);
    free(buf_backup);
    free(verify_buf);
    return ok;
}

static bool file_write_and_verify(const char *path,
                                  const char *data,
                                  char *verify_buf,
                                  size_t verify_buf_size)
{
    if (path == NULL || data == NULL || verify_buf == NULL || verify_buf_size < 2U)
        return false;

    memset(verify_buf, 0, verify_buf_size);

    if (!file_write_string(path, data))
        return false;

    if (!file_read_string(path, verify_buf, verify_buf_size))
        return false;

    if (strcmp(data, verify_buf) != 0)
        return false;

    return true;
}

static bool load_file_text(const char *path, char *out, size_t out_size)
{
    FILE *f;
    size_t nread;

    if (path == NULL || out == NULL || out_size < 2U)
        return false;

    memset(out, 0, out_size);

    f = fopen(path, "r");
    if (f == NULL)
    {
        ESP_LOGE(TAG_FILE_IO, "Failed to open: %s", path);
        return false;
    }

    nread = fread(out, 1, out_size - 1U, f);
    fclose(f);

    out[nread] = '\0';

    if (nread == 0U)
    {
        ESP_LOGE(TAG_FILE_IO, "File empty or unreadable: %s", path);
        return false;
    }

    return true;
}

bool load_or_restore_mqtt_serial_no(char *out, size_t out_size)
{
    char buf_main[MQTT_SERIAL_MAX_LEN];
    char buf_backup[MQTT_SERIAL_MAX_LEN];
    char verify_buf[MQTT_SERIAL_MAX_LEN];
    uint8_t i;

    if (out == NULL || out_size < (MQTT_SERIAL_GEN_LEN + 1U))
    {
        ESP_LOGE(TAG_FILE_IO, "Invalid output buffer for MQTT serial");
        return false;
    }

    memset(out, 0, out_size);
    memset(buf_main, 0, sizeof(buf_main));
    memset(buf_backup, 0, sizeof(buf_backup));
    memset(verify_buf, 0, sizeof(verify_buf));

    if (file_read_string(MQTT_SERIAL_FILE_PATH, buf_main, sizeof(buf_main)))
    {
        if (strlen(buf_main) == MQTT_SERIAL_GEN_LEN)
        {
            bool valid = true;

            for (i = 0; i < MQTT_SERIAL_GEN_LEN; i++)
            {
                if (!isalnum((unsigned char)buf_main[i]))
                {
                    valid = false;
                    break;
                }
            }

            if (valid)
            {
                strncpy(out, buf_main, out_size - 1U);
                out[out_size - 1U] = '\0';

                ESP_LOGI(TAG_FILE_IO, "Loaded MQTT serial from main");
                return true;
            }
        }

        ESP_LOGW(TAG_FILE_IO, "Main MQTT serial invalid");
    }
    else
    {
        ESP_LOGW(TAG_FILE_IO, "Main MQTT serial not readable: %s", MQTT_SERIAL_FILE_PATH);
    }

    if (file_read_string(MQTT_SERIAL_FILE_BACKUP_PATH, buf_backup, sizeof(buf_backup)))
    {
        if (strlen(buf_backup) == MQTT_SERIAL_GEN_LEN)
        {
            bool valid = true;

            for (i = 0; i < MQTT_SERIAL_GEN_LEN; i++)
            {
                if (!isalnum((unsigned char)buf_backup[i]))
                {
                    valid = false;
                    break;
                }
            }

            if (valid)
            {
                if (!file_write_and_verify(MQTT_SERIAL_FILE_PATH,
                                           buf_backup,
                                           verify_buf,
                                           sizeof(verify_buf)))
                {
                    ESP_LOGE(TAG_FILE_IO, "Backup serial valid but failed to restore main");
                    return false;
                }

                strncpy(out, buf_backup, out_size - 1U);
                out[out_size - 1U] = '\0';

                ESP_LOGW(TAG_FILE_IO, "Restored main MQTT serial from backup");
                return true;
            }
        }

        ESP_LOGW(TAG_FILE_IO, "Backup MQTT serial invalid");
    }
    else
    {
        ESP_LOGW(TAG_FILE_IO, "Backup MQTT serial not readable: %s", MQTT_SERIAL_FILE_BACKUP_PATH);
    }

    ESP_LOGE(TAG_FILE_IO, "No valid MQTT serial found in main or backup");
    return false;
}

bool load_or_restore_mqtt_aws_endpoint(char *out, size_t out_size)
{
    char buf_main[MQTT_AWS_ENDPOINT_MAX_LEN];
    char buf_backup[MQTT_AWS_ENDPOINT_MAX_LEN];
    char verify_buf[MQTT_AWS_ENDPOINT_MAX_LEN];

    if (out == NULL || out_size < 2U)
    {
        ESP_LOGE(TAG_FILE_IO, "Invalid output buffer for AWS endpoint");
        return false;
    }

    memset(out, 0, out_size);
    memset(buf_main, 0, sizeof(buf_main));
    memset(buf_backup, 0, sizeof(buf_backup));
    memset(verify_buf, 0, sizeof(verify_buf));

    if (file_read_string(MQTT_AWS_ENDPOINT_FILE_PATH, buf_main, sizeof(buf_main)))
    {
        if (string_is_valid(buf_main,
                            MQTT_AWS_ENDPOINT_MIN_LEN,
                            MQTT_AWS_ENDPOINT_MAX_LEN - 1U))
        {
            strncpy(out, buf_main, out_size - 1U);
            out[out_size - 1U] = '\0';

            ESP_LOGI(TAG_FILE_IO, "Loaded AWS endpoint from main");
            return true;
        }

        ESP_LOGW(TAG_FILE_IO, "Main AWS endpoint invalid");
    }
    else
    {
        ESP_LOGW(TAG_FILE_IO, "Main AWS endpoint not readable: %s", MQTT_AWS_ENDPOINT_FILE_PATH);
    }

    if (file_read_string(MQTT_AWS_ENDPOINT_FILE_BACKUP_PATH, buf_backup, sizeof(buf_backup)))
    {
        if (string_is_valid(buf_backup,
                            MQTT_AWS_ENDPOINT_MIN_LEN,
                            MQTT_AWS_ENDPOINT_MAX_LEN - 1U))
        {
            if (!file_write_and_verify(MQTT_AWS_ENDPOINT_FILE_PATH,
                                       buf_backup,
                                       verify_buf,
                                       sizeof(verify_buf)))
            {
                ESP_LOGE(TAG_FILE_IO, "Backup endpoint valid but failed to restore main");
                return false;
            }

            strncpy(out, buf_backup, out_size - 1U);
            out[out_size - 1U] = '\0';

            ESP_LOGW(TAG_FILE_IO, "Restored main AWS endpoint from backup");
            return true;
        }

        ESP_LOGW(TAG_FILE_IO, "Backup AWS endpoint invalid");
    }
    else
    {
        ESP_LOGW(TAG_FILE_IO, "Backup AWS endpoint not readable: %s", MQTT_AWS_ENDPOINT_FILE_BACKUP_PATH);
    }

    ESP_LOGE(TAG_FILE_IO, "No valid AWS endpoint found in main or backup");
    return false;
}

bool mqtt_load_certs_from_littlefs(char *root_ca,
                                   size_t root_ca_size,
                                   char *device_cert,
                                   size_t device_cert_size,
                                   char *private_key,
                                   size_t private_key_size)
{
    if (root_ca == NULL || root_ca_size < 2U ||
        device_cert == NULL || device_cert_size < 2U ||
        private_key == NULL || private_key_size < 2U)
    {
        ESP_LOGE(TAG_FILE_IO, "Invalid certificate buffers");
        return false;
    }

    if (!load_or_restore_text_file(MQTT_ROOT_CA_FILE_PATH,
                                   MQTT_ROOT_CA_FILE_BACKUP_PATH,
                                   root_ca,
                                   root_ca_size,
                                   "BEGIN CERTIFICATE",
                                   "END CERTIFICATE"))
    {
        ESP_LOGE(TAG_FILE_IO, "Failed to load/restore root CA");
        return false;
    }

    if (!load_or_restore_text_file(MQTT_DEVICE_CERT_FILE_PATH,
                                   MQTT_DEVICE_CERT_FILE_BACKUP_PATH,
                                   device_cert,
                                   device_cert_size,
                                   "BEGIN CERTIFICATE",
                                   "END CERTIFICATE"))
    {
        ESP_LOGE(TAG_FILE_IO, "Failed to load/restore device certificate");
        return false;
    }

    if (!load_or_restore_text_file(MQTT_PRIVATE_KEY_FILE_PATH,
                                   MQTT_PRIVATE_KEY_FILE_BACKUP_PATH,
                                   private_key,
                                   private_key_size,
                                   "BEGIN",
                                   "PRIVATE KEY"))
    {
        ESP_LOGE(TAG_FILE_IO, "Failed to load/restore private key");
        return false;
    }

    ESP_LOGI(TAG_FILE_IO, "MQTT certificates loaded/restored successfully");
    return true;
}




static bool mqtt_topics_are_valid(const mqtt_topic_list_t *list)
{
    int i;

    if (list == NULL)
        return false;

    if (list->count < 2 || list->count > MQTT_MAX_TOPICS)
        return false;

    for (i = 0; i < list->count; i++)
    {
        size_t len = strlen(list->topics[i]);

        if (len == 0U || len >= MQTT_MAX_TOPIC_LEN)
            return false;
    }

    return true;
}

static bool load_mqtt_topics_from_file(const char *path,
                                       mqtt_topic_list_t *out_topics)
{
    FILE *f;
    char line[MQTT_MAX_TOPIC_LEN];
    int count = 0;

    if (path == NULL || out_topics == NULL)
        return false;

    memset(out_topics, 0, sizeof(*out_topics));

    ESP_LOGI(TAG_FILE_IO, "Opening topics file: %s", path);

    f = fopen(path, "r");
    if (f == NULL)
    {
        ESP_LOGE(TAG_FILE_IO, "Failed to open topics file: %s", path);
        return false;
    }

    while (fgets(line, sizeof(line), f) != NULL)
    {
        trim_in_place(line);

        if (line[0] == '\0' || line[0] == '#')
            continue;

        if (count >= MQTT_MAX_TOPICS)
        {
            ESP_LOGE(TAG_FILE_IO, "Too many topics in file: %s", path);
            fclose(f);
            return false;
        }

        strncpy(out_topics->topics[count], line, MQTT_MAX_TOPIC_LEN - 1U);
        out_topics->topics[count][MQTT_MAX_TOPIC_LEN - 1U] = '\0';
        count++;
    }

    fclose(f);

    out_topics->count = count;

    if (!mqtt_topics_are_valid(out_topics))
    {
        ESP_LOGE(TAG_FILE_IO, "Invalid topics content in file: %s", path);
        return false;
    }

    return true;
}


bool load_or_restore_mqtt_topics(mqtt_topic_list_t *out_topics,
                                 char *mbm_req,
                                 size_t mbm_req_size,
                                 char *mbm_res,
                                 size_t mbm_res_size)
{
    char *backup_text = NULL;
    char *verify_buf = NULL;
    size_t text_buf_size = MQTT_MAX_TOPICS * MQTT_MAX_TOPIC_LEN;

    if (out_topics == NULL ||
        mbm_req == NULL || mbm_req_size < 2U ||
        mbm_res == NULL || mbm_res_size < 2U)
    {
        ESP_LOGE(TAG_FILE_IO, "Invalid output buffer for MQTT topics");
        return false;
    }

    memset(out_topics, 0, sizeof(*out_topics));
    memset(mbm_req, 0, mbm_req_size);
    memset(mbm_res, 0, mbm_res_size);

    /* 1) Try main file directly, line by line */
    if (load_mqtt_topics_from_file(MQTT_TOPICS_FILE_PATH, out_topics))
    {
        strncpy(mbm_req, out_topics->topics[0], mbm_req_size - 1U);
        mbm_req[mbm_req_size - 1U] = '\0';

        strncpy(mbm_res, out_topics->topics[1], mbm_res_size - 1U);
        mbm_res[mbm_res_size - 1U] = '\0';

        ESP_LOGI(TAG_FILE_IO, "Loaded MQTT topics from main");
        return true;
    }

    ESP_LOGW(TAG_FILE_IO, "Main MQTT topics file invalid or unreadable");

    /* 2) Try backup file directly, line by line */
    if (!load_mqtt_topics_from_file(MQTT_TOPICS_FILE_BACKUP_PATH, out_topics))
    {
        ESP_LOGW(TAG_FILE_IO, "Backup MQTT topics file invalid or unreadable");
        ESP_LOGE(TAG_FILE_IO, "No valid MQTT topics found in main or backup");
        return false;
    }

    /* 3) Populate outputs from valid backup */
    strncpy(mbm_req, out_topics->topics[0], mbm_req_size - 1U);
    mbm_req[mbm_req_size - 1U] = '\0';

    strncpy(mbm_res, out_topics->topics[1], mbm_res_size - 1U);
    mbm_res[mbm_res_size - 1U] = '\0';

    /* 4) Restore main from backup text */
    backup_text = calloc(1, text_buf_size);
    verify_buf  = calloc(1, text_buf_size);

    if (backup_text != NULL && verify_buf != NULL)
    {
        if (load_file_text(MQTT_TOPICS_FILE_BACKUP_PATH, backup_text, text_buf_size))
        {
            if (file_write_and_verify(MQTT_TOPICS_FILE_PATH,
                                      backup_text,
                                      verify_buf,
                                      text_buf_size))
            {
                ESP_LOGW(TAG_FILE_IO, "Restored main MQTT topics from backup");
            }
            else
            {
                ESP_LOGW(TAG_FILE_IO, "Backup topics loaded, but failed to restore main");
            }
        }
    }

    free(backup_text);
    free(verify_buf);

    return true;
}
