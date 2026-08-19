#ifndef __ETHERNET_H__
#define __ETHERNET_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HMI Card Rev 3.0 default W6100 wiring.
 * SPI2 is shared with the SD card.
 *
 * Polling mode is intentionally the default during bring-up. The W6100 INTP
 * line can be enabled later after GPIO3 routing and idle-high behavior are
 * confirmed with a meter or oscilloscope.
 */
#ifndef ETHERNET_SPI_HOST
#define ETHERNET_SPI_HOST          SPI2_HOST
#endif

#ifndef ETHERNET_PIN_MOSI
#define ETHERNET_PIN_MOSI          GPIO_NUM_23
#endif

#ifndef ETHERNET_PIN_MISO
#define ETHERNET_PIN_MISO          GPIO_NUM_18
#endif

#ifndef ETHERNET_PIN_SCLK
#define ETHERNET_PIN_SCLK          GPIO_NUM_19
#endif

#ifndef ETHERNET_PIN_CS
#define ETHERNET_PIN_CS            GPIO_NUM_21
#endif

#ifndef ETHERNET_PIN_RESET
#define ETHERNET_PIN_RESET         GPIO_NUM_2
#endif

#ifndef ETHERNET_PIN_INTERRUPT
#define ETHERNET_PIN_INTERRUPT     GPIO_NUM_NC
#endif

#ifndef ETHERNET_SPI_CLOCK_HZ
#define ETHERNET_SPI_CLOCK_HZ      (10 * 1000 * 1000)
#endif

#ifndef ETHERNET_POLL_PERIOD_MS
#define ETHERNET_POLL_PERIOD_MS    20
#endif

typedef struct
{
    spi_host_device_t spi_host;
    gpio_num_t pin_mosi;
    gpio_num_t pin_miso;
    gpio_num_t pin_sclk;
    gpio_num_t pin_cs;
    gpio_num_t pin_reset;
    gpio_num_t pin_interrupt; /* Set to GPIO_NUM_NC to use polling. */
    int spi_clock_hz;
    uint32_t poll_period_ms;
} ethernet_cfg_t;

/** Initialize the W6100, attach it to esp-netif and start DHCP. */
esp_err_t ethernet_init(void);

/** Wait until DHCP assigns an IPv4 address. */
esp_err_t ethernet_wait_for_ip(uint32_t timeout_ms);

/** Print the current DHCP-client state. */
esp_err_t ethernet_log_dhcp_status(void);

/** Stop and restart DHCP discovery. */
esp_err_t ethernet_restart_dhcp(void);

bool ethernet_is_link_up(void);
bool ethernet_has_ip(void);

/** Print current link, IP, mask and gateway information. */
void ethernet_log_status(void);

#ifdef __cplusplus
}
#endif

#endif /* __ETHERNET_H__ */
