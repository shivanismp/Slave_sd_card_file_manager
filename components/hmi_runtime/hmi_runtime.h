#ifndef __HMI_RUNTIME_H__
#define __HMI_RUNTIME_H__

#include <stdbool.h>
#include <stdint.h>
bool hmi_nextion_tft_upload_from_file(const char *path, uint32_t baud);

#endif
