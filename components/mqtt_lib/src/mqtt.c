#include <stdatomic.h>
#include "mqtt.h"

#include <esp_app_desc.h>

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "ethernet.h"

static const char *TAG_MQTT = "MQTT_AWS";

#define MQTT_MODBUS_SLAVE_ID 95U

static char *s_ota_rx_buf = NULL;
static int s_ota_rx_total = 0;

static bool s_ota_rx_active = false;

char mqtt_topic_nx_update_cmd[128];
char mqtt_topic_nx_update_status[128];

static char *s_nx_rx_buf = NULL;
static int s_nx_rx_total = 0;
static bool s_nx_rx_active = false;

mqtt_modbus_slave_t mqtt_modbus_slave;

char mqtt_aws_endpoint[128];
char mqtt_serial_no[20];

char aws_root_ca[4096];
char hmi_card_test_cert[4096];
char hmi_card_test_private[4096];

char mqtt_topic_mbm_req[128];
char mqtt_topic_mbm_res[128];

char mqtt_topic_ota_cmd[128];
char mqtt_topic_ota_status[128];

mqtt_topic_list_t g_mqtt_topics;

static EventGroupHandle_t s_mqtt_ev = NULL;
static const int MQTT_CONNECTED_BIT = BIT0;
static esp_mqtt_client_handle_t s_client = NULL;

/* Cross-task connection counter: does not take the presence observer slot. */
static _Atomic uint32_t s_connection_generation = 0;
uint32_t mqtt_connection_generation(void)
{
    return atomic_load_explicit(&s_connection_generation, memory_order_relaxed);
}


#define MQTT_PUBLISHED_RING_SIZE 32

static SemaphoreHandle_t s_puback_mutex = NULL;
static int s_published_ids[MQTT_PUBLISHED_RING_SIZE];
static size_t s_published_head = 0;
static size_t s_published_count = 0;

static char s_data_observer_topic[MQTT_MAX_TOPIC_LEN];
static mqtt_data_observer_t s_data_observer = NULL;
static void *s_data_observer_context = NULL;
static mqtt_connection_observer_t s_connection_observer = NULL;
static void *s_connection_observer_context = NULL;
static mqtt_update_busy_fn_t s_ota_busy = NULL;
static mqtt_update_request_fn_t s_ota_request = NULL;
static mqtt_update_busy_fn_t s_nextion_busy = NULL;
static mqtt_update_request_fn_t s_nextion_request = NULL;

static char s_last_will_topic[MQTT_MAX_TOPIC_LEN];
static char s_last_will_payload[160];
static int s_last_will_qos = 1;
static int s_last_will_retain = 1;

static void mqtt_record_published_msg_id(int msg_id);
static bool mqtt_consume_published_msg_id(int msg_id);

static void mqtt_handle_modbus_request(esp_mqtt_event_handle_t event);
static void mqtt_handle_ota_request(esp_mqtt_event_handle_t event);

static void mqtt_handle_nx_update_request(esp_mqtt_event_handle_t event);

static void mqtt_handle_nx_update_request(esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0)
    {
        if (event->topic_len != strlen(mqtt_topic_nx_update_cmd) ||
            strncmp(event->topic, mqtt_topic_nx_update_cmd, event->topic_len) != 0)
        {
            return;
        }

        free(s_nx_rx_buf);
        s_nx_rx_buf = calloc(1, event->total_data_len + 1);
        if (s_nx_rx_buf == NULL)
        {
            s_nx_rx_active = false;
            s_nx_rx_total = 0;
            ESP_LOGE(TAG_MQTT, "NX RX alloc failed");
            return;
        }

        s_nx_rx_active = true;
        s_nx_rx_total = event->total_data_len;
    }
    else
    {
        if (!s_nx_rx_active || s_nx_rx_buf == NULL)
        {
            return;
        }
    }

    memcpy(s_nx_rx_buf + event->current_data_offset, event->data, event->data_len);

    int rx_end = event->current_data_offset + event->data_len;

    if (rx_end < s_nx_rx_total)
    {
        ESP_LOGI(TAG_MQTT, "NX JSON chunk %d/%d", rx_end, s_nx_rx_total);
        return;
    }

    s_nx_rx_buf[s_nx_rx_total] = '\0';
    ESP_LOGI(TAG_MQTT, "NX JSON full len=%d", s_nx_rx_total);

    if (s_nextion_busy != NULL && s_nextion_busy())
    {
        ESP_LOGW(TAG_MQTT, "Ignoring NX JSON: update already in progress");
    }
    else if (s_nextion_request == NULL ||
             !s_nextion_request(s_nx_rx_buf, (size_t)s_nx_rx_total))
    {
        ESP_LOGE(TAG_MQTT, "Invalid NX update JSON request");
    }

    free(s_nx_rx_buf);
    s_nx_rx_buf = NULL;
    s_nx_rx_active = false;
    s_nx_rx_total = 0;
}

bool mqtt_is_started(void)
{
    return (s_client != NULL);
}

bool mqtt_is_connected(void)
{
    if (s_mqtt_ev == NULL)
        return false;

    return (xEventGroupGetBits(s_mqtt_ev) & MQTT_CONNECTED_BIT) != 0;
}

void mqtt_force_reconnect(void)
{
    if (s_client)
    {
        esp_mqtt_client_reconnect(s_client);
    }
}

static void mqtt_record_published_msg_id(int msg_id)
{
    if (s_puback_mutex == NULL)
        return;

    if (xSemaphoreTake(s_puback_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return;

    s_published_ids[s_published_head] = msg_id;
    s_published_head = (s_published_head + 1U) % MQTT_PUBLISHED_RING_SIZE;

    if (s_published_count < MQTT_PUBLISHED_RING_SIZE)
        s_published_count++;

    xSemaphoreGive(s_puback_mutex);
}

static bool mqtt_consume_published_msg_id(int msg_id)
{
    size_t start;
    size_t i;

    if (s_puback_mutex == NULL)
        return false;

    if (xSemaphoreTake(s_puback_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;

    start = (s_published_head + MQTT_PUBLISHED_RING_SIZE - s_published_count) % MQTT_PUBLISHED_RING_SIZE;

    for (i = 0; i < s_published_count; i++)
    {
        size_t idx = (start + i) % MQTT_PUBLISHED_RING_SIZE;

        if (s_published_ids[idx] == msg_id)
        {
            size_t j;

            for (j = i; j + 1U < s_published_count; j++)
            {
                size_t from = (start + j + 1U) % MQTT_PUBLISHED_RING_SIZE;
                size_t to = (start + j) % MQTT_PUBLISHED_RING_SIZE;
                s_published_ids[to] = s_published_ids[from];
            }

            s_published_head = (start + s_published_count - 1U) % MQTT_PUBLISHED_RING_SIZE;
            s_published_count--;
            xSemaphoreGive(s_puback_mutex);
            return true;
        }
    }

    xSemaphoreGive(s_puback_mutex);
    return false;
}

bool mqtt_publish_text_ex(const char *topic, const char *payload, int qos, int retain, int *out_msg_id)
{
    if (payload == NULL)
        return false;

    return mqtt_publish_binary_ex(topic,
                                  payload,
                                  strlen(payload),
                                  qos,
                                  retain,
                                  out_msg_id);
}

bool mqtt_publish_text(const char *topic, const char *payload, int qos, int retain)
{
    return mqtt_publish_text_ex(topic, payload, qos, retain, NULL);
}

bool mqtt_configure_last_will(const char *topic,
                              const char *payload,
                              int qos,
                              int retain)
{
    if (s_client != NULL || topic == NULL || payload == NULL ||
        topic[0] == '\0' || payload[0] == '\0' ||
        strlen(topic) >= sizeof(s_last_will_topic) ||
        strlen(payload) >= sizeof(s_last_will_payload) ||
        qos < 0 || qos > 2)
    {
        return false;
    }

    strlcpy(s_last_will_topic, topic, sizeof(s_last_will_topic));
    strlcpy(s_last_will_payload, payload, sizeof(s_last_will_payload));
    s_last_will_qos = qos;
    s_last_will_retain = (retain != 0) ? 1 : 0;
    return true;
}

bool mqtt_register_connection_observer(mqtt_connection_observer_t observer,
                                       void *context)
{
    if (observer == NULL || s_connection_observer != NULL)
    {
        return false;
    }

    s_connection_observer = observer;
    s_connection_observer_context = context;
    return true;
}

bool mqtt_publish_binary_ex(const char *topic,
                            const void *payload,
                            size_t payload_len,
                            int qos,
                            int retain,
                            int *out_msg_id)
{
    int msg_id;

    if (out_msg_id != NULL)
        *out_msg_id = -1;

    if (s_client == NULL || topic == NULL || payload == NULL ||
        payload_len == 0U || payload_len > (size_t)INT_MAX)
    {
        return false;
    }

    msg_id = esp_mqtt_client_enqueue(s_client,
                                     topic,
                                     payload,
                                     (int)payload_len,
                                     qos,
                                     retain,
                                     true);

    if (out_msg_id != NULL)
        *out_msg_id = msg_id;

    return (msg_id >= 0);
}

bool mqtt_register_data_observer(const char *topic,
                                 mqtt_data_observer_t observer,
                                 void *context)
{
    if (topic == NULL || observer == NULL || topic[0] == '\0' ||
        strlen(topic) >= sizeof(s_data_observer_topic))
    {
        return false;
    }

    strlcpy(s_data_observer_topic,
            topic,
            sizeof(s_data_observer_topic));
    s_data_observer = observer;
    s_data_observer_context = context;

    if (s_client != NULL && mqtt_is_connected())
    {
        return esp_mqtt_client_subscribe(s_client,
                                         s_data_observer_topic,
                                         1) >= 0;
    }

    return true;
}

void mqtt_register_update_handlers(
    mqtt_update_busy_fn_t ota_busy,
    mqtt_update_request_fn_t ota_request,
    mqtt_update_busy_fn_t nextion_busy,
    mqtt_update_request_fn_t nextion_request)
{
    s_ota_busy = ota_busy;
    s_ota_request = ota_request;
    s_nextion_busy = nextion_busy;
    s_nextion_request = nextion_request;
}

bool mqtt_wait_for_published(int msg_id, uint32_t timeout_ms)
{
    TickType_t start_tick;
    TickType_t timeout_tick;

    if (msg_id < 0)
        return false;

    if (mqtt_consume_published_msg_id(msg_id))
        return true;

    start_tick = xTaskGetTickCount();
    timeout_tick = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_tick) <= timeout_tick)
    {
        if (mqtt_consume_published_msg_id(msg_id))
            return true;

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return false;
}

static void mqtt_handle_modbus_request(esp_mqtt_event_handle_t event)
{
    uint8_t tx[256];
    int tx_len;

    /* Check MQTT topic */
    if (event->topic_len != strlen(mqtt_topic_mbm_req) ||
        strncmp(event->topic,
                mqtt_topic_mbm_req,
                event->topic_len) != 0)
    {
        return;
    }

    /* Minimum Modbus RTU frame:
       Slave ID + Function + CRC low + CRC high */
    if (event->data_len < 4)
    {
        ESP_LOGW(TAG_MQTT, "MQTT MODBUS: frame too short: %d bytes", event->data_len);
        return;
    }

    const uint8_t *rx = (const uint8_t *)event->data;
    const uint8_t received_slave_id = rx[0];

    /*
     * Reject frames addressed to another Modbus slave.
     * Slave 0 is Modbus broadcast, which must not generate a response.
     */
    if (received_slave_id != MQTT_MODBUS_SLAVE_ID)
    {
        if (received_slave_id == 0)
        {
            ESP_LOGW(TAG_MQTT, "MQTT MODBUS: broadcast frame received; no response");
        }
        else
        {
            ESP_LOGI(TAG_MQTT,
                     "MQTT MODBUS: ignoring slave %u; this device is slave %u",
                     received_slave_id,
                     MQTT_MODBUS_SLAVE_ID);
        }

        return;
    }

    tx_len = mqtt_modbus_slave.modbus_slave_process_frame(
        rx,
        (uint16_t)event->data_len,
        tx);

    if (tx_len <= 0)
    {
        return;
    }

    int msg_id = esp_mqtt_client_publish(
        event->client,
        mqtt_topic_mbm_res,
        (const char *)tx,
        tx_len,
        1,
        0);

    (void)msg_id;
}

static void mqtt_handle_ota_request(esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0)
    {
        if (event->topic_len != strlen(mqtt_topic_ota_cmd) ||
            strncmp(event->topic, mqtt_topic_ota_cmd, event->topic_len) != 0)
        {
            return;
        }

        free(s_ota_rx_buf);
        s_ota_rx_buf = calloc(1, event->total_data_len + 1);
        if (s_ota_rx_buf == NULL)
        {
            s_ota_rx_active = false;
            s_ota_rx_total = 0;
            ESP_LOGE(TAG_MQTT, "OTA RX alloc failed");
            return;
        }

        s_ota_rx_active = true;
        s_ota_rx_total = event->total_data_len;
    }
    else
    {
        if (!s_ota_rx_active || s_ota_rx_buf == NULL)
        {
            return;
        }
    }

    memcpy(s_ota_rx_buf + event->current_data_offset, event->data, event->data_len);

    int rx_end = event->current_data_offset + event->data_len;

    if (rx_end < s_ota_rx_total)
    {
        ESP_LOGI(TAG_MQTT, "OTA JSON chunk %d/%d", rx_end, s_ota_rx_total);
        return;
    }

    s_ota_rx_buf[s_ota_rx_total] = '\0';
    ESP_LOGI(TAG_MQTT, "OTA JSON full len=%d", s_ota_rx_total);

    if (s_ota_busy != NULL && s_ota_busy())
    {
        ESP_LOGW(TAG_MQTT, "Ignoring OTA JSON: OTA already in progress");
    }
    else if (s_ota_request == NULL ||
             !s_ota_request(s_ota_rx_buf, (size_t)s_ota_rx_total))
    {
        ESP_LOGE(TAG_MQTT, "Invalid OTA JSON request");
    }

    free(s_ota_rx_buf);
    s_ota_rx_buf = NULL;
    s_ota_rx_active = false;
    s_ota_rx_total = 0;
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG_MQTT, "MQTT_EVENT_BEFORE_CONNECT");
        break;

    case MQTT_EVENT_CONNECTED:
    {
        atomic_fetch_add_explicit(&s_connection_generation, 1U, memory_order_relaxed);
        ESP_LOGI(TAG_MQTT, "AWS IoT MQTT connected");

        // char payload[64];
        // snprintf(payload, sizeof(payload), "{\"state\":\"online\",\"version\":\"%s\"}", esp_app_get_description()->version);

        // esp_mqtt_client_publish(event->client,
        //                          "CSLD94MTZ2/MACHINE_STATUS",
        //                          payload,
        //                          strlen(payload),
        //                          1,
        //                          0);


        for (size_t i = 0; i < g_mqtt_topics.count; i++)
        {
            if (g_mqtt_topics.topics[i][0] == '\0')
                continue;

            esp_mqtt_client_subscribe(event->client, g_mqtt_topics.topics[i], 1);
            ESP_LOGI(TAG_MQTT, "Subscribed: %s", g_mqtt_topics.topics[i]);
        }

        // if (mqtt_topic_ota_cmd[0] != '\0')
        // {
        //     esp_mqtt_client_subscribe(event->client, mqtt_topic_ota_cmd, 1);
        //     ESP_LOGI(TAG_MQTT, "Subscribed OTA CMD: %s", mqtt_topic_ota_cmd);
        // }

        // if (mqtt_topic_ota_status[0] != '\0')
        // {
        //     char online_payload[160];
        //     snprintf(online_payload, sizeof(online_payload),
        //              "{\"state\":\"online\",\"version\":\"%s\"}",
        //              esp_app_get_description()->version);

        //     mqtt_publish_text(mqtt_topic_ota_status, online_payload, 1, 0);
        // }

        // if (mqtt_topic_nx_update_cmd[0] != '\0')
        // {
        //     esp_mqtt_client_subscribe(event->client, mqtt_topic_nx_update_cmd, 1);
        //     ESP_LOGI(TAG_MQTT, "Subscribed NX CMD: %s", mqtt_topic_nx_update_cmd);
        // }

        // if (mqtt_topic_nx_update_status[0] != '\0')
        // {
        //     char nx_online_payload[160];
        //     snprintf(nx_online_payload, sizeof(nx_online_payload),
        //              "{\"state\":\"online\",\"version\":\"ready\"}");

        //     mqtt_publish_text(mqtt_topic_nx_update_status, nx_online_payload, 1, 0);
        // }

        if (s_data_observer != NULL && s_data_observer_topic[0] != '\0')
        {
            esp_mqtt_client_subscribe(event->client,
                                      s_data_observer_topic,
                                      1);
            ESP_LOGI(TAG_MQTT,
                     "Subscribed application topic: %s",
                     s_data_observer_topic);
        }

        /* Publish/worker tasks may proceed after all subscriptions are sent. */
        xEventGroupSetBits(s_mqtt_ev, MQTT_CONNECTED_BIT);

        if (s_connection_observer != NULL)
        {
            s_connection_observer(true, s_connection_observer_context);
        }

        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG_MQTT, "AWS IoT MQTT disconnected");
        xEventGroupClearBits(s_mqtt_ev, MQTT_CONNECTED_BIT);

        if (s_connection_observer != NULL)
        {
            s_connection_observer(false, s_connection_observer_context);
        }
        break;

    // case MQTT_EVENT_DISCONNECTED:
    // ESP_LOGE(TAG_MQTT,
    //          "MQTT disconnected: ethernet_link=%d ethernet_ip=%d",
    //          ethernet_is_link_up(),
    //          ethernet_has_ip());

    // xEventGroupClearBits(s_mqtt_ev, MQTT_CONNECTED_BIT);

    // if (s_connection_observer != NULL)
    // {
    //     s_connection_observer(false, s_connection_observer_context);
    // }
    // break;

    case MQTT_EVENT_ERROR:
    {
        ESP_LOGE(TAG_MQTT, "MQTT_EVENT_ERROR");

        if (event->error_handle)
        {
            ESP_LOGE(TAG_MQTT, "esp_tls_last_esp_err=0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG_MQTT, "esp_tls_stack_err=0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGE(TAG_MQTT, "esp_transport_sock_errno=%d (%s)",
                     event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    }

    case MQTT_EVENT_DATA:
        // printf("MQTT_EVENT_DATA: topic=%.*s, data=%.*s\n",
        //        event->topic_len, event->topic,
        //        event->data_len, event->data);
               
        mqtt_handle_modbus_request(event);
        mqtt_handle_ota_request(event);
        mqtt_handle_nx_update_request(event);

        if (s_data_observer != NULL)
        {
            s_data_observer(event->topic,
                            event->topic_len,
                            (const uint8_t *)event->data,
                            event->data_len,
                            event->total_data_len,
                            event->current_data_offset,
                            s_data_observer_context);
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG_MQTT, "MQTT_EVENT_PUBLISHED msg_id=%d", event->msg_id);
        mqtt_record_published_msg_id(event->msg_id);
        break;

    default:
        break;
    }
}

void mqtt_start_tls(void)
{
    if (s_client != NULL)
    {
        ESP_LOGW(TAG_MQTT, "MQTT already started");
        return;
    }

    if (s_mqtt_ev == NULL)
    {
        s_mqtt_ev = xEventGroupCreate();
        configASSERT(s_mqtt_ev != NULL);
    }

    if (s_puback_mutex == NULL)
    {
        s_puback_mutex = xSemaphoreCreateMutex();
        configASSERT(s_puback_mutex != NULL);
    }

    // if (mqtt_topic_ota_cmd[0] == '\0')
    // {
    //     snprintf(mqtt_topic_ota_cmd, sizeof(mqtt_topic_ota_cmd),
    //              "%s/OTA_CMD", mqtt_serial_no);
    // }

    // if (mqtt_topic_ota_status[0] == '\0')
    // {
    //     snprintf(mqtt_topic_ota_status, sizeof(mqtt_topic_ota_status),
    //              "%s/OTA_STATUS", mqtt_serial_no);
    // }

    // if (mqtt_topic_nx_update_cmd[0] == '\0')
    // {
    //     snprintf(mqtt_topic_nx_update_cmd, sizeof(mqtt_topic_nx_update_cmd),
    //              "%s/NX_UPDATE_CMD", mqtt_serial_no);
    // }

    // if (mqtt_topic_nx_update_status[0] == '\0')
    // {
    //     snprintf(mqtt_topic_nx_update_status, sizeof(mqtt_topic_nx_update_status),
    //              "%s/NX_UPDATE_STATUS", mqtt_serial_no);
    // }

    char uri[160];
    snprintf(uri, sizeof(uri), "mqtts://%s:8883", mqtt_aws_endpoint);

    // char payload[64];
    // snprintf(payload, sizeof(payload), "{\"state\":\"offline\",\"version\":\"%s\"}", 
    //                                             esp_app_get_description()->version);

    

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,

        .network.disable_auto_reconnect = false,
        .network.reconnect_timeout_ms = 5000,
        // .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 10000,

        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        // .session.keepalive = 60,
        .session.keepalive = 15,
        .session.disable_clean_session = true,
        .session.message_retransmit_timeout = 5000,
        .session.last_will.topic = (s_last_will_topic[0] != '\0') ? s_last_will_topic : NULL,
        .session.last_will.msg = (s_last_will_payload[0] != '\0') ? s_last_will_payload : NULL,
        // .session.last_will.topic = "CSLD94MTZ2/MACHINE_STATUS",
        // .session.last_will.msg = (const char *)payload,
        // .session.last_will.msg_len = (int)strlen(payload),
        .session.last_will.msg_len = 0,
        .session.last_will.qos = s_last_will_qos,
        .session.last_will.retain = s_last_will_retain,

        // .session.last_will.qos = 1,
        // .session.last_will.retain = 1,
        // .session.last_will.retain = 0,

        .credentials.client_id = mqtt_serial_no,
        // .credentials.client_id = "bfdgfkgsdklgflksdglkwdgslkglwdglkj5",

        .broker.verification.certificate = aws_root_ca,

        .credentials.authentication.certificate = hmi_card_test_cert,
        .credentials.authentication.key = hmi_card_test_private,
    };

    s_client = esp_mqtt_client_init(&cfg);
    configASSERT(s_client != NULL);

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
}
