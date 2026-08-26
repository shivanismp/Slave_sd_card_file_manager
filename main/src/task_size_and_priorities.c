#include "task_size_and_priorities.h"

// // DEFAULT VALUES

// const UBaseType_t update_network_icon_task_modbus_priority = 5;
// const uint32_t update_network_icon_task_modbus_stack_size_bytes = 4096;

// const UBaseType_t wdt_task_priority = 5;
// const uint32_t wdt_task_stack_size_bytes = 2048;

// const UBaseType_t event_log_task_priority = 4;
// const uint32_t event_log_task_stack_size_bytes = 4096;

// const UBaseType_t ethernet_services_task_priority = 5;
// const uint32_t ethernet_services_task_stack_size_bytes = 4096 * 2;

// const UBaseType_t modbus_local_bin_sd_archive_task_priority = 3;
// const uint32_t modbus_local_bin_sd_archive_task_stack_size_bytes = 4096;

// const UBaseType_t modbus_master_task_priority = 8;
// const uint32_t modbus_master_task_stack_size_bytes = 4096 * 4;

// const UBaseType_t modbus_hmi_master_task_priority = 8;
// const uint32_t modbus_hmi_master_task_stack_size_bytes = 4096 * 4;

// const UBaseType_t mqtt_file_uploader_task_priority = 4;
// const uint32_t mqtt_file_uploader_task_stack_size_bytes = 8U * 1024U;



const UBaseType_t update_network_icon_task_modbus_priority = 5;
const uint32_t update_network_icon_task_modbus_stack_size_bytes = 4096;

const UBaseType_t wdt_task_priority = 5;
const uint32_t wdt_task_stack_size_bytes = 2048;

const UBaseType_t event_log_task_priority = 4;
const uint32_t event_log_task_stack_size_bytes = 4096;

const UBaseType_t ethernet_services_task_priority = 5;
const uint32_t ethernet_services_task_stack_size_bytes = 4096 * 2;

const UBaseType_t modbus_local_bin_sd_archive_task_priority = 3;
const uint32_t modbus_local_bin_sd_archive_task_stack_size_bytes = 4096;

const UBaseType_t modbus_master_task_priority = 8;
const uint32_t modbus_master_task_stack_size_bytes = 4096 * 4;

const UBaseType_t modbus_hmi_master_task_priority = 8;
const uint32_t modbus_hmi_master_task_stack_size_bytes = 4096 * 4;

const UBaseType_t mqtt_file_uploader_task_priority = 4;
const uint32_t mqtt_file_uploader_task_stack_size_bytes = 8U * 1024U;

const UBaseType_t network_manager_task_priority = 5;
const uint32_t network_manager_task_stack_size_bytes = 6144 * 4;
