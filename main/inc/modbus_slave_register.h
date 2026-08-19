#ifndef __MODBUS_SLAVE_REGISTER_H__
#define __MODBUS_SLAVE_REGISTER_H__

#include <stdbool.h>
#include <stdint.h>
#include "modbus_slave.h"

#define HOLD_REG_FILE_PATH "/littlefs/holding_registers.txt"

/*
 * Legacy sequential files caused stale values to overwrite holding registers
 * 17..38. Only the versioned V2 format is accepted now.
 */
bool modbus_slave_load_bar_ranges_from_file(void);

#endif
