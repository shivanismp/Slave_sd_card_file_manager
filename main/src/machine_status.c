#include "machine_status.h"
#include "modbus_slave.h"
#include "mqtt.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STATUS_DATA_BYTES = MQTT_INP_REGISTER_COUNT * 2U,
    STATUS_FRAME_BYTES = STATUS_DATA_BYTES + 5U
};
_Static_assert(MQTT_INP_REGISTER_COUNT == 39U, "Backend expects 39 registers");
_Static_assert(INP_ADDR_MACHINE_STATE_FB == 0U, "Backend state address changed");
_Static_assert(INP_ADDR_ERROR_BITS_LO == 14U, "Backend error address changed");
_Static_assert(STATUS_FRAME_BYTES == 83U, "Backend expects an 83-byte RTU frame");

/* Owned exclusively by the Modbus polling task. */
static bool last_valid;
static uint16_t last_state, last_errors;
static uint32_t last_generation;
static char last_serial[sizeof(mqtt_serial_no)];

static uint16_t read_word(const uint8_t *data, size_t address)
{
    return (uint16_t)(((uint16_t)data[2U * address] << 8) | data[2U * address + 1U]);
}

static uint16_t status_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
    }
    return crc;
}

void machine_status_publish_if_changed(const uint8_t *register_data, size_t length)
{
    if (register_data == NULL || length != STATUS_DATA_BYTES || !mqtt_is_connected())
        return;

    const uint16_t state = read_word(register_data, INP_ADDR_MACHINE_STATE_FB);
    const uint16_t errors = read_word(register_data, INP_ADDR_ERROR_BITS_LO);
    const uint32_t generation = mqtt_connection_generation();
    char serial[sizeof(mqtt_serial_no)];
    memcpy(serial, mqtt_serial_no, sizeof(serial));
    if (!memchr(serial, '\0', sizeof(serial)) || !serial[0] ||
        strpbrk(serial, "/+#") != NULL) return;

    if (last_valid && last_state == state && last_errors == errors &&
        last_generation == generation && strcmp(last_serial, serial) == 0)
        return;

    char topic[MQTT_MAX_TOPIC_LEN];
    int written = snprintf(topic, sizeof(topic), "%s/MBM/STATUS", serial);
    if (written < 0 || (size_t)written >= sizeof(topic)) return;

    uint8_t frame[STATUS_FRAME_BYTES];
    frame[0] = MODBUS_MQTT_SLAVE_ID;          /* 95 decimal / 0x5F */
    frame[1] = 0x04U;                       /* Read input registers response */
    frame[2] = STATUS_DATA_BYTES;            /* 78 / 0x4E */
    memcpy(&frame[3], register_data, STATUS_DATA_BYTES);
    uint16_t crc = status_crc16(frame, STATUS_FRAME_BYTES - 2U);
    frame[STATUS_FRAME_BYTES - 2U] = (uint8_t)crc;
    frame[STATUS_FRAME_BYTES - 1U] = (uint8_t)(crc >> 8);

    /* Binary length is mandatory: frame data includes zero bytes.
     * The existing helper copies the payload into ESP-MQTT's outbox.
     * QoS1 + retain1 preserves the last known complete snapshot.
     */
    if (mqtt_publish_binary_ex(topic, frame, sizeof(frame), 1, 1, NULL)) {
        last_valid = true;
        last_state = state;
        last_errors = errors;
        last_generation = generation;
        memcpy(last_serial, serial, sizeof(last_serial));
    }
    /* Rejected enqueue: leave baseline intact and retry current data next poll. */
}
