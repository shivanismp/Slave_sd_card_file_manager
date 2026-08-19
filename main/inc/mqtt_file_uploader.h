#ifndef MQTT_FILE_UPLOADER_H
#define MQTT_FILE_UPLOADER_H

#include "esp_err.h"
#include "sdcard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_FILE_PENDING_DIR SDCARD_BASE_PATH "/PENDING"
#define MQTT_FILE_SENT_DIR    SDCARD_BASE_PATH "/SENT"

/*
 * Start the persistent SD-card upload worker.
 *
 * The worker subscribes to <serial>/FROM_BACKEND, scans PENDING at boot and
 * after every MQTT reconnect, and moves a verified file to SENT only after a
 * FILE_RECEIVED acknowledgement from the backend.
 */
esp_err_t mqtt_file_uploader_start(void);

/* Wake the worker immediately after a new file is finalized in PENDING. */
void mqtt_file_uploader_notify_file_ready(void);

#ifdef __cplusplus
}
#endif

#endif
