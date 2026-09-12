#ifndef __MODBUS_MASTER_H__
#define __MODBUS_MASTER_H__

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "hmi.h"
// #define MANAGE_MODBUS_PRIORITY 8


// UART and Modbus config
#define MODBUS_1_UART_PORT_NUM UART_NUM_1
#define MODBUS_1_UART_TX_PIN 10
#define MODBUS_1_UART_RX_PIN 15
#define MODBUS_1_UART_RTS_PIN 11
#define MODBUS_1_UART_BUF_SIZE 256
#define MODBUS_1_UART_BAUD_RATE 9600


/*
 * Local change-history record layout (78 bytes total).
 * This matches the MQTT virtual input-register response:
 *
 *   words 0..23  = normal machine/runtime values
 *   words 24..25 = reserved (zero)
 *   words 26..36 = raw control-card input registers 0..10
 *   word  37      = uint32_t Unix timestamp high word
 *   word  38      = uint32_t Unix timestamp low word
 */
#define MODBUS_LOCAL_BIN_START_REG               0U
#define MODBUS_LOCAL_BIN_INPUT_REGISTER_COUNT    37U
#define MODBUS_LOCAL_BIN_TIMESTAMP_REGISTER_COUNT 2U
#define MODBUS_LOCAL_BIN_TIMESTAMP_HI_INDEX      37U
#define MODBUS_LOCAL_BIN_TIMESTAMP_LO_INDEX      38U
#define MODBUS_LOCAL_BIN_INPUT_START_INDEX       0U
#define MODBUS_LOCAL_BIN_REGISTER_COUNT          \
    (MODBUS_LOCAL_BIN_TIMESTAMP_REGISTER_COUNT + \
     MODBUS_LOCAL_BIN_INPUT_REGISTER_COUNT)
#define MODBUS_LOCAL_BIN_WRITE_RETRY_MS     1000U



/*
 * LittleFS -> SD archive configuration.
 * One KB is 1024 bytes. The runtime setter can change this value at any time.
 */
#define MODBUS_LOCAL_BIN_SD_THRESHOLD_KB_DEFAULT  10U
// #define MODBUS_LOCAL_BIN_SD_THRESHOLD_KB_DEFAULT  20U
#define MODBUS_LOCAL_BIN_SD_THRESHOLD_KB_MIN      1U
#define MODBUS_LOCAL_BIN_SD_THRESHOLD_KB_MAX      4096U
#define MODBUS_LOCAL_BIN_SD_CHECK_PERIOD_MS       1000U
#define MODBUS_LOCAL_BIN_SD_RETRY_PERIOD_MS       5000U
#define MODBUS_LOCAL_BIN_SD_COPY_BUFFER_SIZE      4096U


/* -----------------------------------------------------------
 * Function codes
 * ----------------------------------------------------------- */
#define MODBUS_FUNC_READ_COILS          0x01
#define MODBUS_FUNC_READ_DISC_INP       0x02
#define MODBUS_FUNC_READ_HOLD_REGS      0x03
#define MODBUS_FUNC_READ_INP_REGS       0x04
#define MODBUS_FUNC_WRITE_SING_COIL     0x05
#define MODBUS_FUNC_WRITE_SING_REG      0x06
#define MODBUS_FUNC_WRITE_MULT_COILS    0x0F
#define MODBUS_FUNC_WRITE_MULT_REGS     0x10

/* -----------------------------------------------------------
 * Exceptions
 * ----------------------------------------------------------- */
#define MODBUS_EX_ILLEGAL_FUNCTION      0x01
#define MODBUS_EX_ILLEGAL_ADDRESS       0x02
#define MODBUS_EX_ILLEGAL_VALUE         0x03



// ---------------CONTROL CARD-----------------------
// --------------------------------------------------

#define MODBUS_SLAVE_ID_CONTROL_CARD   0x01
// #define MODBUS_CONTROL_CARD_START_ADDR 0x0000
// #define MODBUS_CONTROL_CARD_REG_COUNT 6

// COILS
#define COIL_ADDR_AUTO_POWER_CONTROL_ENABLE  0
#define COIL_ADDR_BYPASS_TEMP_CUTOFF         1


// DISCRETE INPUTS (READ ONLY)
// FUNCTION CODE: (02)
#define DIS_INP_ADDR_ERROR_LM_BIT            0U
#define DIS_INP_ADDR_ERROR_PWM_TRIP_BIT      1U
#define DIS_INP_ADDR_ERROR_WF_BIT            2U
#define DIS_INP_ADDR_ERROR_OH_CL_BIT         3U
#define DIS_INP_ADDR_ERROR_OH_IG_BIT         4U
#define DIS_INP_ADDR_ERROR_EXT_1_BIT         5U
#define DIS_INP_ADDR_ERROR_EXT_2_BIT         6U
#define DIS_INP_ADDR_INP_TEMP_CUTOFF_BIT     7U
#define DIS_INP_ADDR_INP_HF_PT_TRIP_BIT      8U
#define DIS_INP_ADDR_INP_HF_CT_TRIP_BIT      9U
#define DIS_INP_ADDR_ERROR_PHASE_BIT         10U
#define DIS_INP_ADDR_INVERTER_OPEN_LOOP_BIT  11U

#define DIS_INP_TOTAL_BITS  (DIS_INP_ADDR_INVERTER_OPEN_LOOP_BIT + 1U)



// HOLDING REGISTERS (READ/WRITE)
// FUNCTION CODE: (03) - READ, 
//                (06) - WRITE SINGLE REGISTER
//                (16) - WRITE MULTIPLE REGISTER
#define HOLD_REG_ADDR_MACHINE_TRIGGER               0
#define HOLD_REG_ADDR_CONTROL_MODE                  1
#define HOLD_REG_ADDR_CFG_FREQ_MIN                  2
#define HOLD_REG_ADDR_CFG_FREQ_MAX                  3

#define HOLD_REG_ADDR_CFG_MIN_POT_VAL               4
#define HOLD_REG_ADDR_CFG_MAX_POT_VAL               5

#define HOLD_REG_ADDR_CFG_MIN_CT_VAL                6
#define HOLD_REG_ADDR_CFG_MAX_CT_VAL                7

#define HOLD_REG_ADDR_CFG_MIN_PT_VAL                8
#define HOLD_REG_ADDR_CFG_MAX_PT_VAL                9

#define HOLD_REG_ADDR_AUTO_POWER_PERCENT_VAL        10


// INPUT REGISTERS (READ ONLY)
// FUNCTION CODE: (04)
#define INP_REG_ADDR_POT_ADC                    0
// ADC1
#define INP_REG_ADDR_CT_ADC                     1
// ADC2
#define INP_REG_ADDR_PT_ADC                     2

#define INP_REG_ADDR_DCC                        3
#define INP_REG_ADDR_DCV                        4

#define INP_REG_ADDR_ADC3                       5
#define INP_REG_ADDR_ADC4                       6
#define INP_REG_ADDR_ADC5                       7

#define INP_REG_ADDR_HF_PT_CT_PHASE_ANGLE       8
#define INP_REG_ADDR_ON_TIME_LO                 9
#define INP_REG_ADDR_ON_TIME_HI                 10


#define INP_REG_ADDR_POT_PERCENT                11

#define INP_REG_LINE_1_V                        12
#define INP_REG_LINE_1_A                        13

#define INP_REG_LINE_2_V                        14
#define INP_REG_LINE_2_A                        15

#define INP_REG_LINE_3_V                        16
#define INP_REG_LINE_3_A                        17

#define INP_REG_AVG_V                           18
#define INP_REG_AVG_A                           19

#define INP_REG_ADDR_CURRENT_FREQ_KHZ           20

#define INP_REG_AVG_KW                          21

#define INP_REG_AVG_PF                          22

/* Complete control-card input address span. It is read in two FC04 chunks. */
#define CONTROL_CARD_INPUT_FIRST_REG             INP_REG_ADDR_POT_ADC
#define CONTROL_CARD_INPUT_LAST_REG              INP_REG_AVG_PF
#define CONTROL_CARD_INPUT_REG_COUNT             \
    (CONTROL_CARD_INPUT_LAST_REG - CONTROL_CARD_INPUT_FIRST_REG + 1U)

/* Raw diagnostic portion copied to virtual-slave input addresses 26..36. */
#define CONTROL_CARD_RAW_INPUT_FIRST_REG         INP_REG_ADDR_POT_ADC
#define CONTROL_CARD_RAW_INPUT_LAST_REG          INP_REG_ADDR_ON_TIME_HI
#define CONTROL_CARD_RAW_INPUT_REG_COUNT         \
    (CONTROL_CARD_RAW_INPUT_LAST_REG - CONTROL_CARD_RAW_INPUT_FIRST_REG + 1U)

/* Existing telemetry chunk: control-card input registers 11..22. */
#define CONTROL_CARD_TELEMETRY_FIRST_REG         INP_REG_ADDR_POT_PERCENT
#define CONTROL_CARD_TELEMETRY_LAST_REG          INP_REG_AVG_PF
#define CONTROL_CARD_TELEMETRY_REG_COUNT         \
    (CONTROL_CARD_TELEMETRY_LAST_REG -             \
     CONTROL_CARD_TELEMETRY_FIRST_REG + 1U)

// --------------------------------------------------


// ---------------SENSOR CARD------------------------
// --------------------------------------------------
#define MODBUS_SLAVE_ID_SENSOR_CARD    0x02

#define TEMP_CNTRL_ITHERM_CARD_START_ADDR 69

#define TEMP_CNTRL_MULTISPAN_CARD_START_ADDR 0

// #define MODBUS_SENSOR_CARD_START_ADDR   TEMP_CNTRL_ITHERM_CARD_START_ADDR
#define MODBUS_SENSOR_CARD_START_ADDR   TEMP_CNTRL_MULTISPAN_CARD_START_ADDR
#define TEMP_CNTRL_MULTISPAN_CARD_SET_TEMP_ADDR     5
#define TEMP_CNTRL_MULTISPAN_PID_AUTO_TUNE_ADDR     62


#ifdef _MACHINE_TEMPERATURE_CNTRL_
    #define MODBUS_SENSOR_CARD_REG_COUNT        4

    #define REG_ADDR_TC                         0
    #define REG_ADDR_RTD_1                      1
    #define REG_ADDR_RTD_2                      2
    #define REG_ADDR_RTD_3                      3
#endif

#ifdef _MACHINE_TIMER_CNTRL_
    #define MODBUS_SENSOR_CARD_REG_COUNT        3
    
    #define REG_ADDR_RTD_1                      0
    #define REG_ADDR_RTD_2                      1
    #define REG_ADDR_RTD_3                      2
#endif

// #define REG_ADDR_AMP_1                      4
// #define REG_ADDR_AMP_2                      5
// #define REG_ADDR_DCV                        6



// #define MELTER_TEMP_AVG_COUNT     20  // adjust as needed
// #define MELTER_AMP_AVG_COUNT      20
// #define CHILLER_AMP_AVG_COUNT     20

#define MELTER_TEMP_AVG_COUNT     1  // adjust as needed
#define MELTER_AMP_AVG_COUNT      1
#define CHILLER_AMP_AVG_COUNT     1

// --------------------------------------------------

extern volatile bool modbus_busy_flag;

extern volatile bool modbus_check_discrete_input_flag;

// /**
//  * @brief Holds the state of 10 discrete inputs (addresses 0…9).
//  */
// typedef struct {
//     bool input0;
//     bool input1;
//     bool input2;
//     bool input3;
//     bool input4;
//     bool input5;
//     bool input6;
//     bool input7;
//     bool input8;
//     bool input9;
//     bool input10;
// } discrete_inputs_t;

/**
 * @brief Read 10 discrete inputs at addresses 0…9 via Modbus RTU (Function 0x02).
 * @param[out] out   Pointer to an array of 10 bytes; on success each element is 0 or 1.
 * @return true if the read succeeded and CRC/function ID matched; false otherwise.
 */

uint16_t modbus_crc16(const uint8_t *buf, uint16_t len);

bool modbus_write_and_verify_single_reg(uint8_t slave_id, uint16_t value, uint16_t reg_addr);
bool modbus_write_and_verify_multiple_regs(uint8_t slave_id, const uint16_t *values,
                                           uint16_t start_reg_addr,
                                           uint16_t reg_count);
int modbus_read_holding_registers(uint8_t slave_id, uint8_t *rx_buf, uint16_t start_reg_addr,
                                           uint16_t reg_count);

int modbus_read_input_registers(uint8_t slave_id,
                                uint8_t *rx_buf,
                                uint16_t start_reg_addr,
                                uint16_t reg_count);

bool read_discrete_inputs(uint8_t out[DIS_INP_TOTAL_BITS]);

bool send_machine_trigger_to_modbus(uint16_t value);
bool send_control_mode_to_modbus(uint16_t value);
bool send_frequency_to_modbus(uint16_t min_freq, uint16_t max_freq);

bool send_min_pot_to_modbus(uint16_t value);
bool send_max_pot_to_modbus(uint16_t value);

bool send_min_ct_to_modbus(uint16_t value);
bool send_max_ct_to_modbus(uint16_t value);

bool send_min_pt_to_modbus(uint16_t value);
bool send_max_pt_to_modbus(uint16_t value);

/* Change/read the LittleFS-to-SD archive threshold at runtime. */
esp_err_t modbus_local_bin_set_sd_threshold_kb(uint32_t threshold_kb);
uint32_t modbus_local_bin_get_sd_threshold_kb(void);
void read_latest_machine_record(void);


void app_modbus_master(void);




void dummy_update_melter_temp(void);
void dummy_temp_test_task(void *arg);


#endif
