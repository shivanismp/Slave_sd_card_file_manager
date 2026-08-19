#ifndef __FILE_IO_H__
#define __FILE_IO_H__

#include "global.h"
#include "mqtt.h"

#define MQTT_ROOT_CA_FILE_PATH                      "/littlefs/mqtt/certs/AmazonRootCA1.pem"
#define MQTT_ROOT_CA_FILE_BACKUP_PATH               "/littlefs/mqtt/certs/AmazonRootCA1.bak"

#define MQTT_DEVICE_CERT_FILE_PATH                  "/littlefs/mqtt/certs/device.crt"
#define MQTT_DEVICE_CERT_FILE_BACKUP_PATH           "/littlefs/mqtt/certs/device.bak"

#define MQTT_PRIVATE_KEY_FILE_PATH                  "/littlefs/mqtt/certs/private.key"
#define MQTT_PRIVATE_KEY_FILE_BACKUP_PATH           "/littlefs/mqtt/certs/private.bak"

/* Machine Info */
#define MACHINE_INFO_FILE_PATH          "/littlefs/machine_info.txt"

/* MQTT serial / client_id */
#define MQTT_SERIAL_FILE_PATH          "/littlefs/mqtt/mqtt_ser.txt"
#define MQTT_SERIAL_FILE_BACKUP_PATH   "/littlefs/mqtt/mqtt_ser.bak"
#define MQTT_SERIAL_GEN_LEN            10
#define MQTT_SERIAL_MAX_LEN            (MQTT_SERIAL_GEN_LEN + 1)

/* AWS endpoint */
#define MQTT_AWS_ENDPOINT_FILE_PATH         "/littlefs/mqtt/mqtt_aws_endpoint.txt"
#define MQTT_AWS_ENDPOINT_FILE_BACKUP_PATH  "/littlefs/mqtt/mqtt_aws_endpoint.bak"
#define MQTT_AWS_ENDPOINT_MAX_LEN           128
#define MQTT_AWS_ENDPOINT_MIN_LEN           8

#define MQTT_TOPICS_FILE_PATH          "/littlefs/mqtt/topics.txt"
#define MQTT_TOPICS_FILE_BACKUP_PATH   "/littlefs/mqtt/topics.bak"



bool load_or_restore_mqtt_serial_no(char *out, size_t out_size);
bool load_or_restore_mqtt_aws_endpoint(char *out, size_t out_size);
bool mqtt_load_certs_from_littlefs(char *root_ca,
                                   size_t root_ca_size,
                                   char *device_cert,
                                   size_t device_cert_size,
                                   char *private_key,
                                   size_t private_key_size);

bool load_or_restore_mqtt_topics(mqtt_topic_list_t *out_topics,
                                 char *mbm_req,
                                 size_t mbm_req_size,
                                 char *mbm_res,
                                 size_t mbm_res_size);

#endif
