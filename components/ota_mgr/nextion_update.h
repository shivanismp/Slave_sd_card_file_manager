#ifndef __NEXTION_UPDATE_H__
#define __NEXTION_UPDATE_H__

#include <stdbool.h>
#include <stddef.h>

bool nextion_update_request_from_json(const char *json, size_t len);
bool nextion_update_is_in_progress(void);


#endif