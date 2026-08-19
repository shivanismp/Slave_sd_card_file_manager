#ifndef __HMI_H__
#define __HMI_H__

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#define HMI_DATA_LOCK_SHORT_TIMEOUT   1
#define HMI_DATA_SNAPSHOT_TIMEOUT     1

typedef enum
{
    IDLE = 0,
    OFF = 1,
    ON = 2,
    TEMP_CUT_OFF = 3,
    TEMP_CUT_ON = 4,
    ERROR = 5
} MACHINE_TRIGGER_ENUM;

typedef struct
{
    volatile uint16_t power_percent;
    // LOCAL CONTROL
    volatile MACHINE_TRIGGER_ENUM machine_state;
    // ACTUAL SLAVE FEEDBACK
    volatile MACHINE_TRIGGER_ENUM machine_state_fb;
    volatile uint16_t control_mode;
    volatile uint16_t freq_min;
    volatile uint16_t freq_max;
    
    volatile uint16_t min_pot;
    volatile uint16_t max_pot;

    volatile uint16_t min_ct;
    volatile uint16_t max_ct;

    volatile uint16_t min_pt;
    volatile uint16_t max_pt;

    volatile uint16_t auto_power_percent;
    volatile uint16_t auto_power_percent_fb;
    volatile bool auto_power_percent_force_from_mb_slave;

    volatile uint16_t melter_temp;
    volatile uint16_t melter_set_temp;
    
    volatile uint16_t chiller_temp;
    volatile uint16_t igbt_plate_temp;
    volatile uint16_t coil_temp;
    
    volatile uint16_t pwm_freq;
    volatile uint16_t line_1_v;
    volatile uint16_t line_1_a;
    volatile uint16_t line_2_v;
    volatile uint16_t line_2_a;
    volatile uint16_t line_3_v;
    volatile uint16_t line_3_a;
    volatile uint16_t avg_v;
    volatile uint16_t avg_a;
    volatile uint16_t avg_kw;
    volatile uint16_t avg_pf;

    uint16_t wifi_rssi_state;

    uint8_t error_leds[11];

    char hmi_head_text[20];
    char machine_model[20];
    char machine_capacity[20];
    char iot_id[20];
    char version[20];

    char wifi_ap_ssid[20];
    char wifi_ap_pass[20];

} hmi_data_t;

extern volatile hmi_data_t hmi_data;
void hmi_data_mutex_init(void);

bool file_write_melter_set_temp(volatile uint16_t vaule);
bool file_read_melter_set_temp(void);


bool hmi_data_lock(TickType_t timeout_ms);
void hmi_data_unlock(void);
bool hmi_data_get_snapshot(hmi_data_t *out, TickType_t timeout);

void app_hmi(void);



#endif
