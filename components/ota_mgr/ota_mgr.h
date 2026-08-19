#ifndef __OTA_MGR_H__
#define __OTA_MGR_H__

#include <stdbool.h>
#include <stddef.h>

bool ota_mark_running_app_valid_if_needed(void);
bool ota_request_from_json(const char *json, size_t len);
bool ota_is_in_progress(void);

/* Override this in your project if you want a stronger first-boot test */
bool app_ota_self_test(void);




#endif