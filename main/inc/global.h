#ifndef __GLOBAL_H__
#define __GLOBAL_H__

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

#include <esp_random.h>
#include <esp_system.h>

#include "machine_type_select.h"

#include "task_size_and_priorities.h"

bool file_read_string(const char *path, char *out, size_t out_size);
bool file_write_string(const char *path, const char *data);

#endif
