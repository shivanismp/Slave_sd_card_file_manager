#ifndef MACHINE_PRESENCE_H
#define MACHINE_PRESENCE_H

#include <stdbool.h>
#include "esp_err.h"




/*
 * Creates <mqtt_serial_no>/<topic_suffix> and registers the MQTT presence
 * observer. Call once after mqtt_serial_no has been loaded and before MQTT
 * can be started.
 */
esp_err_t machine_presence_init(const char *topic_suffix);

/* Use before an intentional MQTT stop or controlled shutdown. */
bool machine_presence_publish_offline(void);

#endif
