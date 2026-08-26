#include "global.h"
#include "modbus_master.h"
#include "modbus_slave.h"
#include "mqtt_file_uploader.h"
// #include "profiles.h"

// #include "temp_pid.h"
// #include "thermo_ai_ctrl.h"
// #include "thermo_ai_persist.h"
// #include "adaptive_temp_pid.h"

#include "event_log.h"
#include "date_time.h"

#include "esp_log_tags.h"
#include "littlefs.h"
#include "sdcard.h"

#include "freertos/semphr.h"

#include <errno.h>
#include <stdint.h>
#include <time.h>

#define MODBUS_WRITE_RETRIES 3
#define MODBUS_WRITE_RETRIES_DELAY_MS 10
#define MODBUS_TURNAROUND_MS 1 // >= 3.5 char times at 115200; increase if using lower baud
// #define MODBUS_TURNAROUND_MS  4   // >= 3.5 char times at 115200; increase if using lower baud
#define MODBUS_RESP_TIMEOUT_MS 120

// static const char *TAG_MODBUS_MASTER = "MODBUS 1";
// static const char *TEMP_SIM = "TEMP SIM";
// const char *TAG_TEMP_PID_MBM = "TEMP_PID_MBM";
// const char *TAG_CUT_DECISION = "CUT DECISION";

#define MELTER_PID_DT_FALLBACK_SEC 0.70f
#define MELTER_PID_DT_MIN_SEC 0.10f
#define MELTER_PID_DT_MAX_SEC 2.00f

#define MELTER_TEMP_CUT_MIN_OFFSET_DEGC 3.0f
#define MELTER_TEMP_CUT_MAX_OFFSET_DEGC 12.0f
#define MELTER_TEMP_RESTART_MIN_OFFSET_DEGC 2.0f
#define MELTER_TEMP_RESTART_MAX_OFFSET_DEGC 8.0f

#define MELTER_TEMP_HYSTERESIS 5
#define MELTER_TEMP_AMBIENT 90
#define MELTER_TEMP_HEAT_STEP 1
#define MELTER_TEMP_COOL_STEP 1

// static thermo_ai_ctrl_t g_thermo_ai;
// static thermo_ai_persist_t g_thermo_ai_ps;
// static bool g_thermo_ai_ready = false;
// static bool g_temp_cut_latched = false;

// static float g_temp_filtered = 0.0f;

// static thermo_ai_ctrl_t g_thermo_ai;
// static thermo_ai_persist_v2_t g_thermo_ai_ps;
// static bool g_thermo_ai_ready = false;
static TickType_t g_last_temp_pid_tick = 0;

// static float g_temp_cut_dwell_s = 0.0f;
// static bool  g_prev_fb_cut_on   = false;
// static float g_recover_boost_s  = 0.0f;
// static float g_cut_rearm_lockout_s   = 0.0f;

volatile bool modbus_busy_flag = false;
volatile bool modbus_check_discrete_input_flag = false;

static volatile bool updating_local_machine_state = false;

// static adaptive_temp_pid_t g_adapt_pid;

#define MODBUS_LOCAL_BIN_FILE_PATH \
    LITTLEFS_BASE_PATH "/machine_binary_data.bin"
#define MODBUS_LOCAL_BIN_PENDING_PATH \
    LITTLEFS_BASE_PATH "/machine_binary_pending.bin"
#define MODBUS_LOCAL_BIN_SD_FILE_PATTERN \
    MQTT_FILE_PENDING_DIR "/M%07lu.BIN"
#define MODBUS_LOCAL_BIN_SD_TEMP_PATTERN \
    MQTT_FILE_PENDING_DIR "/M%07lu.TMP"
#define MODBUS_LOCAL_BIN_SD_SENT_PATTERN \
    MQTT_FILE_SENT_DIR "/M%07lu.BIN"
#define MODBUS_LOCAL_BIN_SD_MAX_SEGMENT_INDEX 9999999UL
#define MODBUS_LOCAL_BIN_RECORD_SIZE \
    (MODBUS_LOCAL_BIN_REGISTER_COUNT * sizeof(uint16_t))
#define MODBUS_LOCAL_BIN_INPUT_DATA_OFFSET \
    (MODBUS_LOCAL_BIN_INPUT_START_INDEX * sizeof(uint16_t))
#define MODBUS_LOCAL_BIN_INPUT_DATA_SIZE \
    (MODBUS_LOCAL_BIN_INPUT_REGISTER_COUNT * sizeof(uint16_t))

static uint8_t s_machine_last_record[MODBUS_LOCAL_BIN_RECORD_SIZE];
static bool s_machine_last_record_valid = false;
static int64_t s_machine_write_retry_after_us = 0;
static SemaphoreHandle_t s_machine_bin_mutex = NULL;
static TaskHandle_t s_machine_sd_archive_task_handle = NULL;
static volatile uint32_t s_machine_sd_threshold_kb =
    MODBUS_LOCAL_BIN_SD_THRESHOLD_KB_DEFAULT;
static uint32_t s_machine_sd_next_segment = 1U;
static uint8_t s_machine_sd_copy_buffer[MODBUS_LOCAL_BIN_SD_COPY_BUFFER_SIZE];
static bool s_machine_pending_already_archived = false;
static bool s_machine_sd_missing_logged = false;

esp_err_t modbus_local_bin_set_sd_threshold_kb(uint32_t threshold_kb)
{
    if (threshold_kb < MODBUS_LOCAL_BIN_SD_THRESHOLD_KB_MIN ||
        threshold_kb > MODBUS_LOCAL_BIN_SD_THRESHOLD_KB_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_machine_sd_threshold_kb = threshold_kb;

    ESP_LOGI(TAG_MODBUS_MASTER,
             "Local BIN SD threshold changed to %u KB (%u bytes)",
             (unsigned)threshold_kb,
             (unsigned)(threshold_kb * 1024U));

    if (s_machine_sd_archive_task_handle != NULL)
    {
        xTaskNotifyGive(s_machine_sd_archive_task_handle);
    }

    return ESP_OK;
}

uint32_t modbus_local_bin_get_sd_threshold_kb(void)
{
    return s_machine_sd_threshold_kb;
}

static esp_err_t modbus_local_bin_file_size(const char *path,
                                            size_t *out_size)
{
    if (path == NULL || out_size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_size = 0U;
    errno = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return ESP_FAIL;
    }

    long file_size = ftell(file);
    fclose(file);

    if (file_size < 0)
    {
        return ESP_FAIL;
    }

    *out_size = (size_t)file_size;
    return ESP_OK;
}

static esp_err_t modbus_local_bin_append_locked(const uint8_t *record,
                                                size_t record_size,
                                                size_t *out_active_size)
{
    if (record == NULL || record_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_machine_bin_mutex != NULL &&
        xSemaphoreTake(s_machine_bin_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = littlefs_append_file(MODBUS_LOCAL_BIN_FILE_PATH,
                                         record,
                                         record_size);

    if (out_active_size != NULL)
    {
        *out_active_size = 0U;
        if (err == ESP_OK)
        {
            (void)modbus_local_bin_file_size(MODBUS_LOCAL_BIN_FILE_PATH,
                                             out_active_size);
        }
    }

    if (s_machine_bin_mutex != NULL)
    {
        xSemaphoreGive(s_machine_bin_mutex);
    }

    return err;
}

/* Caller must hold s_machine_bin_mutex when the writer task is running. */
static esp_err_t modbus_local_bin_create_empty_active_locked(void)
{
    return littlefs_write_file(MODBUS_LOCAL_BIN_FILE_PATH, NULL, 0U);
}

static esp_err_t modbus_local_bin_ensure_active_file(void)
{
    esp_err_t err = ESP_OK;

    if (s_machine_bin_mutex != NULL &&
        xSemaphoreTake(s_machine_bin_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (!littlefs_exists(MODBUS_LOCAL_BIN_FILE_PATH))
    {
        err = modbus_local_bin_create_empty_active_locked();
    }

    if (s_machine_bin_mutex != NULL)
    {
        xSemaphoreGive(s_machine_bin_mutex);
    }

    return err;
}

/*
 * Atomically move the full active file out of the writer's path. New Modbus
 * records can then create a fresh active file while the pending file is copied.
 */
static bool modbus_local_bin_rotate_if_threshold_reached(void)
{
    if (littlefs_exists(MODBUS_LOCAL_BIN_PENDING_PATH))
    {
        return true;
    }

    uint64_t threshold_bytes =
        (uint64_t)s_machine_sd_threshold_kb * 1024ULL;
    size_t active_size = 0U;
    bool rotated = false;

    if (s_machine_bin_mutex != NULL &&
        xSemaphoreTake(s_machine_bin_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return false;
    }

    if (littlefs_exists(MODBUS_LOCAL_BIN_PENDING_PATH))
    {
        rotated = true;
    }
    else if (modbus_local_bin_file_size(MODBUS_LOCAL_BIN_FILE_PATH,
                                        &active_size) == ESP_OK &&
             (uint64_t)active_size >= threshold_bytes)
    {
        if (rename(MODBUS_LOCAL_BIN_FILE_PATH,
                   MODBUS_LOCAL_BIN_PENDING_PATH) == 0)
        {
            rotated = true;

            /*
             * The threshold data is now protected in the pending file.
             * Recreate the normal writer file at exactly 0 bytes immediately.
             * New register changes can safely accumulate here during the SD
             * copy and will never be deleted with the pending segment.
             */
            esp_err_t reset_err =
                modbus_local_bin_create_empty_active_locked();
            if (reset_err != ESP_OK)
            {
                if (rename(MODBUS_LOCAL_BIN_PENDING_PATH,
                           MODBUS_LOCAL_BIN_FILE_PATH) == 0)
                {
                    rotated = false;
                    ESP_LOGE(TAG_MODBUS_MASTER,
                             "Could not create the empty active BIN (%s); "
                             "rollover was safely postponed",
                             esp_err_to_name(reset_err));
                }
                else
                {
                    ESP_LOGE(TAG_MODBUS_MASTER,
                             "Threshold data remains safe in %s, but the "
                             "empty active BIN could not be created: %s",
                             MODBUS_LOCAL_BIN_PENDING_PATH,
                             esp_err_to_name(reset_err));
                }
            }
        }
        else
        {
            ESP_LOGE(TAG_MODBUS_MASTER,
                     "Cannot rotate local BIN file: %s",
                     strerror(errno));
        }
    }

    if (s_machine_bin_mutex != NULL)
    {
        xSemaphoreGive(s_machine_bin_mutex);
    }

    if (rotated && active_size != 0U)
    {
        ESP_LOGI(TAG_MODBUS_MASTER,
                 "Local BIN reached %u KB; %u bytes queued for SD and "
                 "active BIN reset to 0 bytes",
                 (unsigned)s_machine_sd_threshold_kb,
                 (unsigned)active_size);
    }

    return rotated;
}

static bool modbus_local_bin_next_sd_paths(char *final_path,
                                           size_t final_path_size,
                                           char *temp_path,
                                           size_t temp_path_size)
{
    if (final_path == NULL || temp_path == NULL)
    {
        return false;
    }

    for (uint32_t checked = 0U;
         checked < MODBUS_LOCAL_BIN_SD_MAX_SEGMENT_INDEX;
         checked++)
    {
        char sent_path[64];
        uint32_t index = s_machine_sd_next_segment;

        if (index == 0U || index > MODBUS_LOCAL_BIN_SD_MAX_SEGMENT_INDEX)
        {
            index = 1U;
        }

        snprintf(final_path,
                 final_path_size,
                 MODBUS_LOCAL_BIN_SD_FILE_PATTERN,
                 (unsigned long)index);
        snprintf(temp_path,
                 temp_path_size,
                 MODBUS_LOCAL_BIN_SD_TEMP_PATTERN,
                 (unsigned long)index);
        snprintf(sent_path,
                 sizeof(sent_path),
                 MODBUS_LOCAL_BIN_SD_SENT_PATTERN,
                 (unsigned long)index);

        s_machine_sd_next_segment = index + 1U;

        if (!sdcard_exists(final_path) && !sdcard_exists(sent_path))
        {
            return true;
        }
    }

    return false;
}

static esp_err_t modbus_local_bin_copy_pending_to_sd(void)
{
    char final_path[64];
    char temp_path[64];
    size_t pending_size = 0U;

    if (!sdcard_is_mounted())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t pending_dir_err = sdcard_mkdirs(MQTT_FILE_PENDING_DIR);
    esp_err_t sent_dir_err = sdcard_mkdirs(MQTT_FILE_SENT_DIR);
    if (pending_dir_err != ESP_OK || sent_dir_err != ESP_OK)
    {
        return ESP_FAIL;
    }

    /* A prior SD copy succeeded but LittleFS deletion needs another attempt. */
    if (s_machine_pending_already_archived)
    {
        esp_err_t delete_err =
            littlefs_delete_file(MODBUS_LOCAL_BIN_PENDING_PATH);
        if (delete_err == ESP_OK)
        {
            s_machine_pending_already_archived = false;
        }
        return delete_err;
    }

    if (!modbus_local_bin_next_sd_paths(final_path,
                                        sizeof(final_path),
                                        temp_path,
                                        sizeof(temp_path)))
    {
        return ESP_ERR_NO_MEM;
    }

    if (modbus_local_bin_file_size(MODBUS_LOCAL_BIN_PENDING_PATH,
                                   &pending_size) != ESP_OK ||
        pending_size < MODBUS_LOCAL_BIN_RECORD_SIZE)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)sdcard_delete_file(temp_path);

    FILE *source = fopen(MODBUS_LOCAL_BIN_PENDING_PATH, "rb");
    if (source == NULL)
    {
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "Cannot open pending BIN %s: errno=%d (%s)",
                 MODBUS_LOCAL_BIN_PENDING_PATH,
                 errno,
                 strerror(errno));
        return ESP_FAIL;
    }

    FILE *destination = fopen(temp_path, "wb");
    if (destination == NULL)
    {
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "Cannot create SD temporary file %s: errno=%d (%s)",
                 temp_path,
                 errno,
                 strerror(errno));
        fclose(source);
        return ESP_FAIL;
    }

    esp_err_t copy_err = ESP_OK;
    size_t total_copied = 0U;

    while (true)
    {
        size_t bytes_read = fread(s_machine_sd_copy_buffer,
                                  1U,
                                  sizeof(s_machine_sd_copy_buffer),
                                  source);

        if (bytes_read != 0U)
        {
            size_t bytes_written = fwrite(s_machine_sd_copy_buffer,
                                          1U,
                                          bytes_read,
                                          destination);
            if (bytes_written != bytes_read)
            {
                copy_err = ESP_FAIL;
                break;
            }
            total_copied += bytes_written;
        }

        if (bytes_read < sizeof(s_machine_sd_copy_buffer))
        {
            if (ferror(source))
            {
                copy_err = ESP_FAIL;
            }
            break;
        }
    }

    if (copy_err == ESP_OK && fflush(destination) != 0)
    {
        copy_err = ESP_FAIL;
    }

    fclose(source);
    if (fclose(destination) != 0)
    {
        copy_err = ESP_FAIL;
    }

    if (copy_err == ESP_OK && total_copied != pending_size)
    {
        copy_err = ESP_FAIL;
    }

    if (copy_err != ESP_OK)
    {
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "SD write failed after %u of %u bytes",
                 (unsigned)total_copied,
                 (unsigned)pending_size);
        (void)sdcard_delete_file(temp_path);
        return copy_err;
    }

    if (rename(temp_path, final_path) != 0)
    {
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "Cannot finalize SD file %s -> %s: errno=%d (%s)",
                 temp_path,
                 final_path,
                 errno,
                 strerror(errno));
        (void)sdcard_delete_file(temp_path);
        return ESP_FAIL;
    }

    size_t sd_file_size = 0U;
    if (modbus_local_bin_file_size(final_path, &sd_file_size) != ESP_OK ||
        sd_file_size != pending_size)
    {
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "SD verification failed for %s: expected=%u actual=%u",
                 final_path,
                 (unsigned)pending_size,
                 (unsigned)sd_file_size);
        (void)sdcard_delete_file(final_path);
        return ESP_FAIL;
    }

    s_machine_pending_already_archived = true;
    esp_err_t delete_err = littlefs_delete_file(MODBUS_LOCAL_BIN_PENDING_PATH);
    if (delete_err != ESP_OK)
    {
        return delete_err;
    }

    s_machine_pending_already_archived = false;
    size_t new_active_size = 0U;
    (void)modbus_local_bin_file_size(MODBUS_LOCAL_BIN_FILE_PATH,
                                     &new_active_size);
    ESP_LOGI(TAG_MODBUS_MASTER,
             "Archived and verified %u bytes to %s; internal pending data "
             "deleted; active BIN=%u bytes",
             (unsigned)total_copied,
             final_path,
             (unsigned)new_active_size);
    mqtt_file_uploader_notify_file_ready();
    return ESP_OK;
}

static void modbus_local_bin_sd_archive_task(void *arg)
{
    (void)arg;

    size_t active_size = 0U;
    size_t pending_size = 0U;
    (void)modbus_local_bin_file_size(MODBUS_LOCAL_BIN_FILE_PATH,
                                     &active_size);
    (void)modbus_local_bin_file_size(MODBUS_LOCAL_BIN_PENDING_PATH,
                                     &pending_size);

    ESP_LOGI(TAG_MODBUS_MASTER,
             "Local BIN archive ready: threshold=%u KB (%u bytes), "
             "active=%u bytes, pending=%u bytes, SD=%s",
             (unsigned)s_machine_sd_threshold_kb,
             (unsigned)(s_machine_sd_threshold_kb * 1024U),
             (unsigned)active_size,
             (unsigned)pending_size,
             sdcard_is_mounted() ? "mounted" : "NOT mounted");

    while (true)
    {
        if (!littlefs_exists(MODBUS_LOCAL_BIN_PENDING_PATH))
        {
            s_machine_pending_already_archived = false;
            (void)modbus_local_bin_rotate_if_threshold_reached();
        }

        if (littlefs_exists(MODBUS_LOCAL_BIN_PENDING_PATH))
        {
            if (!sdcard_is_mounted())
            {
                if (!s_machine_sd_missing_logged)
                {
                    ESP_LOGW(TAG_MODBUS_MASTER,
                             "BIN threshold reached, but SD is not mounted; "
                             "pending data is retained in LittleFS");
                    s_machine_sd_missing_logged = true;
                }

                (void)ulTaskNotifyTake(
                    pdTRUE,
                    pdMS_TO_TICKS(MODBUS_LOCAL_BIN_SD_CHECK_PERIOD_MS));
                continue;
            }

            s_machine_sd_missing_logged = false;
            esp_err_t err = modbus_local_bin_copy_pending_to_sd();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG_MODBUS_MASTER,
                         "Pending BIN archive failed: %s",
                         esp_err_to_name(err));
                (void)ulTaskNotifyTake(
                    pdTRUE,
                    pdMS_TO_TICKS(MODBUS_LOCAL_BIN_SD_RETRY_PERIOD_MS));
                continue;
            }

            /* Process another full active segment immediately, if one exists. */
            continue;
        }

        /* A successful append or threshold change wakes this task at once. */
        (void)ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(MODBUS_LOCAL_BIN_SD_CHECK_PERIOD_MS));
    }
}

static uint16_t modbus_machine_record_get(const uint8_t *record,
                                          size_t register_index)
{
    size_t offset = register_index * 2U;

    return ((uint16_t)record[offset] << 8) |
           (uint16_t)record[offset + 1U];
}

static void modbus_machine_record_put(uint8_t *record,
                                      size_t register_index,
                                      uint16_t value)
{
    size_t offset = register_index * 2U;

    record[offset] = (uint8_t)((value >> 8) & 0xFFU);
    record[offset + 1U] = (uint8_t)(value & 0xFFU);
}

static void modbus_machine_record_put_input(uint8_t *record,
                                            size_t input_register,
                                            uint16_t value)
{
    modbus_machine_record_put(record,
                              MODBUS_LOCAL_BIN_INPUT_START_INDEX +
                                  input_register,
                              value);
}

static uint32_t modbus_local_bin_get_unix_timestamp(void)
{
    time_t now = 0;

    /* SNTP is started only after Wi-Fi or Ethernet has an IP address. */
    if (!app_time_is_valid())
    {
        return 0U;
    }

    time(&now);
    if (now < 0 || (uint64_t)now > (uint64_t)UINT32_MAX)
    {
        return 0U;
    }

    return (uint32_t)now;
}

static void modbus_machine_record_put_timestamp(uint8_t *record,
                                                uint32_t timestamp)
{
    modbus_machine_record_put(
        record,
        MODBUS_LOCAL_BIN_TIMESTAMP_HI_INDEX,
        (uint16_t)(timestamp >> 16));
    modbus_machine_record_put(
        record,
        MODBUS_LOCAL_BIN_TIMESTAMP_LO_INDEX,
        (uint16_t)(timestamp & 0xFFFFU));
}

/*
 * Build input registers 0..21 directly from the already-polled runtime
 * snapshot. Words 0..1 remain zero until the record is actually written.
 */
static void modbus_build_local_machine_record(
    const hmi_data_t *snap,
    uint8_t record[MODBUS_LOCAL_BIN_RECORD_SIZE])
{
    uint16_t error_bits = 0U;

    memset(record, 0, MODBUS_LOCAL_BIN_RECORD_SIZE);

    modbus_machine_record_put_input(record, INP_ADDR_MACHINE_STATE_FB,
                                    (uint16_t)snap->machine_state_fb);
    modbus_machine_record_put_input(record, INP_ADDR_AUTO_POWER_PERCENT_FB,
                                    snap->auto_power_percent_fb);
    modbus_machine_record_put_input(record, INP_ADDR_POWER_PERCENT,
                                    snap->power_percent);
    modbus_machine_record_put_input(record, INP_ADDR_LINE_1_V, snap->line_1_v);
    modbus_machine_record_put_input(record, INP_ADDR_LINE_1_A, snap->line_1_a);
    modbus_machine_record_put_input(record, INP_ADDR_LINE_2_V, snap->line_2_v);
    modbus_machine_record_put_input(record, INP_ADDR_LINE_2_A, snap->line_2_a);
    modbus_machine_record_put_input(record, INP_ADDR_LINE_3_V, snap->line_3_v);
    modbus_machine_record_put_input(record, INP_ADDR_LINE_3_A, snap->line_3_a);
    modbus_machine_record_put_input(record, INP_ADDR_AVG_V, snap->avg_v);
    modbus_machine_record_put_input(record, INP_ADDR_AVG_A, snap->avg_a);
    modbus_machine_record_put_input(record, INP_ADDR_PWM_FREQ, snap->pwm_freq);
    modbus_machine_record_put_input(record, INP_ADDR_AVG_KW, snap->avg_kw);
    modbus_machine_record_put_input(record, INP_ADDR_AVG_PF, snap->avg_pf);

    for (size_t bit = 0; bit < 11U; bit++)
    {
        if (snap->error_leds[bit] != 0U)
        {
            error_bits |= (uint16_t)(1U << bit);
        }
    }
    modbus_machine_record_put_input(record, INP_ADDR_ERROR_BITS_LO, error_bits);

#ifdef _MACHINE_TEMPERATURE_CNTRL_
    modbus_machine_record_put_input(record, INP_ADDR_MELTER_TEMP,
                                    snap->melter_temp);
#endif

#ifdef _MACHINE_TIMER_CNTRL_
    modbus_machine_record_put_input(record, INP_ADDR_TIMER_SEC,
                                    snap->timer_sec);
    modbus_machine_record_put_input(record, INP_ADDR_TIMER_MS,
                                    snap->timer_ms);
    modbus_machine_record_put_input(record, INP_ADDR_JOB_COUNTER,
                                    snap->timer_finish_count);
#endif

    modbus_machine_record_put_input(record, INP_ADDR_CHILLER_TEMP,
                                    snap->chiller_temp);
    modbus_machine_record_put_input(record, INP_ADDR_IGBT_PLATE_TEMP,
                                    snap->igbt_plate_temp);
    modbus_machine_record_put_input(record, INP_ADDR_COIL_TEMP,
                                    snap->coil_temp);
    modbus_machine_record_put_input(
        record,
        INP_ADDR_COMM_STATUS,
        (uint16_t)(0x0003U | (modbus_hmi_master_is_online() ? 0x0004U : 0U)));
}

static void modbus_log_changed_machine_registers(const uint8_t *previous,
                                                 const uint8_t *current)
{
    for (size_t reg = 0; reg < MODBUS_LOCAL_BIN_INPUT_REGISTER_COUNT; reg++)
    {
        size_t record_index = MODBUS_LOCAL_BIN_INPUT_START_INDEX + reg;
        uint16_t old_value = modbus_machine_record_get(previous, record_index);
        uint16_t new_value = modbus_machine_record_get(current, record_index);

        if (old_value != new_value)
        {
            ESP_LOGI(TAG_MODBUS_MASTER,
                     "BIN REG[%02u] changed: %u (0x%04X) -> %u (0x%04X)",
                     (unsigned)reg,
                     (unsigned)old_value,
                     (unsigned)old_value,
                     (unsigned)new_value,
                     (unsigned)new_value);
        }
    }
}

static void modbus_store_local_machine_record_if_changed(const hmi_data_t *snap)
{
    uint8_t current[MODBUS_LOCAL_BIN_RECORD_SIZE];

    if (snap == NULL)
    {
        return;
    }

    modbus_build_local_machine_record(snap, current);

    if (s_machine_last_record_valid &&
        memcmp(current + MODBUS_LOCAL_BIN_INPUT_DATA_OFFSET,
               s_machine_last_record + MODBUS_LOCAL_BIN_INPUT_DATA_OFFSET,
               MODBUS_LOCAL_BIN_INPUT_DATA_SIZE) == 0)
    {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us < s_machine_write_retry_after_us)
    {
        return;
    }

    /*
     * Timestamp only after the input-change check. This preserves the original
     * change-only storage behavior instead of creating one record per second.
     * Zero means that neither the Delta RTC fallback nor SNTP has supplied a
     * valid system time yet.
     */
    modbus_machine_record_put_timestamp(
        current,
        modbus_local_bin_get_unix_timestamp());

    size_t active_size = 0U;
    esp_err_t err = modbus_local_bin_append_locked(
        current,
        MODBUS_LOCAL_BIN_RECORD_SIZE,
        &active_size);
    if (err != ESP_OK)
    {
        s_machine_write_retry_after_us =
            now_us + ((int64_t)MODBUS_LOCAL_BIN_WRITE_RETRY_MS * 1000LL);
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "Changed timestamped BIN record was not saved: %s",
                 esp_err_to_name(err));
        return;
    }

    if (s_machine_last_record_valid)
    {
        modbus_log_changed_machine_registers(s_machine_last_record, current);
        ESP_LOGI(TAG_MODBUS_MASTER,
                 "Changed timestamp + 22-register record appended; "
                 "active BIN=%u/%u "
                 "bytes",
                 (unsigned)active_size,
                 (unsigned)(s_machine_sd_threshold_kb * 1024U));
    }
    else
    {
        ESP_LOGI(TAG_MODBUS_MASTER,
                 "Initial timestamp + 22-register record appended; "
                 "active BIN=%u/%u "
                 "bytes",
                 (unsigned)active_size,
                 (unsigned)(s_machine_sd_threshold_kb * 1024U));
    }

    memcpy(s_machine_last_record,
           current,
           MODBUS_LOCAL_BIN_RECORD_SIZE);
    s_machine_last_record_valid = true;
    s_machine_write_retry_after_us = 0;

    if (s_machine_sd_archive_task_handle != NULL)
    {
        xTaskNotifyGive(s_machine_sd_archive_task_handle);
    }
}

void read_latest_machine_record(void)
{
    const char *read_path = MODBUS_LOCAL_BIN_FILE_PATH;
    size_t active_size = 0U;

    if (modbus_local_bin_file_size(MODBUS_LOCAL_BIN_FILE_PATH,
                                   &active_size) != ESP_OK ||
        active_size < MODBUS_LOCAL_BIN_RECORD_SIZE)
    {
        read_path = MODBUS_LOCAL_BIN_PENDING_PATH;
    }

    FILE *file = fopen(read_path, "rb");

    if (file == NULL)
    {
        /* A missing file is normal on first boot. */
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "Cannot seek %s: %s",
                 read_path,
                 strerror(errno));
        fclose(file);
        return;
    }

    long file_size = ftell(file);
    if (file_size < (long)MODBUS_LOCAL_BIN_RECORD_SIZE)
    {
        fclose(file);
        return;
    }

    long complete_records =
        file_size / (long)MODBUS_LOCAL_BIN_RECORD_SIZE;
    long latest_position =
        (complete_records - 1L) * (long)MODBUS_LOCAL_BIN_RECORD_SIZE;

    if (fseek(file, latest_position, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "Cannot seek latest BIN record: %s",
                 strerror(errno));
        fclose(file);
        return;
    }

    size_t bytes_read = fread(s_machine_last_record,
                              1U,
                              MODBUS_LOCAL_BIN_RECORD_SIZE,
                              file);
    fclose(file);

    s_machine_last_record_valid =
        (bytes_read == MODBUS_LOCAL_BIN_RECORD_SIZE);
}

// void modbus_master_temp_ctrl_init(void)
// {
//     adaptive_temp_pid_init(&g_adapt_pid);
// }

uint16_t modbus_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t pos = 0; pos < len; pos++)
    {
        crc ^= (uint16_t)buf[pos];
        for (int i = 0; i < 8; i++)
        {
            if ((crc & 0x0001) != 0)
            {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool modbus_write_and_verify_single_reg(uint8_t slave_id, uint16_t value, uint16_t reg_addr)
{
    uint8_t tx_buf[8];
    uint8_t rx_buf[8];
    uint16_t crc;

    tx_buf[0] = slave_id;
    tx_buf[1] = MODBUS_FUNC_WRITE_SING_REG;
    tx_buf[2] = (reg_addr >> 8) & 0xFF;
    tx_buf[3] = (reg_addr) & 0xFF;
    tx_buf[4] = (value >> 8) & 0xFF;
    tx_buf[5] = (value) & 0xFF;

    crc = modbus_crc16(tx_buf, 6);
    tx_buf[6] = crc & 0xFF;
    tx_buf[7] = (crc >> 8) & 0xFF;

    uart_flush_input(MODBUS_1_UART_PORT_NUM);
    uart_write_bytes(MODBUS_1_UART_PORT_NUM, tx_buf, sizeof(tx_buf));
    uart_wait_tx_done(MODBUS_1_UART_PORT_NUM, pdMS_TO_TICKS(20));

    int len = uart_read_bytes(MODBUS_1_UART_PORT_NUM,
                              rx_buf,
                              sizeof(rx_buf),
                              pdMS_TO_TICKS(100));

    if (len == 8 &&
        rx_buf[0] == MODBUS_SLAVE_ID_CONTROL_CARD &&
        rx_buf[1] == MODBUS_FUNC_WRITE_SING_REG &&
        rx_buf[2] == tx_buf[2] &&
        rx_buf[3] == tx_buf[3] &&
        rx_buf[4] == tx_buf[4] &&
        rx_buf[5] == tx_buf[5])
    {
        return true;
    }

    return false;
}

bool modbus_write_and_verify_multiple_regs(uint8_t slave_id, const uint16_t *values,
                                           uint16_t start_reg_addr,
                                           uint16_t reg_count)
{
    uint8_t tx_buf[256];
    uint8_t rx_buf[8];
    uint16_t crc;
    uint16_t tx_len;

    if (values == NULL)
        return false;

    if (reg_count == 0 || reg_count > 123)
        return false;

    tx_buf[0] = slave_id;
    tx_buf[1] = MODBUS_FUNC_WRITE_MULT_REGS;
    tx_buf[2] = (start_reg_addr >> 8) & 0xFF;
    tx_buf[3] = (start_reg_addr) & 0xFF;
    tx_buf[4] = (reg_count >> 8) & 0xFF;
    tx_buf[5] = (reg_count) & 0xFF;
    tx_buf[6] = (uint8_t)(reg_count * 2U);

    for (uint16_t i = 0; i < reg_count; i++)
    {
        tx_buf[7 + (i * 2)] = (values[i] >> 8) & 0xFF;
        tx_buf[8 + (i * 2)] = (values[i]) & 0xFF;
    }

    tx_len = 7 + (reg_count * 2U);

    crc = modbus_crc16(tx_buf, tx_len);
    tx_buf[tx_len + 0] = crc & 0xFF;
    tx_buf[tx_len + 1] = (crc >> 8) & 0xFF;

    tx_len += 2;

    uart_flush_input(MODBUS_1_UART_PORT_NUM);
    uart_write_bytes(MODBUS_1_UART_PORT_NUM, tx_buf, tx_len);
    uart_wait_tx_done(MODBUS_1_UART_PORT_NUM, pdMS_TO_TICKS(20));

    int len = uart_read_bytes(MODBUS_1_UART_PORT_NUM,
                              rx_buf,
                              sizeof(rx_buf),
                              pdMS_TO_TICKS(100));

    if (len == 8 &&
        rx_buf[0] == slave_id &&
        rx_buf[1] == MODBUS_FUNC_WRITE_MULT_REGS &&
        rx_buf[2] == tx_buf[2] &&
        rx_buf[3] == tx_buf[3] &&
        rx_buf[4] == tx_buf[4] &&
        rx_buf[5] == tx_buf[5])
    {
        return true;
    }

    return false;
}

int modbus_read_holding_registers(uint8_t slave_id, uint8_t *rx_buf,
                                  uint16_t start_reg_addr,
                                  uint16_t reg_count)
{
    uint8_t tx_buf[8];
    uint16_t crc;

    if (rx_buf == NULL)
        return -1;

    if (reg_count == 0 || reg_count > 125)
        return -1;

    tx_buf[0] = slave_id;
    tx_buf[1] = MODBUS_FUNC_READ_HOLD_REGS;
    tx_buf[2] = (start_reg_addr >> 8) & 0xFF;
    tx_buf[3] = (start_reg_addr) & 0xFF;
    tx_buf[4] = (reg_count >> 8) & 0xFF;
    tx_buf[5] = (reg_count) & 0xFF;

    crc = modbus_crc16(tx_buf, 6);
    tx_buf[6] = crc & 0xFF;
    tx_buf[7] = (crc >> 8) & 0xFF;

    uart_flush_input(MODBUS_1_UART_PORT_NUM);
    uart_write_bytes(MODBUS_1_UART_PORT_NUM, tx_buf, sizeof(tx_buf));
    uart_wait_tx_done(MODBUS_1_UART_PORT_NUM, pdMS_TO_TICKS(20));

    int len = uart_read_bytes(MODBUS_1_UART_PORT_NUM,
                              rx_buf,
                              MODBUS_1_UART_BUF_SIZE,
                              pdMS_TO_TICKS(100));

    return len;
}

int modbus_read_input_registers(uint8_t slave_id,
                                uint8_t *rx_buf,
                                uint16_t start_reg_addr,
                                uint16_t reg_count)
{
    uint8_t tx_buf[8];
    uint16_t crc;

    if (rx_buf == NULL)
        return -1;

    if (reg_count == 0 || reg_count > 125)
        return -1;

    tx_buf[0] = slave_id;
    tx_buf[1] = MODBUS_FUNC_READ_INP_REGS;
    tx_buf[2] = (start_reg_addr >> 8) & 0xFF;
    tx_buf[3] = (start_reg_addr) & 0xFF;
    tx_buf[4] = (reg_count >> 8) & 0xFF;
    tx_buf[5] = (reg_count) & 0xFF;

    crc = modbus_crc16(tx_buf, 6);
    tx_buf[6] = crc & 0xFF;
    tx_buf[7] = (crc >> 8) & 0xFF;

    uart_flush_input(MODBUS_1_UART_PORT_NUM);
    uart_write_bytes(MODBUS_1_UART_PORT_NUM, tx_buf, sizeof(tx_buf));
    uart_wait_tx_done(MODBUS_1_UART_PORT_NUM, pdMS_TO_TICKS(20));

    int len = uart_read_bytes(MODBUS_1_UART_PORT_NUM,
                              rx_buf,
                              MODBUS_1_UART_BUF_SIZE,
                              pdMS_TO_TICKS(100));

    return len;
}

bool send_machine_trigger_to_modbus(uint16_t value)
{
    return modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD, value, HOLD_REG_ADDR_MACHINE_TRIGGER);
}

bool send_control_mode_to_modbus(uint16_t value)
{
    return modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD, value, HOLD_REG_ADDR_CONTROL_MODE);
}

bool send_frequency_to_modbus(uint16_t min_freq, uint16_t max_freq)
{
    const uint16_t values[2] = {min_freq, max_freq};
    return modbus_write_and_verify_multiple_regs(MODBUS_SLAVE_ID_CONTROL_CARD, values, HOLD_REG_ADDR_CFG_FREQ_MIN, 2);
}

bool send_min_pot_to_modbus(uint16_t value)
{
    return modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD, value, HOLD_REG_ADDR_CFG_MIN_POT_VAL);
}

bool send_max_pot_to_modbus(uint16_t value)
{
    return modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD, value, HOLD_REG_ADDR_CFG_MAX_POT_VAL);
}

bool send_min_ct_to_modbus(uint16_t value)
{
    return modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD, value, HOLD_REG_ADDR_CFG_MIN_CT_VAL);
}

bool send_max_ct_to_modbus(uint16_t value)
{
    return modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD, value, HOLD_REG_ADDR_CFG_MAX_CT_VAL);
}

bool send_min_pt_to_modbus(uint16_t value)
{
    return modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD, value, HOLD_REG_ADDR_CFG_MIN_PT_VAL);
}

bool send_max_pt_to_modbus(uint16_t value)
{
    return modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD, value, HOLD_REG_ADDR_CFG_MAX_PT_VAL);
}

bool modbus_read_input_regs_11_22(void)
{
    const bool dummy_data_enabled = true;
    const uint16_t reg_count = 12;
    uint8_t rx_buf[MODBUS_1_UART_BUF_SIZE];
    int len;

    len = modbus_read_input_registers(MODBUS_SLAVE_ID_CONTROL_CARD, rx_buf, INP_REG_ADDR_POT_PERCENT, reg_count);

    if (len >= 29 &&
        rx_buf[0] == MODBUS_SLAVE_ID_CONTROL_CARD &&
        rx_buf[1] == MODBUS_FUNC_READ_INP_REGS &&
        rx_buf[2] == reg_count * 2ULL)
    {
        if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        {
            int i = 3;

            /* Reg 11 */
            hmi_data.power_percent = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;

            /* Reg 12 */
            hmi_data.line_1_v = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("L1 Value: %hd\n", hmi_data.line_1_v);

            /* Reg 13 */
            hmi_data.line_1_a = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("A1 Value: %hd\n", hmi_data.line_1_a);

            /* Reg 14 */
            hmi_data.line_2_v = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("L2 Value: %hd\n", hmi_data.line_2_v);

            /* Reg 15 */
            hmi_data.line_2_a = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("A2 Value: %hd\n", hmi_data.line_2_a);

            /* Reg 16 */
            hmi_data.line_3_v = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("L3 Value: %hd\n", hmi_data.line_3_v);

            /* Reg 17 */
            hmi_data.line_3_a = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("A3 Value: %hd\n", hmi_data.line_3_a);

            /* Reg 18 */
            hmi_data.avg_v = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("LAVG Value: %hd\n", hmi_data.avg_v);

            /* Reg 19 */
            hmi_data.avg_a = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("AAVG Value: %hd\n", hmi_data.avg_a);

            /* Reg 20 */
            hmi_data.pwm_freq = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("pwm_freq Value: %hd\n", hmi_data.pwm_freq);
            // printf("pwm_freq Value: %u\n", (unsigned)hmi_data.pwm_freq);

            /* Reg 21 */
            hmi_data.avg_kw = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];
            i += 2;
            // printf("KW Value: %hd\n", hmi_data.avg_kw);

            /* Reg 22 */
            hmi_data.avg_pf = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];

            if (hmi_data.avg_pf >= 998)
                hmi_data.avg_pf -= 3;
            // printf("PF Value: %hd\n", hmi_data.avg_pf);

            hmi_data_unlock();
        }

        if (dummy_data_enabled)
        {
            hmi_data.line_1_v = 4300;
            hmi_data.line_1_a = 250;
            hmi_data.line_2_v = 4300;
            hmi_data.line_2_a = 250;
            hmi_data.line_3_v = 4300;
            hmi_data.line_3_a = 250;
            hmi_data.avg_v = 4300;
            hmi_data.avg_a = 250;
            hmi_data.pwm_freq = 1555;
            hmi_data.avg_kw = 150;
            hmi_data.avg_pf = 999;
        }

        return true;
    }

    ESP_LOGW(TAG_MODBUS_MASTER, "Input register read 11..22 failed, len=%d", len);
    return false;
}

#ifdef _MACHINE_TEMPERATURE_CNTRL_
static void modbus_master_temp_ctrl_step(hmi_data_t *snap)
{
    const float dt_s = 0.2f;

    float measured_temp_c;
    float setpoint_c;
    float power_fb_pct;
    float power_cmd_pct = 0.0f;
    bool control_enable;
    uint8_t local_temp_cut_cmd = TEMP_CUT_OFF;

    if (snap == NULL)
        return;

    measured_temp_c = snap->melter_temp;
    setpoint_c = snap->melter_set_temp;
    power_fb_pct = (float)snap->auto_power_percent_fb;

    control_enable =
        (snap->machine_state_fb == ON) ||
        (snap->machine_state_fb == TEMP_CUT_ON) ||
        (snap->machine_state_fb == TEMP_CUT_OFF);

    // adaptive_temp_pid_step(&g_adapt_pid,
    //                     measured_temp_c,
    //                     setpoint_c,
    //                     power_fb_pct,
    //                     control_enable,
    //                     dt_s,
    //                     &power_cmd_pct,
    //                     &local_temp_cut_cmd);

    /* Apply controller output into snapshot only */
    snap->auto_power_percent = (uint16_t)(power_cmd_pct + 0.5f);

    /*
     * Local overlay:
     * TEMP_CUT_ON means force machine state to TEMP_CUT_ON and zero power.
     * Otherwise, if machine is running in thermal mode, keep it ON.
     */
    if (local_temp_cut_cmd == TEMP_CUT_ON)
    {
        snap->machine_state = TEMP_CUT_ON;
        snap->auto_power_percent = 0U;
    }
    else
    {
        if ((snap->machine_state == TEMP_CUT_ON) ||
            (snap->machine_state == TEMP_CUT_OFF) ||
            (snap->machine_state == ON))
        {
            snap->machine_state = ON;
        }
    }
}
#endif

#ifdef _MACHINE_TIMER_CNTRL_
static void hmi_timer_update_state(bool state)
{
    uint64_t now_us;
    uint32_t set_total_ms;

    if (!hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        return;

    now_us = esp_timer_get_time();

    set_total_ms =
        ((uint32_t)hmi_data.timer_set_sec * 1000U) +
        (uint32_t)hmi_data.timer_set_ms;

    if (state)
    {
        /* rising edge only */
        if (!hmi_data.timer_prev_on)
        {
            hmi_data.timer_deadline_us = now_us + ((uint64_t)set_total_ms * 1000ULL);
            hmi_data.timer_remaining_ms = set_total_ms;
            hmi_data.timer_sec = (uint16_t)(set_total_ms / 1000U);
            hmi_data.timer_ms = (uint16_t)(set_total_ms % 1000U);
            hmi_data.timer_running = true;
            hmi_data.timer_prev_on = true;
        }
    }
    else
    {
        hmi_data.timer_remaining_ms = set_total_ms;
        hmi_data.timer_sec = (uint16_t)(set_total_ms / 1000U);
        hmi_data.timer_ms = (uint16_t)(set_total_ms % 1000U);
        hmi_data.timer_deadline_us = 0U;
        hmi_data.timer_running = false;
        hmi_data.timer_prev_on = false;
    }

    hmi_data_unlock();
}

static void modbus_send_timer_expired_off_if_needed(void)
{
    bool need_off = false;

    if (!hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        return;

    if (hmi_data.timer_expired_off_pending &&
        (hmi_data.machine_state_fb == ON || hmi_data.machine_state_fb == TEMP_CUT_OFF))
    {
        need_off = true;
    }
    else if (hmi_data.timer_expired_off_pending &&
             (hmi_data.machine_state_fb != ON && hmi_data.machine_state_fb != TEMP_CUT_OFF))
    {
        /* already stopped, clear stale request */
        hmi_data.timer_expired_off_pending = false;
    }

    hmi_data_unlock();

    if (!need_off)
        return;

    if (modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_CONTROL_CARD,
                                           OFF,
                                           HOLD_REG_ADDR_MACHINE_TRIGGER))
    {
        if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        {
            hmi_data.timer_expired_off_pending = false;
            hmi_data.machine_state = OFF;
            hmi_data_unlock();
        }

        ESP_LOGI(TAG_MODBUS_MASTER, "[TIMER] Expired -> OFF command sent");
    }
    else
    {
        ESP_LOGW(TAG_MODBUS_MASTER, "[TIMER] Expired -> OFF command send failed");
    }
}

#endif

bool control_card_poll_once(void)
{
    static uint8_t rx_buf[MODBUS_1_UART_BUF_SIZE];
    int len = 0;
    uint16_t crc;
    TickType_t now;
    hmi_data_t snap;
    if (!hmi_data_get_snapshot(&snap, HMI_DATA_SNAPSHOT_TIMEOUT))
        return false;

    vTaskDelay(pdMS_TO_TICKS(5));
    if (!modbus_read_input_regs_11_22())
    {
        ESP_LOGW(TAG_MODBUS_MASTER, "[CONTROL CARD] Failed to read input regs 11..22");
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    memset(rx_buf, 0, sizeof(rx_buf));
    len = modbus_read_holding_registers(MODBUS_SLAVE_ID_CONTROL_CARD, rx_buf, HOLD_REG_ADDR_MACHINE_TRIGGER, 1);

    if (len >= 7 &&
        rx_buf[0] == MODBUS_SLAVE_ID_CONTROL_CARD &&
        rx_buf[1] == MODBUS_FUNC_READ_HOLD_REGS)
    {
        uint16_t mstate = ((uint16_t)rx_buf[3] << 8) | rx_buf[4];
        snap.machine_state_fb = (MACHINE_TRIGGER_ENUM)mstate;
        if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        {
            hmi_data.machine_state_fb = snap.machine_state_fb;

#ifdef _MACHINE_TIMER_CNTRL_

            if (hmi_data.timer_finish_wait_off_ack &&
                (hmi_data.machine_state_fb == OFF || hmi_data.machine_state_fb == IDLE))
            {
                hmi_data.timer_finish_wait_off_ack = false;
                hmi_data.timer_finish_count++;
            }

#endif

            hmi_data_unlock();
        }

#ifdef _MACHINE_TIMER_CNTRL_
        bool state = (snap.machine_state_fb == ON || snap.machine_state_fb == TEMP_CUT_OFF);
        hmi_timer_update_state(state);
#endif

        switch (snap.machine_state_fb)
        {
        case ON:
        case TEMP_CUT_ON:
        case TEMP_CUT_OFF:
        {
            /*
             * Do not overwrite explicit user commands like OFF/IDLE/ERROR
             * just because feedback is still running for one cycle.
             *
             * Only normalize local running overlay states.
             */
            if (snap.machine_state == TEMP_CUT_OFF)
            {
                snap.machine_state = ON;
                if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
                {
                    hmi_data.machine_state = snap.machine_state;
                    hmi_data_unlock();
                }
            }
            break;
        }

        case OFF:
        case IDLE:
        case ERROR:
        {
            snap.machine_state = snap.machine_state_fb;
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                hmi_data.machine_state = snap.machine_state;
                hmi_data_unlock();
            }
            break;
        }

        default:
            break;
        }

#ifdef _MACHINE_TEMPERATURE_CNTRL_
        ESP_LOGI(TAG_MODBUS_MASTER,
                 "[CONTROL CARD] State sync: fb=%d local=%d temp=%u set=%u",
                 snap.machine_state_fb,
                 snap.machine_state,
                 snap.melter_temp,
                 snap.melter_set_temp);
#endif

#ifdef _MACHINE_TIMER_CNTRL_
        ESP_LOGI(TAG_MODBUS_MASTER,
                 "[CONTROL CARD] State sync: fb=%d local=%d timer=%u job=%u",
                 snap.machine_state_fb,
                 snap.machine_state,
                 snap.timer_ms,
                 snap.job_counter);
#endif
    }
    else
    {
        ESP_LOGW(TAG_MODBUS_MASTER, "[CONTROL CARD] Holding register read failed, len=%d", len);
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    memset(rx_buf, 0, sizeof(rx_buf));
    len = modbus_read_holding_registers(MODBUS_SLAVE_ID_CONTROL_CARD, rx_buf, HOLD_REG_ADDR_AUTO_POWER_PERCENT_VAL, 1);

    if (len >= 7 &&
        rx_buf[0] == MODBUS_SLAVE_ID_CONTROL_CARD &&
        rx_buf[1] == MODBUS_FUNC_READ_HOLD_REGS)
    {
        uint16_t auto_power_percent_fb = ((uint16_t)rx_buf[3] << 8) | rx_buf[4];
        snap.auto_power_percent_fb = auto_power_percent_fb;
        if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        {
            hmi_data.auto_power_percent_fb = snap.auto_power_percent_fb;
            hmi_data_unlock();
        }
        // printf("AUTO POWER %%: %hd\n", snap.auto_power_percent_fb);
    }
    else
    {
        ESP_LOGW(TAG_MODBUS_MASTER, "[CONTROL CARD] Holding register read failed, len=%d", len);
    }

    // vTaskDelay(pdMS_TO_TICKS(10));

    // if (modbus_check_discrete_input_flag)
    {
        vTaskDelay(pdMS_TO_TICKS(5));

        /* =========================================================
         * 4) DISCRETE INPUTS
         * ========================================================= */
        {
            uint8_t raw[11];

            if (read_discrete_inputs(raw))
            {
                for (int i = 0; i < 11; i++)
                {
                    snap.error_leds[i] = raw[i];
                }

                if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
                {

                    for (int i = 0; i < 11; i++)
                    {
                        hmi_data.error_leds[i] = snap.error_leds[i];
                    }

                    hmi_data_unlock();
                }
            }
            else
            {
                ESP_LOGW(TAG_MODBUS_MASTER, "[CONTROL CARD] Failed to read discrete inputs");
            }
        }
        // modbus_check_discrete_input_flag = false;
    }
#ifdef _MACHINE_TIMER_CNTRL_
    modbus_send_timer_expired_off_if_needed();
#endif

    return true;
}

bool sensor_card_poll_once(void)
{
    static uint16_t melter_temp_buffer[MELTER_TEMP_AVG_COUNT] = {0};
    static uint32_t melter_temp_sum = 0;
    static uint8_t melter_temp_index = 0;
    static bool melter_temp_filled = false;

    uint8_t rx_buf[MODBUS_1_UART_BUF_SIZE];
    int len = 0;

    len = modbus_read_holding_registers(MODBUS_SLAVE_ID_SENSOR_CARD, rx_buf, MODBUS_SENSOR_CARD_START_ADDR, MODBUS_SENSOR_CARD_REG_COUNT);

    /* Expected response:
       slave + func + bytecount + 8 data bytes + 2 crc = 13 bytes */
    if (len >= 13 &&
        rx_buf[0] == MODBUS_SLAVE_ID_SENSOR_CARD &&
        rx_buf[1] == MODBUS_FUNC_READ_HOLD_REGS &&
        rx_buf[2] == (MODBUS_SENSOR_CARD_REG_COUNT * 2))
    {
        uint16_t rx_crc = (uint16_t)rx_buf[11] | ((uint16_t)rx_buf[12] << 8);
        uint16_t calc_crc = modbus_crc16(rx_buf, 11);

        if (rx_crc != calc_crc)
        {
            ESP_LOGW(TAG_MODBUS_MASTER,
                     "[SENSOR CARD] CRC mismatch: rx=%04X calc=%04X len=%d",
                     rx_crc,
                     calc_crc,
                     len);
            return false;
        }

        for (int i = 0; i < MODBUS_SENSOR_CARD_REG_COUNT; i++)
        {
            uint16_t val = ((uint16_t)rx_buf[3 + i * 2] << 8) |
                           ((uint16_t)rx_buf[4 + i * 2]);

            switch (i)
            {
#ifdef _MACHINE_TEMPERATURE_CNTRL_
            case REG_ADDR_TC:
            {
                melter_temp_sum -= melter_temp_buffer[melter_temp_index];
                melter_temp_buffer[melter_temp_index] = val;
                melter_temp_sum += val;

                melter_temp_index = (melter_temp_index + 1) % MELTER_TEMP_AVG_COUNT;
                if (melter_temp_index == 0)
                    melter_temp_filled = true;

                {
                    uint8_t count = melter_temp_filled ? MELTER_TEMP_AVG_COUNT : melter_temp_index;
                    if (count == 0)
                        count = 1;
                    if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
                    {
                        hmi_data.melter_temp = (uint16_t)(melter_temp_sum / count);
                        hmi_data_unlock();
                    }
                }
                break;
            }
#endif

            case REG_ADDR_RTD_1:
                if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
                {
                    hmi_data.chiller_temp = val;
                    hmi_data_unlock();
                }
                break;

            case REG_ADDR_RTD_2:
                if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
                {
                    hmi_data.igbt_plate_temp = val;
                    hmi_data_unlock();
                }
                break;

            case REG_ADDR_RTD_3:
                if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
                {
                    hmi_data.coil_temp = val;
                    hmi_data_unlock();
                }
                break;

            default:
                break;
            }
        }

        return true;
    }
    else
    {
        ESP_LOGW(TAG_MODBUS_MASTER,
                 "[SENSOR CARD] Read failed len=%d id=%02X fn=%02X bc=%02X",
                 len,
                 (len > 0) ? rx_buf[0] : 0,
                 (len > 1) ? rx_buf[1] : 0,
                 (len > 2) ? rx_buf[2] : 0);
        return false;
    }
}

void modbus_master_task(void *arg)
{

    const uint8_t reg_count = 11;
    uint8_t rx_buf[MODBUS_1_UART_BUF_SIZE];
    int len;

    len = modbus_read_holding_registers(MODBUS_SLAVE_ID_CONTROL_CARD, rx_buf, HOLD_REG_ADDR_MACHINE_TRIGGER, reg_count);

    if (len >= (5 + reg_count * 2) &&
        rx_buf[0] == MODBUS_SLAVE_ID_CONTROL_CARD &&
        rx_buf[1] == MODBUS_FUNC_READ_HOLD_REGS &&
        rx_buf[2] == reg_count * 2U)
    {
        uint16_t val = 0;
        if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        {

            for (int i = 3, idx = 0; idx < reg_count; i += 2, idx++)
            {
                val = ((uint16_t)rx_buf[i] << 8) | rx_buf[i + 1];

                switch (idx)
                {
                case HOLD_REG_ADDR_MACHINE_TRIGGER:
                {
                    hmi_data.machine_state = (MACHINE_TRIGGER_ENUM)val;
                    break;
                }

                case HOLD_REG_ADDR_CONTROL_MODE:
                {
                    hmi_data.control_mode = val;
                    break;
                }

                case HOLD_REG_ADDR_CFG_FREQ_MIN:
                {
                    hmi_data.freq_min = val;
                    break;
                }

                case HOLD_REG_ADDR_CFG_FREQ_MAX:
                {
                    hmi_data.freq_max = val;
                    break;
                }

                case HOLD_REG_ADDR_CFG_MIN_POT_VAL:
                {
                    hmi_data.min_pot = val;
                    break;
                }

                case HOLD_REG_ADDR_CFG_MAX_POT_VAL:
                {
                    hmi_data.max_pot = val;
                    break;
                }

                case HOLD_REG_ADDR_CFG_MIN_CT_VAL:
                {
                    hmi_data.min_ct = val;
                    break;
                }

                case HOLD_REG_ADDR_CFG_MAX_CT_VAL:
                {
                    hmi_data.max_ct = val;
                    break;
                }

                case HOLD_REG_ADDR_CFG_MIN_PT_VAL:
                {
                    hmi_data.min_pt = val;
                    break;
                }

                case HOLD_REG_ADDR_CFG_MAX_PT_VAL:
                {
                    hmi_data.max_pt = val;
                    break;
                }

                case HOLD_REG_ADDR_AUTO_POWER_PERCENT_VAL:
                {
                    hmi_data.auto_power_percent = val;
                    break;
                }

                default:
                    break;
                }
            }

            hmi_data_unlock();
        }
    }

    while (1)
    {
        while (modbus_busy_flag)
            vTaskDelay(pdMS_TO_TICKS(2));

        modbus_busy_flag = true;

        /* 1) Apply staged writes from unified slave map */
        modbus_slave_apply_pending_writes();
        vTaskDelay(pdMS_TO_TICKS(1));

        /* 2) Poll control card => refresh hmi_data feedback */
        control_card_poll_once();
        vTaskDelay(pdMS_TO_TICKS(2));

        /* 3) Poll sensor card => refresh hmi_data feedback */
        sensor_card_poll_once();
        vTaskDelay(pdMS_TO_TICKS(2));

        /* 4) Rebuild unified slave readback banks from latest runtime */
        modbus_slave_sync_from_runtime();
        vTaskDelay(pdMS_TO_TICKS(1));

        {
            hmi_data_t log_snap;
            if (hmi_data_get_snapshot(&log_snap, HMI_DATA_SNAPSHOT_TIMEOUT))
            {
                /* 5) Persist the local snapshot only when any register changes. */
                modbus_store_local_machine_record_if_changed(&log_snap);
                event_log_process_snapshot(&log_snap);
            }
        }

        modbus_busy_flag = false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool read_discrete_inputs(uint8_t out[11])
{
    uint8_t tx[8];
    uint8_t rx[8];

    tx[0] = MODBUS_SLAVE_ID_CONTROL_CARD;
    tx[1] = MODBUS_FUNC_READ_DISC_INP;
    tx[2] = 0x00;
    tx[3] = 0x00;
    tx[4] = 0x00;
    tx[5] = 0x0B; // 11 inputs

    uint16_t crc = modbus_crc16(tx, 6);
    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;

    uart_flush_input(MODBUS_1_UART_PORT_NUM);
    uart_write_bytes(MODBUS_1_UART_PORT_NUM, (const char *)tx, sizeof(tx));

    int len = uart_read_bytes(MODBUS_1_UART_PORT_NUM,
                              rx,
                              sizeof(rx),
                              pdMS_TO_TICKS(200));
    if (len < 7)
    {
        return false;
    }

    if (rx[0] != MODBUS_SLAVE_ID_CONTROL_CARD || rx[1] != 0x02 || rx[2] < 2)
    {
        return false;
    }

    uint16_t resp_crc = (uint16_t)rx[len - 1] << 8 | rx[len - 2];
    if (modbus_crc16(rx, len - 2) != resp_crc)
    {
        return false;
    }

    for (int i = 0; i < 11; i++)
    {
        int byte_idx = 3 + (i / 8);
        int bit_idx = i % 8;
        out[i] = (rx[byte_idx] >> bit_idx) & 0x01;
    }

    return true;
}

/* =========================================================
 * DUMMY TEMPERATURE MODEL
 * ========================================================= */
#ifdef _MACHINE_TEMPERATURE_CNTRL_
void dummy_update_melter_temp(void)
{
    static float temp_f = 0.0f;
    static float retained_heat = 0.0f;

    if (temp_f <= 0.0f)
    {
        temp_f = (float)hmi_data.melter_temp;
    }

    bool machine_running =
        (hmi_data.machine_state_fb == ON ||
         hmi_data.machine_state_fb == TEMP_CUT_ON ||
         hmi_data.machine_state_fb == TEMP_CUT_OFF);

    bool temp_cut_active =
        (hmi_data.machine_state == TEMP_CUT_ON);

    uint16_t power = hmi_data.auto_power_percent_fb;

    if (power > 100U)
    {
        power = 100U;
    }

    if (!machine_running || temp_cut_active)
    {
        power = 0U;
        retained_heat *= 0.70f; // fast decay when cut is active
    }
    else
    {
        if ((float)power > retained_heat)
        {
            retained_heat = (float)power;
        }
        else
        {
            retained_heat *= 0.97f; // slow decay to simulate bottleneck
        }
    }

    /* heating term */
    float heating = retained_heat * 0.05f;

    if (hmi_data.melter_set_temp > 0U)
    {
        float error = (float)hmi_data.melter_set_temp - temp_f;

        if (error < 10.0f)
        {
            float factor = 0.7f + (error / 10.0f) * 0.3f;

            if (factor < 0.7f)
            {
                factor = 0.7f;
            }

            heating *= factor;
        }
    }

    /* cooling term */
    float cooling = 0.0f;
    if (temp_f > (float)MELTER_TEMP_AMBIENT)
    {
        cooling = (temp_f - (float)MELTER_TEMP_AMBIENT) * 0.015f;
    }

    temp_f += heating;
    temp_f -= cooling;

    if (temp_f < (float)MELTER_TEMP_AMBIENT)
    {
        temp_f = (float)MELTER_TEMP_AMBIENT;
    }

    hmi_data.melter_temp = (uint16_t)(temp_f + 0.5f);

    ESP_LOGI(TEMP_SIM,
             "Temp=%.2f Set=%u PowerFb=%u Retained=%.2f Local=%d Fb=%d Heat=%.2f Cool=%.2f",
             temp_f,
             hmi_data.melter_set_temp,
             power,
             retained_heat,
             hmi_data.machine_state,
             hmi_data.machine_state_fb,
             heating,
             cooling);
}
#endif

#ifdef _MACHINE_TEMPERATURE_CNTRL_
void dummy_temp_test_task(void *arg)
{
    if (hmi_data.melter_temp < MELTER_TEMP_AMBIENT)
    {
        hmi_data.melter_temp = MELTER_TEMP_AMBIENT;
    }

    while (1)
    {
        dummy_update_melter_temp();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

void app_modbus_master(void)
{
    // esp_log_level_set(TAG_MODBUS_MASTER, ESP_LOG_NONE);
    esp_log_level_set(TEMP_SIM, ESP_LOG_NONE);
    esp_log_level_set(TAG_CUT_DECISION, ESP_LOG_NONE);
    if (esp_log_level_get("*") != ESP_LOG_NONE)
    {
        esp_log_level_set(TAG_MODBUS_MASTER, ESP_LOG_INFO);
        esp_log_level_set(TAG_TEMP_PID_MBM, ESP_LOG_INFO);
    }

    // temp_pid_init();
    // melter_ai_init_once();
    // modbus_master_temp_ctrl_init();

    uart_config_t uart_config = {
        .baud_rate = MODBUS_1_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};

    uart_driver_install(MODBUS_1_UART_PORT_NUM, MODBUS_1_UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(MODBUS_1_UART_PORT_NUM, &uart_config);
    uart_set_pin(MODBUS_1_UART_PORT_NUM,
                 MODBUS_1_UART_TX_PIN,
                 MODBUS_1_UART_RX_PIN,
                 MODBUS_1_UART_RTS_PIN,
                 UART_PIN_NO_CHANGE);
    uart_set_mode(MODBUS_1_UART_PORT_NUM, UART_MODE_RS485_HALF_DUPLEX);

#ifdef _MACHINE_TEMPERATURE_CNTRL_
    // Read Melter Set Temp from file
    if (file_read_melter_set_temp())
    {
        modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_SENSOR_CARD, hmi_data.melter_set_temp, TEMP_CNTRL_MULTISPAN_CARD_SET_TEMP_ADDR);
    }
#endif

    // // AUTO TUNE START
    // modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_SENSOR_CARD, 1, 62);

    // // AUTO TUNE stop
    // modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_SENSOR_CARD, 0, 62);

    if (s_machine_bin_mutex == NULL)
    {
        s_machine_bin_mutex = xSemaphoreCreateMutex();
    }

    if (s_machine_bin_mutex == NULL)
    {
        ESP_LOGE(TAG_MODBUS_MASTER,
                 "Cannot create local BIN archive mutex");
    }
    else
    {
        esp_err_t active_file_err = modbus_local_bin_ensure_active_file();
        if (active_file_err != ESP_OK)
        {
            ESP_LOGE(TAG_MODBUS_MASTER,
                     "Cannot create/verify active local BIN file: %s",
                     esp_err_to_name(active_file_err));
        }

        if (s_machine_sd_archive_task_handle == NULL)
        {
            BaseType_t archive_task_created = xTaskCreate(
                modbus_local_bin_sd_archive_task,
                "bin_sd_archive",
                modbus_local_bin_sd_archive_task_stack_size_bytes,
                NULL,
                modbus_local_bin_sd_archive_task_priority,
                &s_machine_sd_archive_task_handle);

            if (archive_task_created != pdPASS)
            {
                s_machine_sd_archive_task_handle = NULL;
                ESP_LOGE(TAG_MODBUS_MASTER,
                         "Cannot create local BIN SD archive task");
            }
        }
    }

    xTaskCreate(modbus_master_task, "modbus_master_task", modbus_master_task_stack_size_bytes, NULL, modbus_master_task_priority, NULL);
}
