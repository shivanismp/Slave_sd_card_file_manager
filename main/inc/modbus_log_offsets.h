#ifndef MODBUS_LOG_OFFSETS_H
#define MODBUS_LOG_OFFSETS_H

#include <stdint.h>
#include "esp_err.h"

/* Indexed by CONTROL-CARD FC04 address 0..22, not BIN record position.
 * Values are raw register units. 0 disables triggering; 1 logs any change.
 * ON_TIME_LO/HI form ONE uint32_t offset, in the counter's native units.
 * Compiled defaults apply only when no saved configuration exists.
 */
#define MODBUS_LOG_OFFSET_COUNT 23U
#define MODBUS_LOG_OFFSET_DEFAULT_POT_ADC       1U
#define MODBUS_LOG_OFFSET_DEFAULT_CT_ADC        1U
#define MODBUS_LOG_OFFSET_DEFAULT_PT_ADC        1U
#define MODBUS_LOG_OFFSET_DEFAULT_DCC           1U
#define MODBUS_LOG_OFFSET_DEFAULT_DCV           1U
#define MODBUS_LOG_OFFSET_DEFAULT_ADC3          1U
#define MODBUS_LOG_OFFSET_DEFAULT_ADC4          1U
#define MODBUS_LOG_OFFSET_DEFAULT_ADC5          1U
#define MODBUS_LOG_OFFSET_DEFAULT_PHASE         1U
#define MODBUS_LOG_OFFSET_DEFAULT_ON_TIME_LO    1U
#define MODBUS_LOG_OFFSET_DEFAULT_ON_TIME_HI    0U
#define MODBUS_LOG_OFFSET_DEFAULT_POT_PERCENT   1U
/* Voltage x10 assumed: 30 raw units = 3.0 V. Use 3 for whole-volt data. */
#define MODBUS_LOG_OFFSET_DEFAULT_LINE_1_V      30U
#define MODBUS_LOG_OFFSET_DEFAULT_LINE_1_A      1U
#define MODBUS_LOG_OFFSET_DEFAULT_LINE_2_V      30U
#define MODBUS_LOG_OFFSET_DEFAULT_LINE_2_A      1U
#define MODBUS_LOG_OFFSET_DEFAULT_LINE_3_V      30U
#define MODBUS_LOG_OFFSET_DEFAULT_LINE_3_A      1U
#define MODBUS_LOG_OFFSET_DEFAULT_AVG_V         30U
#define MODBUS_LOG_OFFSET_DEFAULT_AVG_A         1U
#define MODBUS_LOG_OFFSET_DEFAULT_FREQ          1U
#define MODBUS_LOG_OFFSET_DEFAULT_KW            1U
#define MODBUS_LOG_OFFSET_DEFAULT_PF            1U

/* Init once at boot after nvs_flash_init(), before starting Modbus tasks. */
esp_err_t modbus_log_offsets_init(void);
void modbus_log_offsets_get(uint16_t out[MODBUS_LOG_OFFSET_COUNT]);
/* Save one contiguous range in ONE NVS blob/commit; publish RAM on success.
 * first is a control-card input address, not a holding-register address.
 * Identical already-persisted values do not cause another flash write.
 */
esp_err_t modbus_log_offsets_set(uint16_t first, uint16_t count,
                                 const uint16_t *values);

#endif
