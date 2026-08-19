#ifndef __MODBUS_SLAVE_H__
#define __MODBUS_SLAVE_H__

#include "global.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_log.h>

/* -----------------------------------------------------------
 * Physical Delta HMI channel
 *
 * UART0 is now a Modbus RTU MASTER. The Delta HMI is slave 95.
 * The software parser modbus_slave_process_frame() is preserved
 * below and continues to act as the MQTT virtual Modbus slave.
 * ----------------------------------------------------------- */
#define MODBUS_HMI_UART_PORT_NUM        UART_NUM_0
#define MODBUS_HMI_UART_TX_PIN          20
#define MODBUS_HMI_UART_RX_PIN          6
#define MODBUS_HMI_DIR_PIN              7

#define MODBUS_HMI_UART_BUF_SIZE        256

#define MODBUS_HMI_UART_BAUD_RATE       9600
#define MODBUS_HMI_SLAVE_ID             95
#define MODBUS_HMI_POLL_PERIOD_MS       100
#define MODBUS_HMI_RESPONSE_TIMEOUT_MS  300

/*
 * Delta-HMI RTC bridge used by this project.
 *
 * $500..$506: current HMI calendar produced by GETSYSTEMTIME
 * $510..$516: ESP-provided calendar consumed by SETSYSTEMTIME
 * $517:        apply-time request; the DOPSoft macro clears it
 */
#define DELTA_HMI_RTC_READ_BASE_ADDR          500U
#define DELTA_HMI_RTC_SET_BASE_ADDR           510U
#define DELTA_HMI_RTC_SET_TRIGGER_ADDR        517U
#define DELTA_HMI_RTC_CALENDAR_WORDS          7U
#define DELTA_HMI_RTC_SET_BLOCK_WORDS         8U
#define DELTA_HMI_RTC_SERVICE_PERIOD_CYCLES   100U
#define DELTA_HMI_RTC_MAX_DIFFERENCE_SEC      2U
#define DELTA_HMI_RTC_WRITE_RETRY_MS          60000U

/* Compatibility aliases used by the existing source file. */
#define MODBUS_SLAVE_UART_PORT_NUM      MODBUS_HMI_UART_PORT_NUM
#define MODBUS_SLAVE_UART_TX_PIN        MODBUS_HMI_UART_TX_PIN
#define MODBUS_SLAVE_UART_RX_PIN        MODBUS_HMI_UART_RX_PIN
#define MODBUS_SLAVE_DIR_PIN            MODBUS_HMI_DIR_PIN
#define MODBUS_SLAVE_UART_BUF_SIZE      MODBUS_HMI_UART_BUF_SIZE
#define MODBUS_SLAVE_UART_BAUD_RATE     MODBUS_HMI_UART_BAUD_RATE

/* MQTT virtual Modbus server address. Keep this at 95. */
#define MODBUS_MQTT_SLAVE_ID            95
#define MODBUS_SLAVE_ID_EXT             MODBUS_MQTT_SLAVE_ID

/*
 * Delta internal holding-register map.
 *
 * $100 ... $140 are HMI command/setpoint registers. They map to
 * slave_holding_regs[0 ... 40].
 *
 * Runtime/readback values are written from slave_input_regs[] to
 * the Delta internal registers listed farther below.
 */
#define DELTA_HMI_CMD_BASE_ADDR         300U
#define DELTA_HMI_CMD_REG_COUNT         39U
#define DELTA_HMI_FAST_CMD_COUNT        13U


/* -----------------------------------------------------------
 * Bank sizes
 * ----------------------------------------------------------- */
#define MODBUS_SLAVE_NUM_COILS          32
#define MODBUS_SLAVE_NUM_DISC_INPUTS    32
#define MODBUS_SLAVE_NUM_HOLDING_REGS   64
#define MODBUS_SLAVE_NUM_INPUT_REGS     256

/* -----------------------------------------------------------
 * Holding map = command / shadow
 * align with control-card writable map where possible
 * ----------------------------------------------------------- */

// CONTROL CARD
#define HOLD_ADDR_MACHINE_TRIGGER        0
#define HOLD_ADDR_CONTROL_MODE           1

#define HOLD_ADDR_FREQ_MIN               2
#define HOLD_ADDR_FREQ_MAX               3

#define HOLD_ADDR_MIN_POT                4
#define HOLD_ADDR_MAX_POT                5

#define HOLD_ADDR_MIN_CT                 6
#define HOLD_ADDR_MAX_CT                 7

#define HOLD_ADDR_MIN_PT                 8
#define HOLD_ADDR_MAX_PT                 9
#define HOLD_ADDR_AUTO_POWER_PERCENT     10

// SENSOR CARD and INTERNAL
#define HOLD_ADDR_MELTER_SET_TEMP            11
#define HOLD_ADDR_TEMP_CNTRL_PID_AUTO_TUNE   12

// PROGRESS BAR RANGES

#define HOLD_ADDR_BAR_MELTER_TEMP_MIN       13
#define HOLD_ADDR_BAR_MELTER_TEMP_MAX       14


#define HOLD_ADDR_BAR_POWER_PERCENT_MIN     15
#define HOLD_ADDR_BAR_POWER_PERCENT_MAX     16


#define HOLD_ADDR_BAR_L12V_MIN          17
#define HOLD_ADDR_BAR_L12V_MAX          18

#define HOLD_ADDR_BAR_L12A_MIN          19
#define HOLD_ADDR_BAR_L12A_MAX          20

#define HOLD_ADDR_BAR_L23V_MIN          21
#define HOLD_ADDR_BAR_L23V_MAX          22

#define HOLD_ADDR_BAR_L23A_MIN          23
#define HOLD_ADDR_BAR_L23A_MAX          24

#define HOLD_ADDR_BAR_L31V_MIN          25
#define HOLD_ADDR_BAR_L31V_MAX          26

#define HOLD_ADDR_BAR_L31A_MIN          27
#define HOLD_ADDR_BAR_L31A_MAX          28

#define HOLD_ADDR_BAR_LAVGV_MIN         29
#define HOLD_ADDR_BAR_LAVGV_MAX         30

#define HOLD_ADDR_BAR_LAVGA_MIN         31
#define HOLD_ADDR_BAR_LAVGA_MAX         32


#define HOLD_ADDR_BAR_FREQ_MIN          33
#define HOLD_ADDR_BAR_FREQ_MAX          34

#define HOLD_ADDR_BAR_KW_MIN            35
#define HOLD_ADDR_BAR_KW_MAX            36

#define HOLD_ADDR_BAR_PF_MIN            37
#define HOLD_ADDR_BAR_PF_MAX            38








#define DELTA_HMI_PRIMARY_CMD_COUNT     13U

#define DELTA_HMI_BAR_FIRST_INDEX       HOLD_ADDR_BAR_MELTER_TEMP_MIN
#define DELTA_HMI_BAR_LAST_INDEX        HOLD_ADDR_BAR_PF_MAX





/* -----------------------------------------------------------
 * Input map = readback / merged feedback
 * ----------------------------------------------------------- */

// CONTROL CARD
#define INP_ADDR_MACHINE_STATE_FB        0
#define INP_ADDR_AUTO_POWER_PERCENT_FB   1
#define INP_ADDR_POWER_PERCENT           2
#define INP_ADDR_LINE_1_V                3
#define INP_ADDR_LINE_1_A                4
#define INP_ADDR_LINE_2_V                5
#define INP_ADDR_LINE_2_A                6
#define INP_ADDR_LINE_3_V                7
#define INP_ADDR_LINE_3_A                8
#define INP_ADDR_AVG_V                   9
#define INP_ADDR_AVG_A                   10
#define INP_ADDR_PWM_FREQ                11
#define INP_ADDR_AVG_KW                  12
#define INP_ADDR_AVG_PF                  13
#define INP_ADDR_ERROR_BITS_LO           14

// SENSOR CARD
#define INP_ADDR_MELTER_TEMP             15

#define INP_ADDR_CHILLER_TEMP            18
#define INP_ADDR_IGBT_PLATE_TEMP         19
#define INP_ADDR_COIL_TEMP               20               

#define INP_ADDR_COMM_STATUS             21

#define INP_ADDR_LOCAL_SET_TEMP          22

#define INP_ADDR_LOCAL_AUTO_POWER        23

/*
 * MQTT-only function-0x04 view:
 * response values 23 and 24 (zero-based addresses 22 and 23) contain the
 * uint32_t Unix timestamp, high word first. The normal Delta HMI meanings of
 * input addresses 22 and 23 remain unchanged outside the MQTT response.
 */
#define MQTT_INP_ADDR_UNIX_TIME_HI       22
#define MQTT_INP_ADDR_UNIX_TIME_LO       23


// 24 TO 41 FREE
// .
// .
// .
// .


#define INP_ADDR_INFO_MODEL              42 // 15 chars allowed
#define INP_ADDR_INFO_CAPACITY           62 // 15 chars allowed
#define INP_ADDR_INFO_IOT_ID             82 // 15 chars allowed
#define INP_ADDR_INFO_VERSION            102 // 15 chars allowed

#define INP_ADDR_HEAD_TEXT              122 // 20 chars allowed

#define INP_ADDR_LOCAL_WIFI_RSSI_STATE  142
#define INP_ADDR_CLOCK_HOUR             143
#define INP_ADDR_CLOCK_MINUTE           144
#define INP_ADDR_CLOCK_NA_AM_PM         145   // 0 = NA, 1 = AM, 2 = PM

#define INP_ADDR_DATE_DAY               146   // 0 = Sun ... 6 = Sat
#define INP_ADDR_DATE_DATE              147   // 1 to 31
#define INP_ADDR_DATE_MONTH             148   // 0 = Jan ... 11 = Dec

#define INP_ADDR_AP_SSID              149 // 20 chars allowed
#define INP_ADDR_AP_PASS              169 // 20 chars allowed





extern uint16_t slave_holding_regs[MODBUS_SLAVE_NUM_HOLDING_REGS];

int modbus_slave_process_frame(const uint8_t *rx, uint16_t rx_len, uint8_t *tx);


/* Called by modbus_master_task() after downstream polls */
void modbus_slave_sync_from_runtime(void);

/* Called by modbus_master_task() before downstream polls */
void modbus_slave_apply_pending_writes(void);

/* Physical Delta HMI master state. MQTT parser is independent. */
bool modbus_hmi_master_is_online(void);

/* Historical startup name retained so main.c requires no change. */
void app_modbus_slave(void);

#endif
