#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include "global.h"
#include "hmi.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_LOG_PENDING_DIR              "/littlefs/log/pending"
#define EVENT_LOG_ACTIVE_FILE              "/littlefs/log/active.jsonl"
#define EVENT_LOG_META_FILE                "/littlefs/log/meta.txt"

#define EVENT_LOG_DEFAULT_TOPIC_SUFFIX     "/EVENT/LOG"
#define EVENT_LOG_DEFAULT_ROTATE_BYTES     (8U * 1024U)
#define EVENT_LOG_DEFAULT_TOTAL_LIMIT_BYTES (256U * 1024U)
#define EVENT_LOG_DEFAULT_FLUSH_PERIOD_MS  3000U
#define EVENT_LOG_DEFAULT_PUBLISH_TIMEOUT_MS 2000U

typedef bool (*event_log_publish_cb_t)(const char *topic,
                                       const char *payload,
                                       size_t payload_len,
                                       uint32_t timeout_ms);

typedef struct
{
    const char *topic;
    event_log_publish_cb_t publish_cb;

    size_t rotate_bytes;
    size_t total_limit_bytes;

    uint32_t flush_period_ms;
    uint32_t publish_timeout_ms;

    uint16_t voltage_step;
    uint16_t current_step;
    uint16_t power_step;
    uint16_t pf_step;
    uint16_t freq_step;
    uint16_t temp_step;
} event_log_cfg_t;

void event_log_default_cfg(event_log_cfg_t *cfg);
esp_err_t event_log_init(const event_log_cfg_t *cfg);

/* Convenience helpers for your existing mqtt.h API */
void event_log_use_default_mqtt_publish(void);
void event_log_set_default_topic_from_serial(void);

void event_log_process_snapshot(const hmi_data_t *snap);
void event_log_note_text(const char *type,
                         const char *severity,
                         const char *message);

void event_log_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_LOG_H */
