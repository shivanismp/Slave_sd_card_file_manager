#ifndef __MQTT_H__
#define __MQTT_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MQTT_MAX_TOPICS        20
#define MQTT_MAX_TOPIC_LEN     128


typedef struct
{
    int (*modbus_slave_process_frame)(const uint8_t *rx, uint16_t rx_len, uint8_t *tx);
} mqtt_modbus_slave_t;

extern mqtt_modbus_slave_t mqtt_modbus_slave;

typedef struct
{
    char topics[MQTT_MAX_TOPICS][MQTT_MAX_TOPIC_LEN];
    int count;
} mqtt_topic_list_t;
extern mqtt_topic_list_t g_mqtt_topics;

/*
 * Optional application-level MQTT data observer.
 *
 * The callback runs in the MQTT event task and therefore must never block.
 * Fragment offsets are provided so the application can assemble a small
 * control message without assuming that it arrives in one MQTT event.
 */
typedef void (*mqtt_data_observer_t)(const char *topic,
                                     int topic_len,
                                     const uint8_t *data,
                                     int data_len,
                                     int total_data_len,
                                     int current_data_offset,
                                     void *context);

typedef bool (*mqtt_update_busy_fn_t)(void);
typedef bool (*mqtt_update_request_fn_t)(const char *json, size_t len);

extern char mqtt_aws_endpoint[128];
extern char mqtt_serial_no[20];

extern char aws_root_ca[4096];
extern char hmi_card_test_cert[4096];
extern char hmi_card_test_private[4096];

extern char mqtt_topic_mbm_req[128];
extern char mqtt_topic_mbm_res[128];

extern char mqtt_topic_ota_cmd[128];
extern char mqtt_topic_ota_status[128];

void mqtt_start_tls(void);
bool mqtt_is_started(void);
bool mqtt_is_connected(void);
void mqtt_force_reconnect(void);
bool mqtt_publish_text(const char *topic, const char *payload, int qos, int retain);

/*
 * Extended publish helpers for workflows that must wait for broker ACK.
 * Returns false if the message was not accepted into the client outbox.
 */
bool mqtt_publish_text_ex(const char *topic,
                          const char *payload,
                          int qos,
                          int retain,
                          int *out_msg_id);

/* Publish an arbitrary binary payload, including embedded zero bytes. */
bool mqtt_publish_binary_ex(const char *topic,
                            const void *payload,
                            size_t payload_len,
                            int qos,
                            int retain,
                            int *out_msg_id);

/*
 * Register one application observer and subscribe it on every reconnect.
 * Register before mqtt_start_tls() whenever possible.
 */
bool mqtt_register_data_observer(const char *topic,
                                 mqtt_data_observer_t observer,
                                 void *context);

void mqtt_register_update_handlers(
    mqtt_update_busy_fn_t ota_busy,
    mqtt_update_request_fn_t ota_request,
    mqtt_update_busy_fn_t nextion_busy,
    mqtt_update_request_fn_t nextion_request);

/*
 * Wait until MQTT_EVENT_PUBLISHED for the given msg_id.
 * Best used with QoS 1.
 */
bool mqtt_wait_for_published(int msg_id, uint32_t timeout_ms);

extern char mqtt_topic_nx_update_cmd[128];
extern char mqtt_topic_nx_update_status[128];

#endif
