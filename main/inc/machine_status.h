#ifndef MACHINE_STATUS_H
#define MACHINE_STATUS_H
#include <stddef.h>
#include <stdint.h>

/* Pass exactly 39 big-endian register words (78 bytes), without framing.
 * Call from the Modbus task after fresh state/error reads and snapshot build.
 * Sends a complete FC04 RTU response on <serial>/MBM/STATUS on state/error
 * changes, startup and reconnect. Other register changes do not trigger it.
 */
void machine_status_publish_if_changed(const uint8_t *register_data, size_t length);
#endif
