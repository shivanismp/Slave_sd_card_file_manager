#ifndef __TASK_SIZE_AND_PRIORITIES_H__
#define __TASK_SIZE_AND_PRIORITIES_H__

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern const UBaseType_t update_network_icon_task_modbus_priority;
extern const uint32_t update_network_icon_task_modbus_stack_size_bytes;

extern const UBaseType_t wdt_task_priority;
extern const uint32_t wdt_task_stack_size_bytes;

extern const UBaseType_t event_log_task_priority;
extern const uint32_t event_log_task_stack_size_bytes;

extern const UBaseType_t ethernet_services_task_priority;
extern const uint32_t ethernet_services_task_stack_size_bytes;

extern const UBaseType_t modbus_local_bin_sd_archive_task_priority;
extern const uint32_t modbus_local_bin_sd_archive_task_stack_size_bytes;

extern const UBaseType_t modbus_master_task_priority;
extern const uint32_t modbus_master_task_stack_size_bytes;

extern const UBaseType_t modbus_hmi_master_task_priority;
extern const uint32_t modbus_hmi_master_task_stack_size_bytes;

extern const UBaseType_t mqtt_file_uploader_task_priority;
extern const uint32_t mqtt_file_uploader_task_stack_size_bytes;

extern const UBaseType_t network_manager_task_priority;
extern const uint32_t network_manager_task_stack_size_bytes;

// void set_task_priorities(void);

#endif // __TASK_SIZE_AND_PRIORITIES_H__