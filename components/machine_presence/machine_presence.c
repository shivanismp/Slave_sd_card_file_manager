#include "machine_presence.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt.h"


#define MACHINE_PRESENCE_TOPIC_MAX MQTT_MAX_TOPIC_LEN

static const char *TAG = "MACHINE_PRESENCE";
static char s_presence_topic[MACHINE_PRESENCE_TOPIC_MAX];
static char s_online_payload[160];
static char s_offline_payload[160];
static bool s_initialized;

static bool machine_presence_publish(const char *payload)
{
    if (!s_initialized || !mqtt_is_connected())
    {
        return false;
    }

    return mqtt_publish_text(s_presence_topic, payload, 1, 1);
}

// static void machine_presence_connection_changed(bool connected, void *context)
// {
//     (void)context;

//     if (connected)
//     {
//         if (machine_presence_publish(s_online_payload))
//         {
//             ESP_LOGI(TAG, "Published online: %s", s_presence_topic);
//         }
//         else
//         {
//             ESP_LOGW(TAG, "Could not publish online presence");
//         }
//     }
//     else
//     {
//         /* Do not publish here: the network is already unavailable. The broker
//          * publishes the Last Will payload configured during initialization. */
//         ESP_LOGW(TAG, "MQTT disconnected; broker will publish offline on timeout");
//     }
// }



static void machine_presence_connection_changed(
    bool connected,
    void *context)
{
    (void)context;

    if (!connected)
    {
        ESP_LOGW(TAG,
                 "MQTT disconnected; AWS should publish Last Will");
        return;
    }

    int message_id = -1;

    if (!mqtt_publish_text_ex(
            s_presence_topic,
            s_online_payload,
            1,
            1,
            &message_id))
    {
        ESP_LOGE(TAG,
                 "Failed to publish retained online status");
        return;
    }

    ESP_LOGI(TAG,
             "Retained online queued: msg_id=%d",
             message_id);
}


// esp_err_t machine_presence_init(const char *topic_suffix)
// {
//     int written;

//     if (s_initialized)
//     {
//         return ESP_OK;
//     }

//     if (topic_suffix == NULL || topic_suffix[0] == '\0' || mqtt_serial_no[0] == '\0')
//     {
//         return ESP_ERR_INVALID_ARG;
//     }

//     written = snprintf(s_presence_topic, sizeof(s_presence_topic),
//                        "%s/%s", mqtt_serial_no, topic_suffix);
//     if (written < 0 || written >= (int)sizeof(s_presence_topic))
//     {
//         return ESP_ERR_INVALID_SIZE;
//     }

//     snprintf(s_online_payload, sizeof(s_online_payload),
//              "{\"machine_id\":\"%s\",\"status\":\"online\"}",
//              mqtt_serial_no);
//     snprintf(s_offline_payload, sizeof(s_offline_payload),
//              "{\"machine_id\":\"%s\",\"status\":\"offline\"}",
//              mqtt_serial_no);

//     // printf("Presence topic: %s\n", s_presence_topic);
//     // printf("Online payload: %s\n", s_online_payload);
//     // printf("Offline payload: %s\n", s_offline_payload);

//     if (!mqtt_configure_last_will(s_presence_topic, s_offline_payload, 1, 1) ||
//         !mqtt_register_connection_observer(machine_presence_connection_changed, NULL))
//     {
//         return ESP_FAIL;
//     }

//     s_initialized = true;
//     ESP_LOGI(TAG, "Presence topic configured: %s", s_presence_topic);
//     return ESP_OK;
// }

// bool machine_presence_publish_offline(void)
// {
//     return machine_presence_publish(s_offline_payload);
// }



esp_err_t machine_presence_init(const char *topic_suffix)
{
    int written;

    if (s_initialized)
    {
        return ESP_OK;
    }

    if (topic_suffix == NULL ||
        topic_suffix[0] == '\0' ||
        mqtt_serial_no[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(
        s_presence_topic,
        sizeof(s_presence_topic),
        "%s/%s",
        mqtt_serial_no,
        topic_suffix);

    if (written < 0 ||
        written >= (int)sizeof(s_presence_topic))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(
        s_online_payload,
        sizeof(s_online_payload),
        "{\"machine_id\":\"%s\",\"status\":\"online\"}",
        mqtt_serial_no);

    if (written < 0 ||
        written >= (int)sizeof(s_online_payload))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(
        s_offline_payload,
        sizeof(s_offline_payload),
        "{\"machine_id\":\"%s\",\"status\":\"offline\"}",
        mqtt_serial_no);

    if (written < 0 ||
        written >= (int)sizeof(s_offline_payload))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Last Will:
     * QoS 1, retained.
     */
    if (!mqtt_configure_last_will(
            s_presence_topic,
            s_offline_payload,
            1,
            1))
    {
        ESP_LOGE(TAG,
                 "Failed to configure MQTT Last Will");
        return ESP_FAIL;
    }

    if (!mqtt_register_connection_observer(
            machine_presence_connection_changed,
            NULL))
    {
        ESP_LOGE(TAG,
                 "Failed to register MQTT connection observer");
        return ESP_FAIL;
    }

    s_initialized = true;

    ESP_LOGI(TAG,
             "Presence topic configured: %s",
             s_presence_topic);

    return ESP_OK;
}