#include "global.h"
#include "hmi.h"
#include "date_time.h"
#include "modbus_master.h"
#include "modbus_log_offsets.h"
#include "modbus_slave.h"
#include "esp_log_tags.h"
#include "modbus_slave_register.h"
#include "esp_timer.h"
#include <time.h>


static const char *TAG_DELTA_HMI_MASTER = "DELTA_HMI_MASTER";

static void modbus_slave_write_datetime_regs_locked(bool use_24h);
static void modbus_put_ascii_1char_per_reg(uint16_t *regs,
                                           uint16_t start_addr,
                                           uint16_t max_words,
                                           const char *str);

static uint8_t  slave_coils[(MODBUS_SLAVE_NUM_COILS + 7) / 8];
static uint8_t  slave_disc_inputs[(MODBUS_SLAVE_NUM_DISC_INPUTS + 7) / 8];
uint16_t slave_holding_regs[MODBUS_SLAVE_NUM_HOLDING_REGS];
static uint16_t slave_input_regs[MODBUS_SLAVE_NUM_INPUT_REGS];

static SemaphoreHandle_t g_modbus_slave_mutex = NULL;
static SemaphoreHandle_t g_modbus_slave_api_mutex = NULL;

/* Physical Delta HMI master state. This is independent of MQTT parsing. */
static volatile bool g_modbus_hmi_online = false;
static volatile bool g_modbus_hmi_full_sync_required = true;
static uint16_t g_delta_last_commands[DELTA_HMI_CMD_REG_COUNT];
static bool g_delta_command_pending[DELTA_HMI_CMD_REG_COUNT];
static int64_t g_delta_command_pending_until_us[DELTA_HMI_CMD_REG_COUNT];
static uint16_t g_delta_last_input_regs[MODBUS_SLAVE_NUM_INPUT_REGS];

typedef struct
{
    volatile bool machine_trigger_dirty;
    volatile bool control_mode_dirty;

    volatile bool freq_pair_dirty;

    volatile bool min_pot_dirty;
    volatile bool max_pot_dirty;

    volatile bool min_ct_dirty;
    volatile bool max_ct_dirty;
    
    volatile bool min_pt_dirty;
    volatile bool max_pt_dirty;

    volatile bool auto_power_dirty;

    volatile uint16_t machine_trigger_cmd;
    volatile uint16_t control_mode_cmd;

    volatile uint16_t freq_min_cmd;
    volatile uint16_t freq_max_cmd;

    volatile uint16_t min_pot_cmd;
    volatile uint16_t max_pot_cmd;

    volatile uint16_t min_ct_cmd;
    volatile uint16_t max_ct_cmd;

    volatile uint16_t min_pt_cmd;
    volatile uint16_t max_pt_cmd;

    volatile uint16_t auto_power_cmd;

    #ifdef _MACHINE_TEMPERATURE_CNTRL_
    volatile uint16_t melter_set_temp_cmd;
    volatile uint16_t melter_temp_cntrl_pid_auto_tune_cmd;
    volatile bool melter_set_temp_dirty;
    volatile bool melter_temp_cntrl_pid_auto_tune_dirty;
    #endif

    #ifdef _MACHINE_TIMER_CNTRL_
        volatile uint16_t timer_set_time_sec_cmd;
        volatile uint16_t timer_set_time_ms_cmd;
        volatile uint16_t timer_reset_count_cmd;

        volatile bool timer_set_time_sec_dirty;
        volatile bool timer_set_time_ms_dirty;
        volatile bool timer_reset_count_dirty;
    #endif


} modbus_slave_broker_t;

static modbus_slave_broker_t g_slave_broker = {0};


/* -----------------------------------------------------------
 * Bit helpers
 * ----------------------------------------------------------- */
static inline void bit_set(uint8_t *buf, uint16_t bit_idx, bool value)
{
    uint16_t byte_idx = bit_idx / 8U;
    uint8_t  mask     = (uint8_t)(1U << (bit_idx % 8U));

    if (value)
        buf[byte_idx] |= mask;
    else
        buf[byte_idx] &= (uint8_t)(~mask);
}

static inline bool bit_get(const uint8_t *buf, uint16_t bit_idx)
{
    uint16_t byte_idx = bit_idx / 8U;
    uint8_t  mask     = (uint8_t)(1U << (bit_idx % 8U));
    return ((buf[byte_idx] & mask) != 0U);
}

/* -----------------------------------------------------------
 * CRC16
 * ----------------------------------------------------------- */
static uint16_t modbus_slave_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t pos = 0; pos < len; pos++)
    {
        crc ^= (uint16_t)buf[pos];
        for (int i = 0; i < 8; i++)
        {
            if (crc & 0x0001U)
            {
                crc >>= 1;
                crc ^= 0xA001U;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}

/* -----------------------------------------------------------
 * UART init
 * ----------------------------------------------------------- */
static esp_err_t modbus_slave_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = MODBUS_SLAVE_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    esp_err_t err;

    err = uart_driver_install(MODBUS_SLAVE_UART_PORT_NUM,
                              MODBUS_SLAVE_UART_BUF_SIZE * 2,
                              0,
                              0,
                              NULL,
                              0);
    if (err != ESP_OK)
        return err;

    err = uart_param_config(MODBUS_SLAVE_UART_PORT_NUM, &uart_config);
    if (err != ESP_OK)
        return err;

    err = uart_set_pin(MODBUS_SLAVE_UART_PORT_NUM,
                       MODBUS_SLAVE_UART_TX_PIN,
                       MODBUS_SLAVE_UART_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
        return err;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MODBUS_SLAVE_DIR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    gpio_set_level(MODBUS_SLAVE_DIR_PIN, 0); /* RX mode */
    uart_flush_input(MODBUS_SLAVE_UART_PORT_NUM);

    return ESP_OK;
}

/*
 * The old hardware-slave send/receive loop was removed. UART0 is used only
 * by the Delta HMI master transaction functions below.
 */

static bool modbus_slave_frame_valid(const uint8_t *buf, uint16_t len)
{
    if (len < 4U)
        return false;

    uint16_t rx_crc = (uint16_t)buf[len - 2] |
                      ((uint16_t)buf[len - 1] << 8);
    uint16_t calc_crc = modbus_slave_crc16(buf, len - 2);

    return (rx_crc == calc_crc);
}

/* -----------------------------------------------------------
 * Bank init / sync
 * ----------------------------------------------------------- */
static void modbus_slave_init_banks(void)
{
    memset(slave_coils, 0, sizeof(slave_coils));
    memset(slave_disc_inputs, 0, sizeof(slave_disc_inputs));
    memset(slave_holding_regs, 0, sizeof(slave_holding_regs));
    memset(slave_input_regs, 0, sizeof(slave_input_regs));
}

void modbus_slave_sync_from_runtime(void)
{
    hmi_data_t snap;

    if (g_modbus_slave_mutex == NULL)
        return;

    if (!hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        return;

    memcpy(&snap, (const void *)&hmi_data, sizeof(hmi_data_t));
    hmi_data_unlock();

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);

    /* -----------------------
     * Holding regs = command shadow
     * ----------------------- */
    slave_holding_regs[HOLD_ADDR_MACHINE_TRIGGER]      = (uint16_t)snap.machine_state;
    slave_holding_regs[HOLD_ADDR_CONTROL_MODE]         = snap.control_mode;
    slave_holding_regs[HOLD_ADDR_FREQ_MIN]             = snap.freq_min;
    slave_holding_regs[HOLD_ADDR_FREQ_MAX]             = snap.freq_max;
    slave_holding_regs[HOLD_ADDR_MIN_POT]              = snap.min_pot;
    slave_holding_regs[HOLD_ADDR_MAX_POT]              = snap.max_pot;
    slave_holding_regs[HOLD_ADDR_MIN_CT]               = snap.min_ct;
    slave_holding_regs[HOLD_ADDR_MAX_CT]               = snap.max_ct;
    slave_holding_regs[HOLD_ADDR_MIN_PT]               = snap.min_pt;
    slave_holding_regs[HOLD_ADDR_MAX_PT]               = snap.max_pt;
    slave_holding_regs[HOLD_ADDR_AUTO_POWER_PERCENT]   = snap.auto_power_percent;
    #ifdef _MACHINE_TEMPERATURE_CNTRL_
        slave_holding_regs[HOLD_ADDR_MELTER_SET_TEMP]      = snap.melter_set_temp;
        slave_holding_regs[HOLD_ADDR_BAR_MELTER_TEMP_MAX] = snap.melter_set_temp;
    #endif

    #ifdef _MACHINE_TIMER_CNTRL_
        slave_holding_regs[HOLD_ADDR_SET_TIME_SEC]      = snap.timer_set_sec;
        slave_holding_regs[HOLD_ADDR_SET_TIME_MS]       = snap.timer_set_ms;
    #endif

    uint16_t bar_freq_min = 0;
    uint16_t bar_freq_max = 0;
    if (snap.freq_min > snap.freq_max)
    {
        bar_freq_min = snap.freq_max;
        bar_freq_max = snap.freq_min;
    }
    else
    {
        bar_freq_min = snap.freq_min;
        bar_freq_max = snap.freq_max;
    }

    slave_holding_regs[HOLD_ADDR_BAR_FREQ_MIN]          = bar_freq_min;
    slave_holding_regs[HOLD_ADDR_BAR_FREQ_MAX]          = bar_freq_max;

    modbus_put_ascii_1char_per_reg(slave_input_regs,
                               INP_ADDR_AP_SSID,
                               20,
                               snap.wifi_ap_ssid);

    modbus_put_ascii_1char_per_reg(slave_input_regs,
                               INP_ADDR_AP_PASS,
                               20,
                               snap.wifi_ap_pass);

    modbus_put_ascii_1char_per_reg(slave_input_regs,
                               INP_ADDR_STA_SSID,
                               40U,
                               snap.sta_ssid);

    modbus_put_ascii_1char_per_reg(slave_input_regs,
                               INP_ADDR_STA_IP,
                               20U,
                               snap.sta_ip);

    /* -----------------------
     * Input regs = merged feedback
     * ----------------------- */
    slave_input_regs[INP_ADDR_MACHINE_STATE_FB]        = (uint16_t)snap.machine_state_fb;
    slave_input_regs[INP_ADDR_AUTO_POWER_PERCENT_FB]   = snap.auto_power_percent_fb;
    slave_input_regs[INP_ADDR_POWER_PERCENT]           = snap.power_percent;
    
    slave_input_regs[INP_ADDR_LINE_1_V]                = snap.line_1_v;
    slave_input_regs[INP_ADDR_LINE_1_A]                = snap.line_1_a;
    slave_input_regs[INP_ADDR_LINE_2_V]                = snap.line_2_v;
    slave_input_regs[INP_ADDR_LINE_2_A]                = snap.line_2_a;
    slave_input_regs[INP_ADDR_LINE_3_V]                = snap.line_3_v;
    slave_input_regs[INP_ADDR_LINE_3_A]                = snap.line_3_a;
    slave_input_regs[INP_ADDR_AVG_V]                   = snap.avg_v;
    slave_input_regs[INP_ADDR_AVG_A]                   = snap.avg_a;
    slave_input_regs[INP_ADDR_PWM_FREQ]                = snap.pwm_freq;
    slave_input_regs[INP_ADDR_AVG_KW]                  = snap.avg_kw;
    slave_input_regs[INP_ADDR_AVG_PF]                  = snap.avg_pf;

    slave_input_regs[INP_ADDR_ERROR_BITS_LO]           = 0;

    for (size_t i = 0U; i < DIS_INP_TOTAL_BITS; i++)
    {
        if (snap.error_leds[i])
            slave_input_regs[INP_ADDR_ERROR_BITS_LO] |= (1U << i);
    }

    /* -----------------------
     * Discrete inputs = bit feedback
     * ----------------------- */
    memset(slave_disc_inputs, 0, sizeof(slave_disc_inputs));
    for (size_t i = 0U; i < DIS_INP_TOTAL_BITS; i++)
    {
        bit_set(slave_disc_inputs, i, (snap.error_leds[i] != 0U));
    }

    #ifdef _MACHINE_TEMPERATURE_CNTRL_
        slave_input_regs[INP_ADDR_MELTER_TEMP]             = snap.melter_temp;
        slave_input_regs[INP_ADDR_LOCAL_SET_TEMP]          = snap.melter_set_temp;
    #endif

    #ifdef _MACHINE_TIMER_CNTRL_
        slave_input_regs[INP_ADDR_TIMER_SEC] = snap.timer_sec;
        slave_input_regs[INP_ADDR_TIMER_MS] = snap.timer_ms;
        slave_input_regs[INP_ADDR_JOB_COUNTER] = snap.timer_finish_count;
    #endif

    // slave_input_regs[INP_ADDR_MELTER_TEMP]             = 1150;
    slave_input_regs[INP_ADDR_CHILLER_TEMP]            = snap.chiller_temp;
    slave_input_regs[INP_ADDR_IGBT_PLATE_TEMP]         = snap.igbt_plate_temp;
    slave_input_regs[INP_ADDR_COIL_TEMP]               = snap.coil_temp;

    slave_input_regs[INP_ADDR_COMM_STATUS]             = (uint16_t)(0x0003U | (g_modbus_hmi_online ? 0x0004U : 0U));
    slave_input_regs[INP_ADDR_LOCAL_AUTO_POWER]        = snap.auto_power_percent;

    _Static_assert(INP_ADDR_CONTROL_CARD_RAW_COUNT ==
                       HMI_CONTROL_CARD_RAW_INPUT_COUNT,
                   "Virtual-slave raw input count mismatch");

    /* Control-card input 0..10 -> virtual-slave input 26..36. */
    for (uint16_t reg = 0U;
         reg < INP_ADDR_CONTROL_CARD_RAW_COUNT;
         ++reg)
    {
        slave_input_regs[INP_ADDR_CONTROL_CARD_RAW_FIRST + reg] =
            snap.control_card_raw_input[reg];
    }

    slave_input_regs[INP_ADDR_LOCAL_WIFI_RSSI_STATE]   = snap.wifi_rssi_state;

    /* Clock registers for Delta HMI */
    // modbus_slave_write_datetime_regs_locked(true);
    modbus_slave_write_datetime_regs_locked(false);

    xSemaphoreGive(g_modbus_slave_mutex);
}

/* -----------------------------------------------------------
 * Broker
 * ----------------------------------------------------------- */
static void modbus_slave_stage_holding_write(uint16_t addr, uint16_t value)
{
    slave_holding_regs[addr] = value;

    switch (addr)
    {
        case HOLD_ADDR_MACHINE_TRIGGER:
            g_slave_broker.machine_trigger_cmd = value;
            g_slave_broker.machine_trigger_dirty = true;
            break;

        case HOLD_ADDR_CONTROL_MODE:
            g_slave_broker.control_mode_cmd = value;
            g_slave_broker.control_mode_dirty = true;
            break;

        case HOLD_ADDR_FREQ_MIN:
            g_slave_broker.freq_min_cmd = value;
            break;

        case HOLD_ADDR_FREQ_MAX:
            g_slave_broker.freq_max_cmd = value;
            g_slave_broker.freq_pair_dirty = true;
            break;

        case HOLD_ADDR_MIN_POT:
            g_slave_broker.min_pot_cmd = value;
            g_slave_broker.min_pot_dirty = true;
            break;

        case HOLD_ADDR_MAX_POT:
            g_slave_broker.max_pot_cmd = value;
            g_slave_broker.max_pot_dirty = true;
            break;
        
        case HOLD_ADDR_MIN_CT:
            g_slave_broker.min_ct_cmd = value;
            g_slave_broker.min_ct_dirty = true;
            break;

        case HOLD_ADDR_MAX_CT:
            g_slave_broker.max_ct_cmd = value;
            g_slave_broker.max_ct_dirty = true;
            break;
        
        case HOLD_ADDR_MIN_PT:
            g_slave_broker.min_pt_cmd = value;
            g_slave_broker.min_pt_dirty = true;
            break;

        case HOLD_ADDR_MAX_PT:
            g_slave_broker.max_pt_cmd = value;
            g_slave_broker.max_pt_dirty = true;
            break;

        case HOLD_ADDR_AUTO_POWER_PERCENT:
            g_slave_broker.auto_power_cmd = value;
            g_slave_broker.auto_power_dirty = true;
            break;

        case HOLD_ADDR_MELTER_SET_TEMP:
            g_slave_broker.melter_set_temp_cmd = value;
            g_slave_broker.melter_set_temp_dirty = true;
            break;
        
        case HOLD_ADDR_TEMP_CNTRL_PID_AUTO_TUNE:
            g_slave_broker.melter_temp_cntrl_pid_auto_tune_cmd = value;
            g_slave_broker.melter_temp_cntrl_pid_auto_tune_dirty = true;
            break;

        default:
            /* shadow only for future write helpers */
            break;
    }
}

void modbus_slave_apply_pending_writes(void)
{
    modbus_slave_broker_t local;

    if (g_modbus_slave_mutex == NULL)
        return;

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    local = g_slave_broker;
    memset(&g_slave_broker, 0, sizeof(g_slave_broker));
    xSemaphoreGive(g_modbus_slave_mutex);

    if (local.machine_trigger_dirty)
    {
        bool ok = send_machine_trigger_to_modbus(local.machine_trigger_cmd);

        if (ok)
        {
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.machine_state = (MACHINE_TRIGGER_ENUM)local.machine_trigger_cmd;
                hmi_data_unlock();
            }
        }
        else
        {
            /* optional: restore broker dirty flag or set comm error status */
        }
    }

    if (local.control_mode_dirty)
    {
        bool ok = send_control_mode_to_modbus(local.control_mode_cmd);

        if (ok)
        {
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.control_mode = local.control_mode_cmd;
                hmi_data_unlock();
            }
        }
        else
        {
        }
    }

    if (local.freq_pair_dirty)
    {
        bool ok = send_frequency_to_modbus(local.freq_min_cmd,
                                       local.freq_max_cmd);


        if (ok)
        {
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.freq_min = local.freq_min_cmd;
                hmi_data.freq_max = local.freq_max_cmd;
                hmi_data_unlock();
            }
        }
        else
        {
        }
    }

    if (local.min_pot_dirty)
    {
        bool ok = send_min_pot_to_modbus(local.min_pot_cmd);

        if (ok)
        {
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.min_pot = local.min_pot_cmd;
                hmi_data_unlock();
            }
            
        }
        else
        {
        }
    }
    if (local.max_pot_dirty)
    {
        
        bool ok = send_max_pot_to_modbus(local.max_pot_cmd);

        if (ok)
        {   if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.max_pot = local.max_pot_cmd;
                hmi_data_unlock();
            }
            
        }
        else
        {
        }
    }

    if (local.min_ct_dirty)
    {
        bool ok = send_min_ct_to_modbus(local.min_ct_cmd);

        if (ok)
        {
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.min_ct = local.min_ct_cmd;
                hmi_data_unlock();
            }
            
        }
        else
        {
        }
    }
    if (local.max_ct_dirty)
    {
        bool ok = send_max_ct_to_modbus(local.max_ct_cmd);

        if (ok)
        {
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.max_ct = local.max_ct_cmd;
                hmi_data_unlock();
            }
            
        }
        else
        {
        }
    }

    if (local.min_pt_dirty)
    {
        bool ok = send_min_pt_to_modbus(local.min_pt_cmd);

        if (ok)
        {
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.min_pt = local.min_pt_cmd;
                hmi_data_unlock();
            }
            
        }
        else
        {
        }
    }
    if (local.max_pt_dirty)
    {
        bool ok = send_max_pt_to_modbus(local.max_pt_cmd);

        if (ok)
        {
            if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
            {
                /* keep desired/local command in sync */
                hmi_data.max_pt = local.max_pt_cmd;
                hmi_data_unlock();
            }
            
        }
        else
        {
        }
    }

    if (local.auto_power_dirty)
    {
        uint16_t power_percent = local.auto_power_cmd;

        if (power_percent > 100U)
            power_percent = 100U;

        if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        {
            if (power_percent == 0U)
            {
                // /* 0 means release manual override and let AI take back control */
                // hmi_data.auto_power_percent_force_from_mb_slave = false;
            }
            else
            {
                /* non-zero means manual/modbus override owns the power command */
                hmi_data.auto_power_percent_force_from_mb_slave = true;

                hmi_data.auto_power_percent = power_percent;
                
            }
            hmi_data_unlock();
        }
    }

    if (local.melter_set_temp_dirty)
    {
        uint16_t set_temp = local.melter_set_temp_cmd;

        if (hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        {
            hmi_data.melter_set_temp = set_temp;
            hmi_data_unlock();
        }
        hmi_data_t snap;
        if (!hmi_data_get_snapshot(&snap, HMI_DATA_SNAPSHOT_TIMEOUT))
            return;

        modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_SENSOR_CARD, set_temp, TEMP_CNTRL_MULTISPAN_CARD_SET_TEMP_ADDR);

        xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
        slave_holding_regs[HOLD_ADDR_BAR_MELTER_TEMP_MAX] = set_temp;
        xSemaphoreGive(g_modbus_slave_mutex);

        file_write_melter_set_temp(set_temp);
    }

    if (local.melter_temp_cntrl_pid_auto_tune_dirty)
    {
        uint16_t pid_auto_tune_cmd = local.melter_temp_cntrl_pid_auto_tune_cmd;
        modbus_write_and_verify_single_reg(MODBUS_SLAVE_ID_SENSOR_CARD, pid_auto_tune_cmd, TEMP_CNTRL_MULTISPAN_PID_AUTO_TUNE_ADDR);

    }

}

/* -----------------------------------------------------------
 * Response helpers
 * ----------------------------------------------------------- */
static int build_exception(uint8_t slave_id, uint8_t func, uint8_t exc_code, uint8_t *tx)
{
    tx[0] = slave_id;
    tx[1] = (uint8_t)(func | 0x80U);
    tx[2] = exc_code;

    uint16_t crc = modbus_slave_crc16(tx, 3);
    tx[3] = (uint8_t)(crc & 0xFF);
    tx[4] = (uint8_t)((crc >> 8) & 0xFF);

    return 5;
}

static int build_read_bits_response(uint8_t slave_id,
                                    uint8_t func,
                                    uint16_t start,
                                    uint16_t qty,
                                    const uint8_t *bits_src,
                                    uint16_t max_bits,
                                    uint8_t *tx)
{
    if ((qty == 0U) || (qty > 2000U))
        return build_exception(slave_id, func, MODBUS_EX_ILLEGAL_VALUE, tx);

    if ((start + qty) > max_bits)
        return build_exception(slave_id, func, MODBUS_EX_ILLEGAL_ADDRESS, tx);

    uint8_t byte_count = (uint8_t)((qty + 7U) / 8U);

    tx[0] = slave_id;
    tx[1] = func;
    tx[2] = byte_count;

    memset(&tx[3], 0, byte_count);

    for (uint16_t i = 0; i < qty; i++)
    {
        if (bit_get(bits_src, start + i))
            tx[3 + (i / 8U)] |= (uint8_t)(1U << (i % 8U));
    }

    uint16_t len = (uint16_t)(3U + byte_count);
    uint16_t crc = modbus_slave_crc16(tx, len);
    tx[len + 0] = (uint8_t)(crc & 0xFF);
    tx[len + 1] = (uint8_t)((crc >> 8) & 0xFF);
    
    return (int)(len + 2U);
}

static int build_read_regs_response(uint8_t slave_id,
                                    uint8_t func,
                                    uint16_t start,
                                    uint16_t qty,
                                    const uint16_t *regs,
                                    uint16_t reg_count,
                                    uint8_t *tx)
{
    if ((qty == 0U) || (qty > 125U))
        return build_exception(slave_id, func, MODBUS_EX_ILLEGAL_VALUE, tx);

    if ((start + qty) > reg_count)
        return build_exception(slave_id, func, MODBUS_EX_ILLEGAL_ADDRESS, tx);

    tx[0] = slave_id;
    tx[1] = func;
    tx[2] = (uint8_t)(qty * 2U);

    for (uint16_t i = 0; i < qty; i++)
    {
        uint16_t v = regs[start + i];
        tx[3 + i * 2] = (uint8_t)((v >> 8) & 0xFF);
        tx[4 + i * 2] = (uint8_t)(v & 0xFF);
    }

    uint16_t len = (uint16_t)(3U + (qty * 2U));
    uint16_t crc = modbus_slave_crc16(tx, len);
    tx[len + 0] = (uint8_t)(crc & 0xFF);
    tx[len + 1] = (uint8_t)((crc >> 8) & 0xFF);

    return (int)(len + 2U);
}

static int handle_fc01(const uint8_t *rx, uint8_t *tx)
{
    uint16_t start = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t qty   = ((uint16_t)rx[4] << 8) | rx[5];

    int ret;
    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    ret = build_read_bits_response(rx[0],
                                   MODBUS_FUNC_READ_COILS,
                                   start,
                                   qty,
                                   slave_coils,
                                   MODBUS_SLAVE_NUM_COILS,
                                   tx);
    xSemaphoreGive(g_modbus_slave_mutex);
    return ret;
}

static int handle_fc02(const uint8_t *rx, uint8_t *tx)
{
    uint16_t start = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t qty   = ((uint16_t)rx[4] << 8) | rx[5];

    int ret;
    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    ret = build_read_bits_response(rx[0],
                                   MODBUS_FUNC_READ_DISC_INP,
                                   start,
                                   qty,
                                   slave_disc_inputs,
                                   MODBUS_SLAVE_NUM_DISC_INPUTS,
                                   tx);
    xSemaphoreGive(g_modbus_slave_mutex);
    return ret;
}

static int handle_fc03(const uint8_t *rx, uint8_t *tx)
{
    uint16_t start = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t qty   = ((uint16_t)rx[4] << 8) | rx[5];

    int ret;
    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    modbus_log_offsets_get(&slave_holding_regs[HOLD_ADDR_LOG_OFFSET_FIRST]);
    ret = build_read_regs_response(rx[0],
                                   MODBUS_FUNC_READ_HOLD_REGS,
                                   start,
                                   qty,
                                   slave_holding_regs,
                                   MODBUS_SLAVE_NUM_HOLDING_REGS,
                                   tx);
    xSemaphoreGive(g_modbus_slave_mutex);
    return ret;
}

static int handle_fc04(const uint8_t *rx, uint8_t *tx)
{
    uint16_t start = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t qty   = ((uint16_t)rx[4] << 8) | rx[5];

    int ret;
    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);

    /*
     * Build an MQTT-only view with timestamp high/low at addresses 37/38.
     * Save and restore the normal register values while holding the mutex, so
     * the physical Delta HMI continues to see its existing address map.
     */
    uint16_t saved_time_hi = slave_input_regs[MQTT_INP_ADDR_UNIX_TIME_HI];
    uint16_t saved_time_lo = slave_input_regs[MQTT_INP_ADDR_UNIX_TIME_LO];
    uint32_t unix_time = 0U;
    if (app_time_is_valid())
    {
        time_t now = 0;
        time(&now);
        if (now >= 0 && (uint64_t)now <= (uint64_t)UINT32_MAX)
        {
            unix_time = (uint32_t)now;
        }
    }

    slave_input_regs[MQTT_INP_ADDR_UNIX_TIME_HI] =
        (uint16_t)(unix_time >> 16);
    slave_input_regs[MQTT_INP_ADDR_UNIX_TIME_LO] =
        (uint16_t)(unix_time & 0xFFFFU);

    ret = build_read_regs_response(rx[0],
                                   MODBUS_FUNC_READ_INP_REGS,
                                   start,
                                   qty,
                                   slave_input_regs,
                                   MODBUS_SLAVE_NUM_INPUT_REGS,
                                   tx);

    slave_input_regs[MQTT_INP_ADDR_UNIX_TIME_HI] = saved_time_hi;
    slave_input_regs[MQTT_INP_ADDR_UNIX_TIME_LO] = saved_time_lo;
    xSemaphoreGive(g_modbus_slave_mutex);
    return ret;
}

static int handle_fc05(const uint8_t *rx, uint8_t *tx)
{
    uint16_t addr = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t val  = ((uint16_t)rx[4] << 8) | rx[5];

    if (addr >= MODBUS_SLAVE_NUM_COILS)
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_ADDRESS, tx);

    if (!(val == 0x0000U || val == 0xFF00U))
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_VALUE, tx);

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    bit_set(slave_coils, addr, (val == 0xFF00U));
    xSemaphoreGive(g_modbus_slave_mutex);

    memcpy(tx, rx, 6);
    {
        uint16_t crc = modbus_slave_crc16(tx, 6);
        tx[6] = (uint8_t)(crc & 0xFF);
        tx[7] = (uint8_t)((crc >> 8) & 0xFF);
    }

    return 8;
}

static int handle_fc06(const uint8_t *rx, uint8_t *tx)
{
    uint16_t addr  = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t value = ((uint16_t)rx[4] << 8) | rx[5];

    if (addr >= MODBUS_SLAVE_NUM_HOLDING_REGS)
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_ADDRESS, tx);

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    if (addr >= HOLD_ADDR_LOG_OFFSET_FIRST && addr <= HOLD_ADDR_LOG_OFFSET_LAST)
    {
        esp_err_t err = modbus_log_offsets_set(
            addr - HOLD_ADDR_LOG_OFFSET_FIRST, 1U, &value);
        if (err != ESP_OK)
        {
            xSemaphoreGive(g_modbus_slave_mutex);
            ESP_LOGE(TAG_MODBUS_SLAVE, "BIN offset save failed: %s", esp_err_to_name(err));
            return build_exception(rx[0], rx[1], 0x04U, tx); /* Device failure */
        }
        modbus_log_offsets_get(&slave_holding_regs[HOLD_ADDR_LOG_OFFSET_FIRST]);
    }
    else
        modbus_slave_stage_holding_write(addr, value);
    xSemaphoreGive(g_modbus_slave_mutex);

    memcpy(tx, rx, 6);
    {
        uint16_t crc = modbus_slave_crc16(tx, 6);
        tx[6] = (uint8_t)(crc & 0xFF);
        tx[7] = (uint8_t)((crc >> 8) & 0xFF);
    }

    return 8;
}

static int handle_fc0F(const uint8_t *rx, uint16_t rx_len, uint8_t *tx)
{
    uint16_t start = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t qty   = ((uint16_t)rx[4] << 8) | rx[5];
    uint8_t byte_count = rx[6];

    if ((qty == 0U) || (qty > 1968U))
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_VALUE, tx);

    if ((start + qty) > MODBUS_SLAVE_NUM_COILS)
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_ADDRESS, tx);

    if (rx_len < (uint16_t)(7U + byte_count + 2U))
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_VALUE, tx);

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < qty; i++)
    {
        bool bit = ((rx[7 + (i / 8U)] >> (i % 8U)) & 0x01U) != 0U;
        bit_set(slave_coils, start + i, bit);
    }
    xSemaphoreGive(g_modbus_slave_mutex);

    tx[0] = rx[0];
    tx[1] = rx[1];
    tx[2] = rx[2];
    tx[3] = rx[3];
    tx[4] = rx[4];
    tx[5] = rx[5];

    {
        uint16_t crc = modbus_slave_crc16(tx, 6);
        tx[6] = (uint8_t)(crc & 0xFF);
        tx[7] = (uint8_t)((crc >> 8) & 0xFF);
    }

    return 8;
}

static int handle_fc10(const uint8_t *rx, uint16_t rx_len, uint8_t *tx)
{
    uint16_t start = ((uint16_t)rx[2] << 8) | rx[3];
    uint16_t qty   = ((uint16_t)rx[4] << 8) | rx[5];
    uint8_t byte_count = rx[6];

    if ((qty == 0U) || (qty > 123U))
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_VALUE, tx);

    if ((start + qty) > MODBUS_SLAVE_NUM_HOLDING_REGS)
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_ADDRESS, tx);

    if (byte_count != (uint8_t)(qty * 2U))
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_VALUE, tx);

    if (rx_len < (uint16_t)(7U + byte_count + 2U))
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_VALUE, tx);

    bool touches_offsets = start <= HOLD_ADDR_LOG_OFFSET_LAST &&
                           (uint32_t)start + qty > HOLD_ADDR_LOG_OFFSET_FIRST;
    /* Keep a persistent settings transaction separate from machine commands. */
    if (touches_offsets && (start < HOLD_ADDR_LOG_OFFSET_FIRST ||
        (uint32_t)start + qty > HOLD_ADDR_LOG_OFFSET_LAST + 1U))
        return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_ADDRESS, tx);

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    if (touches_offsets)
    {
        uint16_t values[MODBUS_LOG_OFFSET_COUNT];
        for (uint16_t i = 0U; i < qty; ++i)
            values[i] = ((uint16_t)rx[7 + i * 2] << 8) | rx[8 + i * 2];
        esp_err_t err = modbus_log_offsets_set(
            start - HOLD_ADDR_LOG_OFFSET_FIRST, qty, values);
        if (err != ESP_OK)
        {
            xSemaphoreGive(g_modbus_slave_mutex);
            ESP_LOGE(TAG_MODBUS_SLAVE, "BIN offsets save failed: %s", esp_err_to_name(err));
            return build_exception(rx[0], rx[1], 0x04U, tx);
        }
        modbus_log_offsets_get(&slave_holding_regs[HOLD_ADDR_LOG_OFFSET_FIRST]);
    }
    else
    {
        for (uint16_t i = 0; i < qty; i++)
        {
            uint16_t value = ((uint16_t)rx[7 + i * 2] << 8) | rx[8 + i * 2];
            modbus_slave_stage_holding_write(start + i, value);
        }
    }
    xSemaphoreGive(g_modbus_slave_mutex);

    tx[0] = rx[0];
    tx[1] = rx[1];
    tx[2] = rx[2];
    tx[3] = rx[3];
    tx[4] = rx[4];
    tx[5] = rx[5];

    {
        uint16_t crc = modbus_slave_crc16(tx, 6);
        tx[6] = (uint8_t)(crc & 0xFF);
        tx[7] = (uint8_t)((crc >> 8) & 0xFF);
    }

    return 8;
}

static int modbus_slave_process_frame_locked(const uint8_t *rx, uint16_t rx_len, uint8_t *tx)
{
    if (rx == NULL || tx == NULL)
        return 0;

    if (rx_len < 4)
        return 0;

    if (!modbus_slave_frame_valid(rx, rx_len))
        return 0;

    if (rx[0] != MODBUS_SLAVE_ID_EXT)
        return 0;

    switch (rx[1])
    {
        case MODBUS_FUNC_READ_COILS:
            return handle_fc01(rx, tx);

        case MODBUS_FUNC_READ_DISC_INP:
            return handle_fc02(rx, tx);

        case MODBUS_FUNC_READ_HOLD_REGS:
            return handle_fc03(rx, tx);

        case MODBUS_FUNC_READ_INP_REGS:
            return handle_fc04(rx, tx);

        case MODBUS_FUNC_WRITE_SING_COIL:
            return handle_fc05(rx, tx);

        case MODBUS_FUNC_WRITE_SING_REG:
            return handle_fc06(rx, tx);

        case MODBUS_FUNC_WRITE_MULT_COILS:
            return handle_fc0F(rx, rx_len, tx);

        case MODBUS_FUNC_WRITE_MULT_REGS:
            return handle_fc10(rx, rx_len, tx);

        default:
            return build_exception(rx[0], rx[1], MODBUS_EX_ILLEGAL_FUNCTION, tx);
    }
}

int modbus_slave_process_frame(const uint8_t *rx, uint16_t rx_len, uint8_t *tx)
{
    int ret = 0;

    if (g_modbus_slave_api_mutex == NULL)
        return 0;

    xSemaphoreTake(g_modbus_slave_api_mutex, portMAX_DELAY);
    ret = modbus_slave_process_frame_locked(rx, rx_len, tx);
    xSemaphoreGive(g_modbus_slave_api_mutex);

    return ret;
}

static esp_err_t modbus_hmi_receive_response(uint8_t expected_function,
                                                uint8_t *response,
                                                uint16_t response_capacity,
                                                uint16_t *response_length)
{
    if (response == NULL || response_length == NULL || response_capacity < 5U)
        return ESP_ERR_INVALID_ARG;

    uint16_t total = 0U;
    uint16_t expected = 0U;
    int64_t deadline_us = esp_timer_get_time() +
                          ((int64_t)MODBUS_HMI_RESPONSE_TIMEOUT_MS * 1000LL);

    while (esp_timer_get_time() < deadline_us && total < response_capacity)
    {
        int got = uart_read_bytes(MODBUS_HMI_UART_PORT_NUM,
                                  response + total,
                                  response_capacity - total,
                                  pdMS_TO_TICKS(10));

        if (got > 0)
            total = (uint16_t)(total + (uint16_t)got);

        if (total >= 2U && (response[1] & 0x80U) != 0U)
            expected = 5U;
        else if (total >= 3U && response[1] == MODBUS_FUNC_READ_HOLD_REGS)
            expected = (uint16_t)(3U + response[2] + 2U);
        else if (total >= 2U && response[1] == MODBUS_FUNC_WRITE_MULT_REGS)
            expected = 8U;
        else if (total >= 2U && response[1] == MODBUS_FUNC_WRITE_SING_REG)
            expected = 8U;

        if (expected > 0U && total >= expected)
            break;
    }

    if (total < 5U)
        return ESP_ERR_TIMEOUT;

    if (response[0] != MODBUS_HMI_SLAVE_ID)
        return ESP_ERR_INVALID_RESPONSE;

    if (!modbus_slave_frame_valid(response, total))
        return ESP_ERR_INVALID_CRC;

    if ((response[1] & 0x80U) != 0U)
    {
        ESP_LOGW(TAG_DELTA_HMI_MASTER,
                 "Delta exception: fc=0x%02X code=0x%02X",
                 response[1],
                 (total >= 3U) ? response[2] : 0xFFU);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (response[1] != expected_function)
        return ESP_ERR_INVALID_RESPONSE;

    *response_length = total;
    return ESP_OK;
}

static esp_err_t modbus_hmi_exchange(const uint8_t *request,
                                     uint16_t request_length,
                                     uint8_t expected_function,
                                     uint8_t *response,
                                     uint16_t response_capacity,
                                     uint16_t *response_length)
{
    if (request == NULL || request_length < 4U)
        return ESP_ERR_INVALID_ARG;

    uart_flush_input(MODBUS_HMI_UART_PORT_NUM);

    gpio_set_level(MODBUS_HMI_DIR_PIN, 1); /* TX mode */
    esp_rom_delay_us(50);

    int written = uart_write_bytes(MODBUS_HMI_UART_PORT_NUM,
                                   (const char *)request,
                                   request_length);

    if (written != request_length)
    {
        gpio_set_level(MODBUS_HMI_DIR_PIN, 0);
        return ESP_FAIL;
    }

    esp_err_t err = uart_wait_tx_done(MODBUS_HMI_UART_PORT_NUM,
                                      pdMS_TO_TICKS(150));

    esp_rom_delay_us(50);
    gpio_set_level(MODBUS_HMI_DIR_PIN, 0); /* RX mode */

    if (err != ESP_OK)
        return err;

    return modbus_hmi_receive_response(expected_function,
                                       response,
                                       response_capacity,
                                       response_length);
}

static esp_err_t modbus_hmi_read_holding(uint16_t first_address,
                                         uint16_t quantity,
                                         uint16_t *values)
{
    if (values == NULL || quantity == 0U || quantity > 125U)
        return ESP_ERR_INVALID_ARG;

    uint8_t request[8];
    uint8_t response[256];
    uint16_t response_length = 0U;

    request[0] = MODBUS_HMI_SLAVE_ID;
    request[1] = MODBUS_FUNC_READ_HOLD_REGS;
    request[2] = (uint8_t)(first_address >> 8U);
    request[3] = (uint8_t)(first_address & 0xFFU);
    request[4] = (uint8_t)(quantity >> 8U);
    request[5] = (uint8_t)(quantity & 0xFFU);

    uint16_t crc = modbus_slave_crc16(request, 6U);
    request[6] = (uint8_t)(crc & 0xFFU);
    request[7] = (uint8_t)(crc >> 8U);

    esp_err_t err = modbus_hmi_exchange(request,
                                        sizeof(request),
                                        MODBUS_FUNC_READ_HOLD_REGS,
                                        response,
                                        sizeof(response),
                                        &response_length);
    if (err != ESP_OK)
        return err;

    if (response_length != (uint16_t)(5U + (quantity * 2U)) ||
        response[2] != (uint8_t)(quantity * 2U))
        return ESP_ERR_INVALID_RESPONSE;

    for (uint16_t i = 0U; i < quantity; ++i)
    {
        values[i] = ((uint16_t)response[3U + (i * 2U)] << 8U) |
                    response[4U + (i * 2U)];
    }

    return ESP_OK;
}

static esp_err_t modbus_hmi_write_holding(uint16_t first_address,
                                          const uint16_t *values,
                                          uint16_t quantity)
{
    if (values == NULL || quantity == 0U || quantity > 123U)
        return ESP_ERR_INVALID_ARG;

    uint8_t request[256];
    uint8_t response[16];
    uint16_t response_length = 0U;
    uint16_t request_length = (uint16_t)(9U + (quantity * 2U));

    request[0] = MODBUS_HMI_SLAVE_ID;
    request[1] = MODBUS_FUNC_WRITE_MULT_REGS;
    request[2] = (uint8_t)(first_address >> 8U);
    request[3] = (uint8_t)(first_address & 0xFFU);
    request[4] = (uint8_t)(quantity >> 8U);
    request[5] = (uint8_t)(quantity & 0xFFU);
    request[6] = (uint8_t)(quantity * 2U);

    for (uint16_t i = 0U; i < quantity; ++i)
    {
        request[7U + (i * 2U)] = (uint8_t)(values[i] >> 8U);
        request[8U + (i * 2U)] = (uint8_t)(values[i] & 0xFFU);
    }

    uint16_t crc = modbus_slave_crc16(request, (uint16_t)(request_length - 2U));
    request[request_length - 2U] = (uint8_t)(crc & 0xFFU);
    request[request_length - 1U] = (uint8_t)(crc >> 8U);

    esp_err_t err = modbus_hmi_exchange(request,
                                        request_length,
                                        MODBUS_FUNC_WRITE_MULT_REGS,
                                        response,
                                        sizeof(response),
                                        &response_length);
    if (err != ESP_OK)
        return err;

    if (response_length != 8U ||
        response[2] != request[2] || response[3] != request[3] ||
        response[4] != request[4] || response[5] != request[5])
        return ESP_ERR_INVALID_RESPONSE;

    return ESP_OK;
}

/*
 * Write the ESP local calendar and trigger in one FC16 transaction.
 * The DOPSoft Clock Macro consumes $510..$516 with SETSYSTEMTIME($510)
 * and clears $517 after applying the value to the HMI RTC.
 */
static esp_err_t modbus_hmi_write_esp_time_to_delta(void)
{
    uint16_t set_block[DELTA_HMI_RTC_SET_BLOCK_WORDS] = {0};

    if (!app_time_get_local_calendar(set_block))
        return ESP_ERR_INVALID_STATE;

    set_block[DELTA_HMI_RTC_CALENDAR_WORDS] = 1U;

    esp_err_t err = modbus_hmi_write_holding(
        DELTA_HMI_RTC_SET_BASE_ADDR,
        set_block,
        DELTA_HMI_RTC_SET_BLOCK_WORDS);

    if (err == ESP_OK)
    {
        app_time_mark_hmi_updated();
        ESP_LOGI(TAG_DELTA_HMI_MASTER,
                 "Queued system time for Delta RTC at $%u..$%u",
                 (unsigned)DELTA_HMI_RTC_SET_BASE_ADDR,
                 (unsigned)DELTA_HMI_RTC_SET_TRIGGER_ADDR);
    }

    return err;
}

/*
 * Time-source policy:
 *   1. Delta RTC initializes the ESP system clock when Internet is absent.
 *   2. A successful SNTP callback permanently outranks the Delta fallback.
 *   3. SNTP-corrected local calendar time is mirrored back to the Delta RTC.
 */
static esp_err_t modbus_hmi_time_service(void)
{
    static int64_t last_hmi_write_attempt_us = 0;
    uint16_t hmi_calendar[DELTA_HMI_RTC_CALENDAR_WORDS] = {0};

    esp_err_t err = modbus_hmi_read_holding(
        DELTA_HMI_RTC_READ_BASE_ADDR,
        DELTA_HMI_RTC_CALENDAR_WORDS,
        hmi_calendar);

    if (err != ESP_OK)
        return err;

    time_t hmi_time = 0;
    const bool hmi_time_valid =
        app_time_hmi_calendar_to_unix(hmi_calendar, &hmi_time);
    const app_time_source_t source = app_time_get_source();

    if (source != APP_TIME_SOURCE_SNTP && !app_time_is_valid())
    {
        if (!hmi_time_valid)
        {
            ESP_LOGW(TAG_DELTA_HMI_MASTER,
                     "Delta RTC at $%u..$%u is invalid; timestamp remains zero",
                     (unsigned)DELTA_HMI_RTC_READ_BASE_ADDR,
                     (unsigned)(DELTA_HMI_RTC_READ_BASE_ADDR +
                                DELTA_HMI_RTC_CALENDAR_WORDS - 1U));
            return ESP_OK;
        }

        (void)app_time_set_from_hmi_calendar(hmi_calendar);
        return ESP_OK;
    }

    if (source != APP_TIME_SOURCE_SNTP)
        return ESP_OK;

    time_t esp_time = 0;
    time(&esp_time);

    int64_t difference = hmi_time_valid
        ? ((int64_t)esp_time - (int64_t)hmi_time)
        : INT64_MAX;

    if (difference < 0 && difference != INT64_MIN)
        difference = -difference;

    const bool update_required =
        app_time_hmi_update_pending() ||
        !hmi_time_valid ||
        difference > (int64_t)DELTA_HMI_RTC_MAX_DIFFERENCE_SEC;

    if (!update_required)
    {
        app_time_mark_hmi_updated();
        return ESP_OK;
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t retry_us =
        (int64_t)DELTA_HMI_RTC_WRITE_RETRY_MS * 1000LL;

    if (last_hmi_write_attempt_us != 0 &&
        (now_us - last_hmi_write_attempt_us) < retry_us)
    {
        return ESP_OK;
    }

    last_hmi_write_attempt_us = now_us;
    return modbus_hmi_write_esp_time_to_delta();
}

static void modbus_hmi_note_result(esp_err_t err)
{
    static uint8_t failure_count = 0U;

    if (err == ESP_ERR_NOT_FOUND)
        return; /* No Modbus transaction was needed. */

    if (err == ESP_OK)
    {
        if (!g_modbus_hmi_online)
            g_modbus_hmi_full_sync_required = true;

        g_modbus_hmi_online = true;
        failure_count = 0U;
        return;
    }

    if (failure_count < 255U)
        ++failure_count;

    if (failure_count >= 3U)
    {
        g_modbus_hmi_online = false;
        g_modbus_hmi_full_sync_required = true;
    }
}

static void modbus_hmi_copy_holding_snapshot(uint16_t *destination)
{
    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
    memcpy(destination,
           slave_holding_regs,
           DELTA_HMI_CMD_REG_COUNT * sizeof(uint16_t));
    xSemaphoreGive(g_modbus_slave_mutex);
}

static void modbus_hmi_stage_command_changes(uint16_t local_start,
                                             const uint16_t *values,
                                             uint16_t quantity)
{
    bool frequency_pair_changed = false;
    int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);

    for (uint16_t offset = 0U; offset < quantity; ++offset)
    {
        uint16_t index = (uint16_t)(local_start + offset);

        if (index >= DELTA_HMI_CMD_REG_COUNT ||
            g_delta_last_commands[index] == values[offset])
            continue;

        /*
         * Registers 13..38 are ESP-owned progress-bar limits. Never accept
         * them as Delta-originated commands, even if a future read range is
         * accidentally widened.
         */
        if (index >= DELTA_HMI_BAR_FIRST_INDEX &&
            index <= DELTA_HMI_BAR_LAST_INDEX)
        {
            ESP_LOGW(TAG_DELTA_HMI_MASTER,
                     "Ignored Delta writeback to ESP-owned HR%u: value=%u",
                     index, values[offset]);
            continue;
        }

        if (index == HOLD_ADDR_FREQ_MIN || index == HOLD_ADDR_FREQ_MAX)
        {
            frequency_pair_changed = true;
            continue;
        }

        modbus_slave_stage_holding_write(index, values[offset]);
        g_delta_last_commands[index] = values[offset];
        g_delta_command_pending[index] = true;
        g_delta_command_pending_until_us[index] = now_us + 2000000LL;
    }

    if (frequency_pair_changed)
    {
        uint16_t freq_min = g_delta_last_commands[HOLD_ADDR_FREQ_MIN];
        uint16_t freq_max = g_delta_last_commands[HOLD_ADDR_FREQ_MAX];

        if (HOLD_ADDR_FREQ_MIN >= local_start &&
            HOLD_ADDR_FREQ_MIN < (uint16_t)(local_start + quantity))
            freq_min = values[HOLD_ADDR_FREQ_MIN - local_start];

        if (HOLD_ADDR_FREQ_MAX >= local_start &&
            HOLD_ADDR_FREQ_MAX < (uint16_t)(local_start + quantity))
            freq_max = values[HOLD_ADDR_FREQ_MAX - local_start];

        modbus_slave_stage_holding_write(HOLD_ADDR_FREQ_MIN, freq_min);
        modbus_slave_stage_holding_write(HOLD_ADDR_FREQ_MAX, freq_max);

        g_delta_last_commands[HOLD_ADDR_FREQ_MIN] = freq_min;
        g_delta_last_commands[HOLD_ADDR_FREQ_MAX] = freq_max;
        g_delta_command_pending[HOLD_ADDR_FREQ_MIN] = true;
        g_delta_command_pending[HOLD_ADDR_FREQ_MAX] = true;
        g_delta_command_pending_until_us[HOLD_ADDR_FREQ_MIN] = now_us + 2000000LL;
        g_delta_command_pending_until_us[HOLD_ADDR_FREQ_MAX] = now_us + 2000000LL;
    }

    xSemaphoreGive(g_modbus_slave_mutex);
}

static esp_err_t modbus_hmi_sync_command_chunk(uint16_t local_start,
                                               uint16_t quantity)
{
    uint16_t local_values[DELTA_HMI_CMD_REG_COUNT];
    uint16_t write_values[DELTA_HMI_CMD_REG_COUNT];
    bool write_required = false;
    int64_t now_us = esp_timer_get_time();

    modbus_hmi_copy_holding_snapshot(local_values);

    for (uint16_t offset = 0U; offset < quantity; ++offset)
    {
        uint16_t index = (uint16_t)(local_start + offset);
        write_values[offset] = local_values[index];

        if (g_delta_command_pending[index])
        {
            if (local_values[index] == g_delta_last_commands[index])
            {
                g_delta_command_pending[index] = false;
            }
            else if (now_us < g_delta_command_pending_until_us[index])
            {
                /* Preserve the HMI-originated value if another word in the
                 * same FC16 chunk must be written for an MQTT change. */
                write_values[offset] = g_delta_last_commands[index];
                continue;
            }
            else
            {
                g_delta_command_pending[index] = false;
            }
        }

        if (local_values[index] != g_delta_last_commands[index])
            write_required = true;
    }

    if (!write_required)
        return ESP_ERR_NOT_FOUND;

    esp_err_t err = modbus_hmi_write_holding(
        (uint16_t)(DELTA_HMI_CMD_BASE_ADDR + local_start),
        write_values,
        quantity);

    if (err == ESP_OK)
    {
        for (uint16_t offset = 0U; offset < quantity; ++offset)
        {
            uint16_t index = (uint16_t)(local_start + offset);

            if (!g_delta_command_pending[index])
                g_delta_last_commands[index] = write_values[offset];
        }
    }

    return err;
}

typedef struct
{
    uint16_t source_first;
    uint16_t delta_first;
    uint16_t quantity;
} delta_status_segment_t;

static const delta_status_segment_t g_delta_status_segments[] =
{
    /*
     * Numeric/runtime feedback.
     *
     * ESP input register n -> Delta internal register $n
     */
    {0U,   0U,   24U},   /* Input 0–23   -> Delta $0–$23 */
    {24U,  24U,  18U},   /* Includes raw control-card inputs at $26–$36. */

    /*
     * Machine information strings.
     * Twenty registers are reserved for each string.
     */
    {
        INP_ADDR_INFO_MODEL,
        INP_ADDR_INFO_MODEL,
        20U
    },

    {
        INP_ADDR_INFO_CAPACITY,
        INP_ADDR_INFO_CAPACITY,
        20U
    },

    {
        INP_ADDR_INFO_IOT_ID,
        INP_ADDR_INFO_IOT_ID,
        20U
    },

    {
        INP_ADDR_INFO_VERSION,
        INP_ADDR_INFO_VERSION,
        20U
    },

    /*
     * Heading text.
     */
    {
        INP_ADDR_HEAD_TEXT,
        INP_ADDR_HEAD_TEXT,
        20U
    },

    /*
     * Network icon, clock and date:
     * input 142–148 -> Delta $142–$148
     */
    {
        INP_ADDR_LOCAL_WIFI_RSSI_STATE,
        INP_ADDR_LOCAL_WIFI_RSSI_STATE,
        7U
    },

    /*
     * Access-point credentials.
     */
    {
        INP_ADDR_AP_SSID,
        INP_ADDR_AP_SSID,
        20U
    },

    {
        INP_ADDR_AP_PASS,
        INP_ADDR_AP_PASS,
        20U
    },

    /* Active uplink. Split the 40-character SSID to fit values[24]. */
    {INP_ADDR_STA_SSID,       INP_ADDR_STA_SSID,       20U},
    {INP_ADDR_STA_SSID + 20U, INP_ADDR_STA_SSID + 20U, 20U},
    {INP_ADDR_STA_IP,         INP_ADDR_STA_IP,         20U},
};

static esp_err_t modbus_hmi_write_status_segment(uint16_t segment_index,
                                                  bool force)
{
    if (segment_index >=
        (sizeof(g_delta_status_segments) / sizeof(g_delta_status_segments[0])))
        return ESP_ERR_INVALID_ARG;

    const delta_status_segment_t *segment = &g_delta_status_segments[segment_index];
    uint16_t values[24];
    bool changed = force;

    if (segment->quantity > (sizeof(values) / sizeof(values[0])))
        return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);

    for (uint16_t i = 0U; i < segment->quantity; ++i)
    {
        uint16_t source = (uint16_t)(segment->source_first + i);
        values[i] = slave_input_regs[source];

        if (values[i] != g_delta_last_input_regs[source])
            changed = true;
    }

    xSemaphoreGive(g_modbus_slave_mutex);

    if (!changed)
        return ESP_ERR_NOT_FOUND;

    esp_err_t err = modbus_hmi_write_holding(segment->delta_first,
                                             values,
                                             segment->quantity);

    if (err == ESP_OK)
    {
        for (uint16_t i = 0U; i < segment->quantity; ++i)
        {
            uint16_t source = (uint16_t)(segment->source_first + i);
            g_delta_last_input_regs[source] = values[i];
        }
    }

    return err;
}

static esp_err_t  modbus_hmi_full_sync(void)
{
    uint16_t commands[DELTA_HMI_CMD_REG_COUNT];
    modbus_hmi_copy_holding_snapshot(commands);

    esp_err_t err;

    /* Primary operator commands: $300–$312 */
    err = modbus_hmi_write_holding(
        DELTA_HMI_CMD_BASE_ADDR,
        commands,
        DELTA_HMI_PRIMARY_CMD_COUNT);

    if (err != ESP_OK)
        return err;

    /* ESP-owned bar ranges: $313–$338 */
    err = modbus_hmi_write_holding(
        (uint16_t)(DELTA_HMI_CMD_BASE_ADDR +
                HOLD_ADDR_BAR_MELTER_TEMP_MIN),
        &commands[HOLD_ADDR_BAR_MELTER_TEMP_MIN],
        (uint16_t)(HOLD_ADDR_BAR_PF_MAX -
                HOLD_ADDR_BAR_MELTER_TEMP_MIN + 1U));

    if (err != ESP_OK)
        return err;

    memcpy(g_delta_last_commands, commands, sizeof(g_delta_last_commands));
    memset(g_delta_command_pending, 0, sizeof(g_delta_command_pending));

    for (uint16_t i = 0U;
         i < (sizeof(g_delta_status_segments) / sizeof(g_delta_status_segments[0]));
         ++i)
    {
        err = modbus_hmi_write_status_segment(i, true);
        if (err != ESP_OK)
            return err;
    }

    return ESP_OK;
}


static esp_err_t modbus_hmi_sync_bar_ranges(bool force)
{
    enum
    {
        BAR_FIRST = HOLD_ADDR_BAR_MELTER_TEMP_MIN,
        BAR_LAST  = HOLD_ADDR_BAR_PF_MAX,
        BAR_COUNT = BAR_LAST - BAR_FIRST + 1U
    };

    static uint16_t previous_values[BAR_COUNT];
    static bool previous_valid = false;

    uint16_t current_values[BAR_COUNT];
    bool changed = force || !previous_valid;

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);

    for (uint16_t i = 0U; i < BAR_COUNT; ++i)
    {
        current_values[i] =
            slave_holding_regs[BAR_FIRST + i];

        if (!previous_valid ||
            current_values[i] != previous_values[i])
        {
            changed = true;
        }
    }

    xSemaphoreGive(g_modbus_slave_mutex);

    if (!changed)
        return ESP_ERR_NOT_FOUND;

    esp_err_t err = modbus_hmi_write_holding(
        (uint16_t)(DELTA_HMI_CMD_BASE_ADDR + BAR_FIRST),
        current_values,
        BAR_COUNT);

    if (err == ESP_OK)
    {
        memcpy(previous_values,
               current_values,
               sizeof(previous_values));

        previous_valid = true;
    }

    return err;
}


static void modbus_hmi_master_task(void *arg)
{
    (void)arg;

    uint16_t fast_commands[DELTA_HMI_FAST_CMD_COUNT];
    uint16_t cycle = 0U;
    uint16_t slow_status_segment = 1U;

    while (1)
    {
        esp_err_t err;

        if (g_modbus_hmi_full_sync_required)
        {
            err = modbus_hmi_full_sync();
            modbus_hmi_note_result(err);

            if (err == ESP_OK)
            {
                g_modbus_hmi_full_sync_required = false;
                slow_status_segment = 1U;
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }
        err = modbus_hmi_read_holding(DELTA_HMI_CMD_BASE_ADDR,
                                      DELTA_HMI_FAST_CMD_COUNT,
                                      fast_commands);
        modbus_hmi_note_result(err);

        if (err == ESP_OK)
            modbus_hmi_stage_command_changes(0U,
                                             fast_commands,
                                             DELTA_HMI_FAST_CMD_COUNT);

        /*
        * Only read the actual operator command registers.
        *
        * Holding 13–38 are bar limits owned by the ESP32.
        * They must not be read back from Delta and staged as commands.
        */
        /*
         * Run immediately after the HMI connects, then every ten seconds.
         * This replaces the previous unvalidated $500 read on every 100 ms
         * loop and actually initializes the ESP wall clock from the HMI RTC.
         */
        if ((cycle % DELTA_HMI_RTC_SERVICE_PERIOD_CYCLES) == 0U)
        {
            err = modbus_hmi_time_service();
            modbus_hmi_note_result(err);
        }

        /* MQTT/runtime-originated setpoint changes are mirrored to Delta. */
        err = modbus_hmi_sync_command_chunk(0U, DELTA_HMI_FAST_CMD_COUNT);
        modbus_hmi_note_result(err);

        /* Fast feedback every cycle. */
        err = modbus_hmi_write_status_segment(0U, false);
        modbus_hmi_note_result(err);

        /*
        * Bar limits are ESP-owned. Write them to Delta, but never read
        * them back as operator commands.
        */
        if ((cycle % 5U) == 0U)
        {
            err = modbus_hmi_sync_bar_ranges(false);
            modbus_hmi_note_result(err);
        }

        if ((cycle % 50U) == 0U)
        {
            xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);
            const uint16_t hr17 = slave_holding_regs[HOLD_ADDR_BAR_L12V_MIN];
            const uint16_t hr18 = slave_holding_regs[HOLD_ADDR_BAR_L12V_MAX];
            xSemaphoreGive(g_modbus_slave_mutex);

            ESP_LOGI(TAG_DELTA_HMI_MASTER,
                     "Bar ownership check: HR17=%u -> $%u, HR18=%u -> $%u",
                     hr17,
                     (unsigned)(DELTA_HMI_CMD_BASE_ADDR + HOLD_ADDR_BAR_L12V_MIN),
                     hr18,
                     (unsigned)(DELTA_HMI_CMD_BASE_ADDR + HOLD_ADDR_BAR_L12V_MAX));
        }

        /* One slow/status/text segment per cycle. */
        err = modbus_hmi_write_status_segment(slow_status_segment, false);
        modbus_hmi_note_result(err);

        ++slow_status_segment;
        if (slow_status_segment >=
            (sizeof(g_delta_status_segments) / sizeof(g_delta_status_segments[0])))
            slow_status_segment = 1U;

        ++cycle;
        vTaskDelay(pdMS_TO_TICKS(MODBUS_HMI_POLL_PERIOD_MS));
    }
}






static void modbus_put_ascii_1char_per_reg(uint16_t *regs,
                                           uint16_t start_addr,
                                           uint16_t max_words,
                                           const char *str)
{
    uint16_t i;

    if (regs == NULL || str == NULL)
        return;

    for (i = 0; i < max_words; i++)
    {
        if (str[i] == '\0')
            break;

        regs[start_addr + i] = (uint16_t)(uint8_t)str[i];
    }

    while (i < max_words)
    {
        regs[start_addr + i] = 0;
        i++;
    }
}



static void modbus_slave_bar_range_init_registers(void)
{
    if (g_modbus_slave_mutex == NULL)
        return;

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);

    // =========================================================
    // DEFAULT VALUES
    // =========================================================

    slave_holding_regs[HOLD_ADDR_BAR_MELTER_TEMP_MIN]   = 0;

    slave_holding_regs[HOLD_ADDR_BAR_POWER_PERCENT_MIN] = 1;
    slave_holding_regs[HOLD_ADDR_BAR_POWER_PERCENT_MAX] = 100;

    slave_holding_regs[HOLD_ADDR_BAR_L12V_MIN]          = 4000;
    slave_holding_regs[HOLD_ADDR_BAR_L12V_MAX]          = 4600;

    slave_holding_regs[HOLD_ADDR_BAR_L12A_MIN]          = 0;
    slave_holding_regs[HOLD_ADDR_BAR_L12A_MAX]          = 80;

    slave_holding_regs[HOLD_ADDR_BAR_L23V_MIN]          = 4000;
    slave_holding_regs[HOLD_ADDR_BAR_L23V_MAX]          = 4600;

    slave_holding_regs[HOLD_ADDR_BAR_L23A_MIN]          = 0;
    slave_holding_regs[HOLD_ADDR_BAR_L23A_MAX]          = 80;

    slave_holding_regs[HOLD_ADDR_BAR_L31V_MIN]          = 4000;
    slave_holding_regs[HOLD_ADDR_BAR_L31V_MAX]          = 4600;

    slave_holding_regs[HOLD_ADDR_BAR_L31A_MIN]          = 0;
    slave_holding_regs[HOLD_ADDR_BAR_L31A_MAX]          = 80;

    slave_holding_regs[HOLD_ADDR_BAR_LAVGV_MIN]         = 4000;
    slave_holding_regs[HOLD_ADDR_BAR_LAVGV_MAX]         = 4600;

    slave_holding_regs[HOLD_ADDR_BAR_LAVGA_MIN]         = 0;
    slave_holding_regs[HOLD_ADDR_BAR_LAVGA_MAX]         = 80;

    slave_holding_regs[HOLD_ADDR_BAR_KW_MIN]            = 0;
    slave_holding_regs[HOLD_ADDR_BAR_KW_MAX]            = 80;

    slave_holding_regs[HOLD_ADDR_BAR_PF_MIN]            = 0;
    slave_holding_regs[HOLD_ADDR_BAR_PF_MAX]            = 999;

    // =========================================================
    // LOAD FROM FILE
    // =========================================================

    const bool bar_file_loaded = modbus_slave_load_bar_ranges_from_file();

    ESP_LOGI(TAG_DELTA_HMI_MASTER,
             "Bar ranges initialised (%s): HR17=%u HR18=%u HR19=%u HR20=%u",
             bar_file_loaded ? "validated file" : "compiled defaults",
             slave_holding_regs[HOLD_ADDR_BAR_L12V_MIN],
             slave_holding_regs[HOLD_ADDR_BAR_L12V_MAX],
             slave_holding_regs[HOLD_ADDR_BAR_L12A_MIN],
             slave_holding_regs[HOLD_ADDR_BAR_L12A_MAX]);

    xSemaphoreGive(g_modbus_slave_mutex);
}

static void modbus_slave_info_init_registers(void)
{
    hmi_data_t snap;

    if (g_modbus_slave_mutex == NULL)
        return;

    if (!hmi_data_lock(HMI_DATA_LOCK_SHORT_TIMEOUT))
        return;

    memcpy(&snap, (const void *)&hmi_data, sizeof(hmi_data_t));
    hmi_data_unlock();

    xSemaphoreTake(g_modbus_slave_mutex, portMAX_DELAY);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_INFO_MODEL,
        20U,
        snap.machine_model);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_INFO_CAPACITY,
        20U,
        snap.machine_capacity);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_INFO_IOT_ID,
        20U,
        snap.iot_id);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_INFO_VERSION,
        20U,
        snap.version);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_HEAD_TEXT,
        20U,
        snap.hmi_head_text);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_AP_SSID,
        20U,
        snap.wifi_ap_ssid);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_AP_PASS,
        20U,
        snap.wifi_ap_pass);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_STA_SSID,
        40U,
        snap.sta_ssid);

    modbus_put_ascii_1char_per_reg(
        slave_input_regs,
        INP_ADDR_STA_IP,
        20U,
        snap.sta_ip);

    xSemaphoreGive(g_modbus_slave_mutex);
}

static void modbus_slave_write_datetime_regs_locked(bool use_24h)
{
    time_t now;
    struct tm ti;

    /*
     * Invalid / not synced time.
     *
     * AM/PM:
     * 0 = NA
     *
     * For hour/min/date/day/month, 0 can be a valid value,
     * so 0xFFFF is better for "not available".
     */
    if (!app_time_is_valid())
    {
        slave_input_regs[INP_ADDR_CLOCK_HOUR]     = 0U;
        slave_input_regs[INP_ADDR_CLOCK_MINUTE]   = 0U;
        slave_input_regs[INP_ADDR_CLOCK_NA_AM_PM] = 0U;

        slave_input_regs[INP_ADDR_DATE_DAY]       = 0U;
        slave_input_regs[INP_ADDR_DATE_DATE]      = 0U;
        slave_input_regs[INP_ADDR_DATE_MONTH]     = 0U;

        return;
    }

    time(&now);
    localtime_r(&now, &ti);

    slave_input_regs[INP_ADDR_CLOCK_MINUTE] = (uint16_t)ti.tm_min;

    if (use_24h)
    {
        slave_input_regs[INP_ADDR_CLOCK_HOUR]     = (uint16_t)ti.tm_hour;   // 0 to 23
        slave_input_regs[INP_ADDR_CLOCK_NA_AM_PM] = 0U;                    // NA
    }
    else
    {
        int hour12 = ti.tm_hour % 12;

        if (hour12 == 0)
            hour12 = 12;

        slave_input_regs[INP_ADDR_CLOCK_HOUR] = (uint16_t)hour12;          // 1 to 12

        if (ti.tm_hour >= 12)
            slave_input_regs[INP_ADDR_CLOCK_NA_AM_PM] = 2U;                // PM
        else
            slave_input_regs[INP_ADDR_CLOCK_NA_AM_PM] = 1U;                // AM
    }

    slave_input_regs[INP_ADDR_DATE_DAY]   = (uint16_t)ti.tm_wday + 1;          // 0 == "", 1 == Sun ... 6 == Sat
    slave_input_regs[INP_ADDR_DATE_DATE]  = (uint16_t)ti.tm_mday;          // 1 to 31
    slave_input_regs[INP_ADDR_DATE_MONTH] = (uint16_t)ti.tm_mon + 1;           // 0 == "", 1 == Jan ... 11 == Dec
}

bool modbus_hmi_master_is_online(void)
{
    return g_modbus_hmi_online;
}

void app_modbus_slave(void)
{
    if (g_modbus_slave_mutex == NULL)
        g_modbus_slave_mutex = xSemaphoreCreateMutex();

    if (g_modbus_slave_api_mutex == NULL)
        g_modbus_slave_api_mutex = xSemaphoreCreateMutex();

    if (g_modbus_slave_mutex == NULL || g_modbus_slave_api_mutex == NULL)
        return;

    if (esp_log_level_get("*") != ESP_LOG_NONE)
        esp_log_level_set(TAG_MODBUS_SLAVE, ESP_LOG_INFO);


    /* Physical UART is now the Delta-HMI Modbus RTU master. */
    if (modbus_slave_uart_init() != ESP_OK)
    {
        ESP_LOGE(TAG_DELTA_HMI_MASTER, "UART master init failed");
        return;
    }

    modbus_slave_init_banks();
    _Static_assert(HOLD_ADDR_LOG_OFFSET_COUNT == MODBUS_LOG_OFFSET_COUNT,
                   "Logging offset holding map count mismatch");
    _Static_assert(HOLD_ADDR_LOG_OFFSET_FIRST >= DELTA_HMI_CMD_REG_COUNT,
                   "Logging offsets overlap Delta command registers");
    _Static_assert(HOLD_ADDR_LOG_OFFSET_LAST < MODBUS_SLAVE_NUM_HOLDING_REGS,
                   "Logging offsets exceed holding bank");
    modbus_log_offsets_get(&slave_holding_regs[HOLD_ADDR_LOG_OFFSET_FIRST]);
    
    modbus_slave_bar_range_init_registers();
    modbus_slave_info_init_registers();
    modbus_slave_sync_from_runtime();

    memset(g_delta_last_commands, 0xFF, sizeof(g_delta_last_commands));
    memset(g_delta_command_pending, 0, sizeof(g_delta_command_pending));
    memset(g_delta_command_pending_until_us, 0, sizeof(g_delta_command_pending_until_us));
    memset(g_delta_last_input_regs, 0xFF, sizeof(g_delta_last_input_regs));

    g_modbus_hmi_online = false;
    g_modbus_hmi_full_sync_required = true;

    // +2 priority set (Preious - 10) to ensure that the Modbus HMI master task runs before the Modbus slave task.
    xTaskCreate(modbus_hmi_master_task,
                "modbus_hmi_master_task",
                modbus_hmi_master_task_stack_size_bytes,
                NULL,
                modbus_hmi_master_task_priority,
                NULL);
}
