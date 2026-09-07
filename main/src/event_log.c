#include "event_log.h"

#include "date_time.h"
#include "littlefs.h"
#include "mqtt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "esp_log_tags.h"

typedef struct
{
    uint32_t next_seq;
    uint32_t next_file_idx;
    uint32_t boot_id;

    bool initialized;
    bool last_time_valid;

    int last_machine_local;
    int last_machine_fb;

    uint16_t last_set_temp;
    uint16_t last_auto_power_fb;
    uint16_t last_auto_power_cmd;
    uint16_t last_power_percent;

    uint16_t last_error_bits;

    bool extrema_valid;

    uint16_t min_line_1_v;
    uint16_t max_line_1_v;
    uint16_t min_line_2_v;
    uint16_t max_line_2_v;
    uint16_t min_line_3_v;
    uint16_t max_line_3_v;
    uint16_t min_avg_v;
    uint16_t max_avg_v;

    uint16_t min_line_1_a;
    uint16_t max_line_1_a;
    uint16_t min_line_2_a;
    uint16_t max_line_2_a;
    uint16_t min_line_3_a;
    uint16_t max_line_3_a;
    uint16_t min_avg_a;
    uint16_t max_avg_a;

    uint16_t min_avg_kw;
    uint16_t max_avg_kw;
    uint16_t min_avg_pf;
    uint16_t max_avg_pf;
    uint16_t min_pwm_freq;
    uint16_t max_pwm_freq;

    bool heat_cycle_active;
    bool setpoint_reached_logged;
    uint32_t heat_cycle_start_ms;
    uint16_t heat_cycle_set_temp;
} event_log_state_t;

static event_log_cfg_t s_cfg;
static event_log_state_t s_st;
static SemaphoreHandle_t s_event_log_mutex = NULL;
static char s_topic_buf[128];

static const char *error_bit_name(uint8_t bit)
{
    switch (bit)
    {
        case 0:  return "NO_LOAD";
        case 1:  return "IGBT_TRIP";
        case 2:  return "WATER_FAIL";
        case 3:  return "OVER_HEAT_COIL";
        case 4:  return "OVER_HEAT_IGBT";
        case 5:  return "EXT_1";
        case 6:  return "EXT_2";
        case 7:  return "TEMP_CUTOFF_INPUT";
        case 8:  return "HF_PT_TRIP";
        case 9:  return "HF_CT_TRIP";
        case 10: return "PHASE_ERROR";
        default: return "UNKNOWN";
    }
}

static const char *machine_state_name(int st)
{
    switch (st)
    {
        case IDLE:         return "IDLE";
        case OFF:          return "OFF";
        case ON:           return "ON";
        case TEMP_CUT_ON:  return "TEMP_CUT_ON";
        case TEMP_CUT_OFF: return "TEMP_CUT_OFF";
        case ERROR:        return "ERROR";
        default:           return "UNKNOWN";
    }
}

static uint32_t uptime_ms_u32(void)
{
    uint64_t us = (uint64_t)esp_timer_get_time();
    return (uint32_t)(us / 1000ULL);
}

static bool event_log_lock(uint32_t timeout_ms)
{
    if (s_event_log_mutex == NULL)
        return false;

    return (xSemaphoreTake(s_event_log_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

static void event_log_unlock(void)
{
    if (s_event_log_mutex != NULL)
        xSemaphoreGive(s_event_log_mutex);
}

static void event_log_default_steps(event_log_cfg_t *cfg)
{
    cfg->voltage_step = 2U;
    cfg->current_step = 1U;
    cfg->power_step   = 1U;
    cfg->pf_step      = 5U;
    cfg->freq_step    = 5U;
    cfg->temp_step    = 5U;
}

void event_log_default_cfg(event_log_cfg_t *cfg)
{
    if (cfg == NULL)
        return;

    memset(cfg, 0, sizeof(*cfg));
    cfg->topic              = NULL;
    cfg->publish_cb         = NULL;
    cfg->rotate_bytes       = EVENT_LOG_DEFAULT_ROTATE_BYTES;
    cfg->total_limit_bytes  = EVENT_LOG_DEFAULT_TOTAL_LIMIT_BYTES;
    cfg->flush_period_ms    = EVENT_LOG_DEFAULT_FLUSH_PERIOD_MS;
    cfg->publish_timeout_ms = EVENT_LOG_DEFAULT_PUBLISH_TIMEOUT_MS;
    event_log_default_steps(cfg);
}

static bool read_meta_file(uint32_t *next_seq,
                           uint32_t *next_file_idx,
                           uint32_t *boot_id)
{
    FILE *fp;
    char line[64];

    if (next_seq == NULL || next_file_idx == NULL || boot_id == NULL)
        return false;

    *next_seq = 1U;
    *next_file_idx = 1U;
    *boot_id = 0U;

    fp = fopen(EVENT_LOG_META_FILE, "r");
    if (fp == NULL)
        return false;

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        uint32_t val = 0U;

        if (sscanf(line, "next_seq=%" SCNu32, &val) == 1)
        {
            *next_seq = val;
            continue;
        }

        if (sscanf(line, "next_file_idx=%" SCNu32, &val) == 1)
        {
            *next_file_idx = val;
            continue;
        }

        if (sscanf(line, "boot_id=%" SCNu32, &val) == 1)
        {
            *boot_id = val;
            continue;
        }
    }

    fclose(fp);
    return true;
}

static esp_err_t write_meta_file(void)
{
    char buf[128];
    int n;

    n = snprintf(buf,
                 sizeof(buf),
                 "next_seq=%" PRIu32 "\nnext_file_idx=%" PRIu32 "\nboot_id=%" PRIu32 "\n",
                 s_st.next_seq,
                 s_st.next_file_idx,
                 s_st.boot_id);

    if (n <= 0 || (size_t)n >= sizeof(buf))
        return ESP_FAIL;

    return littlefs_write_file(EVENT_LOG_META_FILE, buf, (size_t)n);
}

static bool path_size_bytes(const char *path, size_t *out_size)
{
    struct stat st;

    if (out_size == NULL)
        return false;

    *out_size = 0U;

    if (path == NULL)
        return false;

    if (stat(path, &st) != 0)
        return false;

    if (!S_ISREG(st.st_mode))
        return false;

    *out_size = (size_t)st.st_size;
    return true;
}

static bool pending_file_name(uint32_t idx, char *out, size_t out_size)
{
    int n;

    if (out == NULL || out_size < 8U)
        return false;

    n = snprintf(out,
                 out_size,
                 EVENT_LOG_PENDING_DIR "/%010" PRIu32 ".jsonl",
                 idx);

    return (n > 0 && (size_t)n < out_size);
}

static bool find_oldest_pending(char *out_path, size_t out_path_size)
{
    DIR *dir;
    struct dirent *ent;
    char best_name[64] = {0};
    bool found = false;

    if (out_path == NULL || out_path_size == 0U)
        return false;

    dir = opendir(EVENT_LOG_PENDING_DIR);
    if (dir == NULL)
        return false;

    while ((ent = readdir(dir)) != NULL)
    {
        if (ent->d_name[0] == '.')
            continue;

        if (strstr(ent->d_name, ".jsonl") == NULL)
            continue;

        if (!found || strcmp(ent->d_name, best_name) < 0)
        {
            strncpy(best_name, ent->d_name, sizeof(best_name) - 1U);
            best_name[sizeof(best_name) - 1U] = '\0';
            found = true;
        }
    }

    closedir(dir);

    if (!found)
        return false;

    if (snprintf(out_path, out_path_size, EVENT_LOG_PENDING_DIR "/%s", best_name) >= (int)out_path_size)
        return false;

    return true;
}

static size_t total_spool_bytes(void)
{
    size_t total = 0U;
    size_t sz = 0U;
    DIR *dir;
    struct dirent *ent;

    if (path_size_bytes(EVENT_LOG_ACTIVE_FILE, &sz))
        total += sz;

    dir = opendir(EVENT_LOG_PENDING_DIR);
    if (dir == NULL)
        return total;

    while ((ent = readdir(dir)) != NULL)
    {
        char path[256];

        if (ent->d_name[0] == '.')
            continue;

        if (snprintf(path, sizeof(path), EVENT_LOG_PENDING_DIR "/%s", ent->d_name) >= (int)sizeof(path))
            continue;

        if (path_size_bytes(path, &sz))
            total += sz;
    }

    closedir(dir);
    return total;
}

static void prune_if_needed(void)
{
    while (total_spool_bytes() > s_cfg.total_limit_bytes)
    {
        char oldest[256];

        if (!find_oldest_pending(oldest, sizeof(oldest)))
            break;

        ESP_LOGW(TAG_EVENT_LOG, "Pruning oldest pending log file: %s", oldest);
        (void)unlink(oldest);
    }
}

static esp_err_t seal_active_file_if_needed(bool force)
{
    size_t active_size = 0U;
    char dst[256];

    if (!littlefs_exists(EVENT_LOG_ACTIVE_FILE))
        return ESP_OK;

    if (!path_size_bytes(EVENT_LOG_ACTIVE_FILE, &active_size))
        return ESP_OK;

    if (!force && active_size < s_cfg.rotate_bytes)
        return ESP_OK;

    if (active_size == 0U)
    {
        (void)unlink(EVENT_LOG_ACTIVE_FILE);
        return ESP_OK;
    }

    if (!pending_file_name(s_st.next_file_idx, dst, sizeof(dst)))
        return ESP_FAIL;

    (void)unlink(dst);
    if (rename(EVENT_LOG_ACTIVE_FILE, dst) != 0)
    {
        ESP_LOGE(TAG_EVENT_LOG, "rename active->pending failed: errno=%d", errno);
        return ESP_FAIL;
    }

    s_st.next_file_idx++;
    (void)write_meta_file();
    prune_if_needed();
    return ESP_OK;
}

static void recover_active_file(void)
{
    (void)seal_active_file_if_needed(true);
}

static bool append_raw_line(const char *line)
{
    FILE *fp;
    size_t len;

    if (line == NULL || line[0] == '\0')
        return false;

    fp = fopen(EVENT_LOG_ACTIVE_FILE, "ab");
    if (fp == NULL)
    {
        ESP_LOGE(TAG_EVENT_LOG, "Failed to open active log file: errno=%d", errno);
        return false;
    }

    len = strlen(line);
    if (fwrite(line, 1U, len, fp) != len)
    {
        fclose(fp);
        ESP_LOGE(TAG_EVENT_LOG, "Failed to append log line");
        return false;
    }

    if (fflush(fp) != 0)
        ESP_LOGW(TAG_EVENT_LOG, "fflush failed on active log file");

    fclose(fp);
    (void)seal_active_file_if_needed(false);
    prune_if_needed();
    return true;
}

static void append_event_json(const char *type,
                              const char *severity,
                              const char *kv_json)
{
    char line[768];
    time_t now = 0;
    bool time_valid = app_time_is_valid();
    int n;

    if (!s_st.initialized)
        return;

    if (type == NULL)
        type = "GENERIC";

    if (severity == NULL)
        severity = "INFO";

    if (time_valid)
        time(&now);

    if (kv_json == NULL)
        kv_json = "";

    n = snprintf(line,
                 sizeof(line),
                 "{\"seq\":%" PRIu32 ",\"boot_id\":%" PRIu32 ",\"ts\":%lld,\"ts_valid\":%s,\"uptime_ms\":%" PRIu32 ",\"type\":\"%s\",\"severity\":\"%s\"%s%s}\n",
                 s_st.next_seq,
                 s_st.boot_id,
                 (long long)now,
                 time_valid ? "true" : "false",
                 uptime_ms_u32(),
                 type,
                 severity,
                 (kv_json[0] != '\0') ? "," : "",
                 kv_json);

    if (n <= 0 || (size_t)n >= sizeof(line))
        return;

    if (append_raw_line(line))
    {
        s_st.next_seq++;
        (void)write_meta_file();
    }
}

static void log_u16_event(const char *type,
                          const char *severity,
                          const char *metric,
                          const char *extreme,
                          uint16_t value)
{
    char kv[192];
    int n;

    n = snprintf(kv,
                 sizeof(kv),
                 "\"metric\":\"%s\",\"extreme\":\"%s\",\"value\":%u",
                 metric,
                 extreme,
                 (unsigned)value);

    if (n > 0 && (size_t)n < sizeof(kv))
        append_event_json(type, severity, kv);
}

static bool running_state(int st)
{
    return (st == ON || st == TEMP_CUT_ON || st == TEMP_CUT_OFF);
}

static uint16_t error_mask_from_snapshot(const hmi_data_t *snap)
{
    uint16_t mask = 0U;
    uint8_t i;

    if (snap == NULL)
        return 0U;

    for (i = 0U; i < 12U; i++)
    {
        if (snap->error_leds[i] != 0U)
            mask |= (uint16_t)(1U << i);
    }

    return mask;
}

static void maybe_log_extreme_u16(const char *metric,
                                  uint16_t value,
                                  uint16_t *min_v,
                                  uint16_t *max_v,
                                  uint16_t step)
{
    if (min_v == NULL || max_v == NULL)
        return;

    if (value < *min_v && (uint16_t)(*min_v - value) >= step)
    {
        *min_v = value;
        log_u16_event("EXTREME", "WARN", metric, "min", value);
    }

    if (value > *max_v && (uint16_t)(value - *max_v) >= step)
    {
        *max_v = value;
        log_u16_event("EXTREME", "INFO", metric, "max", value);
    }
}

static void seed_extrema_from_snapshot(const hmi_data_t *snap)
{
    if (snap == NULL)
        return;

    s_st.min_line_1_v = s_st.max_line_1_v = snap->line_1_v;
    s_st.min_line_2_v = s_st.max_line_2_v = snap->line_2_v;
    s_st.min_line_3_v = s_st.max_line_3_v = snap->line_3_v;
    s_st.min_avg_v    = s_st.max_avg_v    = snap->avg_v;

    s_st.min_line_1_a = s_st.max_line_1_a = snap->line_1_a;
    s_st.min_line_2_a = s_st.max_line_2_a = snap->line_2_a;
    s_st.min_line_3_a = s_st.max_line_3_a = snap->line_3_a;
    s_st.min_avg_a    = s_st.max_avg_a    = snap->avg_a;

    s_st.min_avg_kw   = s_st.max_avg_kw   = snap->avg_kw;
    s_st.min_avg_pf   = s_st.max_avg_pf   = snap->avg_pf;
    s_st.min_pwm_freq = s_st.max_pwm_freq = snap->pwm_freq;

    s_st.extrema_valid = true;
    append_event_json("METER_BASELINE_CAPTURED", "INFO", "\"message\":\"Initial meter baseline captured\"");
}

static void log_machine_state_changes(const hmi_data_t *snap)
{
    char kv[256];
    int n;

    if (snap == NULL)
        return;

    if ((int)snap->machine_state != s_st.last_machine_local ||
        (int)snap->machine_state_fb != s_st.last_machine_fb)
    {
        n = snprintf(kv,
                     sizeof(kv),
                     "\"local\":%d,\"local_name\":\"%s\",\"fb\":%d,\"fb_name\":\"%s\"",
                     (int)snap->machine_state,
                     machine_state_name((int)snap->machine_state),
                     (int)snap->machine_state_fb,
                     machine_state_name((int)snap->machine_state_fb));

        if (n > 0 && (size_t)n < sizeof(kv))
            append_event_json("STATE_CHANGE", "INFO", kv);

        s_st.last_machine_local = (int)snap->machine_state;
        s_st.last_machine_fb    = (int)snap->machine_state_fb;
    }
}

static void log_set_temp_change(const hmi_data_t *snap)
{
    char kv[160];
    int n;

    if (snap == NULL)
        return;

    if (s_st.last_set_temp == UINT16_MAX)
    {
        s_st.last_set_temp = snap->melter_set_temp;
        return;
    }

    if (snap->melter_set_temp != s_st.last_set_temp)
    {
        n = snprintf(kv,
                     sizeof(kv),
                     "\"prev\":%u,\"next\":%u",
                     (unsigned)s_st.last_set_temp,
                     (unsigned)snap->melter_set_temp);

        if (n > 0 && (size_t)n < sizeof(kv))
            append_event_json("SET_TEMP_CHANGED", "INFO", kv);

        s_st.last_set_temp = snap->melter_set_temp;

        if (running_state((int)snap->machine_state_fb) && snap->melter_set_temp > 0U)
        {
            s_st.heat_cycle_active = true;
            s_st.heat_cycle_start_ms = uptime_ms_u32();
            s_st.heat_cycle_set_temp = snap->melter_set_temp;
            s_st.setpoint_reached_logged = false;
        }
    }
}

static void log_error_edges(const hmi_data_t *snap)
{
    uint16_t mask;
    uint16_t changed;
    uint8_t i;

    if (snap == NULL)
        return;

    mask = error_mask_from_snapshot(snap);
    if (s_st.last_error_bits == UINT16_MAX)
    {
        s_st.last_error_bits = mask;
        return;
    }

    changed = (uint16_t)(mask ^ s_st.last_error_bits);

    if (changed == 0U)
        return;

    for (i = 0U; i < 11U; i++)
    {
        if ((changed & (1U << i)) != 0U)
        {
            char kv[192];
            int n = snprintf(kv,
                             sizeof(kv),
                             "\"bit\":%u,\"name\":\"%s\",\"active\":%s",
                             (unsigned)i,
                             error_bit_name(i),
                             ((mask & (1U << i)) != 0U) ? "true" : "false");

            if (n > 0 && (size_t)n < sizeof(kv))
            {
                append_event_json(((mask & (1U << i)) != 0U) ? "ERROR_ON" : "ERROR_OFF",
                                  ((mask & (1U << i)) != 0U) ? "ERROR" : "INFO",
                                  kv);
            }
        }
    }

    s_st.last_error_bits = mask;
}

static void log_power_command_change(const hmi_data_t *snap)
{
    if (snap == NULL)
        return;

    s_st.last_auto_power_cmd = snap->auto_power_percent;
    s_st.last_auto_power_fb  = snap->auto_power_percent_fb;
    s_st.last_power_percent  = snap->power_percent;
}

static void log_meter_extremes(const hmi_data_t *snap)
{
    if (snap == NULL)
        return;

    if (!s_st.extrema_valid)
    {
        seed_extrema_from_snapshot(snap);
        return;
    }

    maybe_log_extreme_u16("line_1_v", snap->line_1_v, &s_st.min_line_1_v, &s_st.max_line_1_v, s_cfg.voltage_step);
    maybe_log_extreme_u16("line_2_v", snap->line_2_v, &s_st.min_line_2_v, &s_st.max_line_2_v, s_cfg.voltage_step);
    maybe_log_extreme_u16("line_3_v", snap->line_3_v, &s_st.min_line_3_v, &s_st.max_line_3_v, s_cfg.voltage_step);
    maybe_log_extreme_u16("avg_v",    snap->avg_v,    &s_st.min_avg_v,    &s_st.max_avg_v,    s_cfg.voltage_step);

    maybe_log_extreme_u16("line_1_a", snap->line_1_a, &s_st.min_line_1_a, &s_st.max_line_1_a, s_cfg.current_step);
    maybe_log_extreme_u16("line_2_a", snap->line_2_a, &s_st.min_line_2_a, &s_st.max_line_2_a, s_cfg.current_step);
    maybe_log_extreme_u16("line_3_a", snap->line_3_a, &s_st.min_line_3_a, &s_st.max_line_3_a, s_cfg.current_step);
    maybe_log_extreme_u16("avg_a",    snap->avg_a,    &s_st.min_avg_a,    &s_st.max_avg_a,    s_cfg.current_step);

    maybe_log_extreme_u16("avg_kw",   snap->avg_kw,   &s_st.min_avg_kw,   &s_st.max_avg_kw,   s_cfg.power_step);
    maybe_log_extreme_u16("avg_pf",   snap->avg_pf,   &s_st.min_avg_pf,   &s_st.max_avg_pf,   s_cfg.pf_step);
    maybe_log_extreme_u16("pwm_freq", snap->pwm_freq, &s_st.min_pwm_freq, &s_st.max_pwm_freq, s_cfg.freq_step);
}

static void log_time_sync_edge(void)
{
    bool now_valid = app_time_is_valid();

    if (now_valid && !s_st.last_time_valid)
        append_event_json("TIME_SYNCED", "INFO", "\"message\":\"Time became valid\"");

    s_st.last_time_valid = now_valid;
}


static void handle_heat_cycle(const hmi_data_t *snap)
{
    bool running;

    if (snap == NULL)
        return;

    running = running_state((int)snap->machine_state_fb) && (snap->melter_set_temp > 0U);

    if (running && !s_st.heat_cycle_active)
    {
        s_st.heat_cycle_active = true;
        s_st.heat_cycle_start_ms = uptime_ms_u32();
        s_st.heat_cycle_set_temp = snap->melter_set_temp;
        s_st.setpoint_reached_logged = false;

        append_event_json("HEAT_CYCLE_STARTED", "INFO", "\"message\":\"Heat cycle started\"");
    }

    if (!running && s_st.heat_cycle_active)
    {
        char kv[192];
        uint32_t elapsed = uptime_ms_u32() - s_st.heat_cycle_start_ms;
        int n = snprintf(kv,
                         sizeof(kv),
                         "\"duration_ms\":%" PRIu32 ",\"set_temp\":%u,\"reached\":%s",
                         elapsed,
                         (unsigned)s_st.heat_cycle_set_temp,
                         s_st.setpoint_reached_logged ? "true" : "false");

        if (n > 0 && (size_t)n < sizeof(kv))
            append_event_json("HEAT_CYCLE_ENDED", "INFO", kv);

        s_st.heat_cycle_active = false;
        s_st.setpoint_reached_logged = false;
    }

    if (s_st.heat_cycle_active &&
        !s_st.setpoint_reached_logged &&
        snap->melter_set_temp > 0U &&
        snap->melter_temp >= snap->melter_set_temp)
    {
        char kv[256];
        uint32_t elapsed = uptime_ms_u32() - s_st.heat_cycle_start_ms;
        int n = snprintf(kv,
                         sizeof(kv),
                         "\"set_temp\":%u,\"actual_temp\":%u,\"elapsed_ms\":%" PRIu32 ",\"elapsed_s\":%" PRIu32,
                         (unsigned)snap->melter_set_temp,
                         (unsigned)snap->melter_temp,
                         elapsed,
                         elapsed / 1000U);

        if (n > 0 && (size_t)n < sizeof(kv))
            append_event_json("TEMP_SET_REACHED", "INFO", kv);

        s_st.setpoint_reached_logged = true;
    }
}
static bool event_log_publish_via_mqtt(const char *topic,
                                       const char *payload,
                                       size_t payload_len,
                                       uint32_t timeout_ms)
{
    int msg_id = -1;

    (void)payload_len;

    if (topic == NULL || payload == NULL)
        return false;

    if (!mqtt_is_connected())
        return false;

    if (!mqtt_publish_text_ex(topic, payload, 1, 0, &msg_id))
        return false;

    return mqtt_wait_for_published(msg_id, timeout_ms);
}

void event_log_use_default_mqtt_publish(void)
{
    if (!event_log_lock(100U))
        return;

    s_cfg.publish_cb = event_log_publish_via_mqtt;
    event_log_unlock();
}

void event_log_set_default_topic_from_serial(void)
{
    if (!event_log_lock(100U))
        return;

    if (mqtt_serial_no[0] != '\0')
    {
        snprintf(s_topic_buf,
                 sizeof(s_topic_buf),
                 "%s%s",
                 mqtt_serial_no,
                 EVENT_LOG_DEFAULT_TOPIC_SUFFIX);
        s_cfg.topic = s_topic_buf;
    }

    event_log_unlock();
}

esp_err_t event_log_init(const event_log_cfg_t *cfg)
{
    uint32_t next_seq = 1U;
    uint32_t next_file_idx = 1U;
    uint32_t boot_id = 0U;
    const esp_app_desc_t *app;
    char kv[256];
    int n;

    if (cfg == NULL)
        return ESP_ERR_INVALID_ARG;

    if (s_event_log_mutex == NULL)
        s_event_log_mutex = xSemaphoreCreateMutex();

    if (s_event_log_mutex == NULL)
        return ESP_ERR_NO_MEM;

    if (!event_log_lock(200U))
        return ESP_ERR_TIMEOUT;

    s_cfg = *cfg;

    if (s_cfg.rotate_bytes == 0U)
        s_cfg.rotate_bytes = EVENT_LOG_DEFAULT_ROTATE_BYTES;
    if (s_cfg.total_limit_bytes == 0U)
        s_cfg.total_limit_bytes = EVENT_LOG_DEFAULT_TOTAL_LIMIT_BYTES;
    if (s_cfg.flush_period_ms == 0U)
        s_cfg.flush_period_ms = EVENT_LOG_DEFAULT_FLUSH_PERIOD_MS;
    if (s_cfg.publish_timeout_ms == 0U)
        s_cfg.publish_timeout_ms = EVENT_LOG_DEFAULT_PUBLISH_TIMEOUT_MS;

    if (s_cfg.voltage_step == 0U || s_cfg.current_step == 0U ||
        s_cfg.power_step == 0U || s_cfg.pf_step == 0U ||
        s_cfg.freq_step == 0U || s_cfg.temp_step == 0U)
    {
        event_log_default_steps(&s_cfg);
    }

    if (s_cfg.topic == NULL && mqtt_serial_no[0] != '\0')
    {
        snprintf(s_topic_buf,
                 sizeof(s_topic_buf),
                 "%s%s",
                 mqtt_serial_no,
                 EVENT_LOG_DEFAULT_TOPIC_SUFFIX);
        s_cfg.topic = s_topic_buf;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.last_machine_local = -999;
    s_st.last_machine_fb = -999;
    s_st.last_set_temp = UINT16_MAX;
    s_st.last_auto_power_fb = UINT16_MAX;
    s_st.last_auto_power_cmd = UINT16_MAX;
    s_st.last_power_percent = UINT16_MAX;
    s_st.last_error_bits = UINT16_MAX;

    if (littlefs_mkdirs(EVENT_LOG_PENDING_DIR) != ESP_OK)
    {
        event_log_unlock();
        return ESP_FAIL;
    }

    (void)read_meta_file(&next_seq, &next_file_idx, &boot_id);

    s_st.next_seq = (next_seq == 0U) ? 1U : next_seq;
    s_st.next_file_idx = (next_file_idx == 0U) ? 1U : next_file_idx;
    s_st.boot_id = (boot_id != 0U) ? boot_id : esp_random();
    s_st.last_time_valid = app_time_is_valid();
    s_st.initialized = true;

    recover_active_file();
    prune_if_needed();
    (void)write_meta_file();

    app = esp_app_get_description();
    n = snprintf(kv,
                 sizeof(kv),
                 "\"message\":\"logger_init\",\"version\":\"%s\",\"boot_id\":%" PRIu32,
                 (app != NULL) ? app->version : "unknown",
                 s_st.boot_id);
    if (n > 0 && (size_t)n < sizeof(kv))
        append_event_json("LOGGER_INIT", "INFO", kv);

    event_log_unlock();
    return ESP_OK;
}

void event_log_note_text(const char *type,
                         const char *severity,
                         const char *message)
{
    char kv[384];
    int n;

    if (message == NULL)
        return;

    if (!event_log_lock(100U))
        return;

    n = snprintf(kv, sizeof(kv), "\"message\":\"%s\"", message);
    if (n > 0 && (size_t)n < sizeof(kv))
        append_event_json(type, severity, kv);

    event_log_unlock();
}

void event_log_process_snapshot(const hmi_data_t *snap)
{
    if (snap == NULL)
        return;

    if (!event_log_lock(1))
        return;

    if (!s_st.initialized)
    {
        event_log_unlock();
        return;
    }

    log_time_sync_edge();
    log_machine_state_changes(snap);

    log_set_temp_change(snap);

    log_error_edges(snap);
    log_power_command_change(snap);
    log_meter_extremes(snap);

    handle_heat_cycle(snap);
    
    event_log_unlock();
}

static bool event_log_flush_pending_once(void)
{
    char pending[256];
    FILE *fp;
    char line[768];

    if (!event_log_lock(100U))
        return false;

    if (!s_st.initialized)
    {
        event_log_unlock();
        return false;
    }

    if (s_cfg.publish_cb == NULL || s_cfg.topic == NULL || s_cfg.topic[0] == '\0')
    {
        event_log_unlock();
        return false;
    }

    if (s_cfg.publish_cb == event_log_publish_via_mqtt && !mqtt_is_connected())
    {
        event_log_unlock();
        return false;
    }

    /* Force-seal here so fresh events do not wait for rotate_bytes to be reached. */
    (void)seal_active_file_if_needed(true);

    if (!find_oldest_pending(pending, sizeof(pending)))
    {
        event_log_unlock();
        return false;
    }

    fp = fopen(pending, "rb");
    if (fp == NULL)
    {
        event_log_unlock();
        return false;
    }

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        size_t len = strlen(line);

        while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r'))
        {
            line[len - 1U] = '\0';
            len--;
        }

        if (len == 0U)
            continue;

        if (!s_cfg.publish_cb(s_cfg.topic,
                              line,
                              len,
                              s_cfg.publish_timeout_ms))
        {
            fclose(fp);
            event_log_unlock();
            return false;
        }
    }

    fclose(fp);
    (void)unlink(pending);
    event_log_unlock();
    return true;
}

void event_log_task(void *arg)
{
    (void)arg;

    while (1)
    {
        (void)event_log_flush_pending_once();
        vTaskDelay(pdMS_TO_TICKS(s_cfg.flush_period_ms));
    }
}
