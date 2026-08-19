#include "ethernet.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_eth.h"
#include "esp_eth_mac_w6100.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy_w6100.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

static const char *TAG = "ETH_W6100";

#define ETH_LINK_BIT    BIT0
#define ETH_IP_BIT      BIT1

static EventGroupHandle_t s_event_group;

static esp_eth_handle_t s_eth_handle;
static esp_eth_mac_t *s_mac;
static esp_eth_phy_t *s_phy;
static esp_netif_t *s_eth_netif;
static esp_eth_netif_glue_handle_t s_eth_glue;

static esp_event_handler_instance_t s_eth_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;

static bool s_driver_installed;
static bool s_handlers_registered;
static bool s_started;
static bool s_link_up;
static bool s_has_ip;

static esp_err_t ethernet_init_ex(const ethernet_cfg_t *cfg);


static const char *ethernet_dhcp_status_name(esp_netif_dhcp_status_t status)
{
    switch (status)
    {
    case ESP_NETIF_DHCP_INIT:
        return "INIT";
    case ESP_NETIF_DHCP_STARTED:
        return "STARTED";
    case ESP_NETIF_DHCP_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t ethernet_ensure_dhcp_client(void)
{
    if (s_eth_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    esp_err_t err = esp_netif_dhcpc_get_status(s_eth_netif, &status);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_netif_dhcpc_get_status failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "DHCP client status: %s", ethernet_dhcp_status_name(status));

    if (status == ESP_NETIF_DHCP_STARTED)
    {
        return ESP_OK;
    }

    err = esp_netif_dhcpc_start(s_eth_netif);
    if (err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
    {
        ESP_LOGI(TAG, "DHCP client started");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "esp_netif_dhcpc_start failed: %s", esp_err_to_name(err));
    return err;
}

static ethernet_cfg_t ethernet_default_config(void)
{
    const ethernet_cfg_t cfg = {
        .spi_host = ETHERNET_SPI_HOST,
        .pin_mosi = ETHERNET_PIN_MOSI,
        .pin_miso = ETHERNET_PIN_MISO,
        .pin_sclk = ETHERNET_PIN_SCLK,
        .pin_cs = ETHERNET_PIN_CS,
        .pin_reset = ETHERNET_PIN_RESET,
        .pin_interrupt = ETHERNET_PIN_INTERRUPT,
        .spi_clock_hz = ETHERNET_SPI_CLOCK_HZ,
        .poll_period_ms = ETHERNET_POLL_PERIOD_MS,
    };

    return cfg;
}

static esp_err_t ethernet_hardware_reset(const ethernet_cfg_t *cfg)
{
    if (cfg->pin_reset == GPIO_NUM_NC)
    {
        ESP_LOGW(TAG, "No W6100 hardware-reset GPIO configured");
        return ESP_OK;
    }

    gpio_config_t reset_cfg = {
        .pin_bit_mask = 1ULL << cfg->pin_reset,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&reset_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "reset GPIO configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    /* RSTN is active-low. */
    gpio_set_level(cfg->pin_reset, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    gpio_set_level(cfg->pin_reset, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_LOGI(TAG, "W6100 hardware reset completed on GPIO %d", cfg->pin_reset);
    return ESP_OK;
}

static esp_err_t ethernet_prepare_spi_bus(const ethernet_cfg_t *cfg)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = cfg->pin_mosi,
        .miso_io_num = cfg->pin_miso,
        .sclk_io_num = cfg->pin_sclk,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(cfg->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "SPI%d bus initialized by Ethernet", (int)cfg->spi_host + 1);
        return ESP_OK;
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        /* Expected when the SD-card driver initialized the shared bus first. */
        ESP_LOGI(TAG, "SPI bus already initialized; sharing it with the SD card");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return err;
}


static bool ethernet_mac_is_valid(const uint8_t mac[6])
{
    if (mac == NULL)
    {
        return false;
    }

    bool all_zero = true;
    bool all_ff = true;

    for (size_t i = 0; i < 6; ++i)
    {
        if (mac[i] != 0x00)
        {
            all_zero = false;
        }

        if (mac[i] != 0xFF)
        {
            all_ff = false;
        }
    }

    /*
     * Reject zero, broadcast and multicast addresses.
     * A valid Ethernet station address must have bit 0 of octet 0 cleared.
     */
    return !all_zero && !all_ff && ((mac[0] & 0x01U) == 0U);
}

static esp_err_t ethernet_assign_mac_address(void)
{
    if (s_eth_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t mac[6] = {0};

    /*
     * Obtain the Ethernet-specific MAC derived by ESP-IDF from the
     * ESP32-C6 factory-programmed base MAC address.
     */
    esp_err_t err = esp_read_mac(mac, ESP_MAC_ETH);

    if (err != ESP_OK || !ethernet_mac_is_valid(mac))
    {
        ESP_LOGW(TAG,
                 "ESP_MAC_ETH unavailable or invalid; deriving a local MAC "
                 "from the Wi-Fi station MAC");

        uint8_t wifi_mac[6] = {0};

        err = esp_read_mac(wifi_mac, ESP_MAC_WIFI_STA);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "Unable to read ESP32 base MAC: %s",
                     esp_err_to_name(err));
            return err;
        }

        err = esp_derive_local_mac(mac, wifi_mac);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "Unable to derive Ethernet MAC: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    if (!ethernet_mac_is_valid(mac))
    {
        ESP_LOGE(TAG,
                 "Generated Ethernet MAC is invalid: "
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
        return ESP_FAIL;
    }

    /*
     * This must happen while the Ethernet driver is stopped, before the
     * driver is attached to esp-netif and before esp_eth_start().
     */
    err = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "ETH_CMD_S_MAC_ADDR failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    uint8_t readback[6] = {0};

    err = esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, readback);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "ETH_CMD_G_MAC_ADDR failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    if (memcmp(mac, readback, sizeof(mac)) != 0 ||
        !ethernet_mac_is_valid(readback))
    {
        ESP_LOGE(TAG,
                 "W6100 MAC verification failed. Expected "
                 "%02X:%02X:%02X:%02X:%02X:%02X, read "
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5],
                 readback[0], readback[1], readback[2],
                 readback[3], readback[4], readback[5]);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "W6100 MAC assigned: %02X:%02X:%02X:%02X:%02X:%02X",
             readback[0], readback[1], readback[2],
             readback[3], readback[4], readback[5]);

    return ESP_OK;
}

static void ethernet_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id)
    {
    case ETHERNET_EVENT_START:
        s_started = true;
        ESP_LOGI(TAG, "Ethernet started");
        break;

    case ETHERNET_EVENT_CONNECTED:
    {
        s_link_up = true;
        xEventGroupSetBits(s_event_group, ETH_LINK_BIT);
        ESP_LOGI(TAG, "Ethernet link up");

        uint8_t mac[6] = {0};
        if (s_eth_handle != NULL &&
            esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2],
                     mac[3], mac[4], mac[5]);
        }

        /* ESP-NETIF normally starts DHCP through its default Ethernet
         * event handler. This explicit check makes the state visible and
         * starts it if another application component left it stopped. */
        esp_err_t dhcp_err = ethernet_ensure_dhcp_client();
        if (dhcp_err != ESP_OK)
        {
            ESP_LOGE(TAG, "Unable to start DHCP client: %s",
                     esp_err_to_name(dhcp_err));
        }
        break;
    }

    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        s_has_ip = false;
        xEventGroupClearBits(s_event_group, ETH_LINK_BIT | ETH_IP_BIT);
        ESP_LOGW(TAG, "Ethernet link down");
        break;

    case ETHERNET_EVENT_STOP:
        s_started = false;
        s_link_up = false;
        s_has_ip = false;
        xEventGroupClearBits(s_event_group, ETH_LINK_BIT | ETH_IP_BIT);
        ESP_LOGI(TAG, "Ethernet stopped");
        break;

    default:
        break;
    }
}

static void ethernet_got_ip_handler(void *arg,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

    s_has_ip = true;
    xEventGroupSetBits(s_event_group, ETH_IP_BIT);

    ESP_LOGI(TAG, "Ethernet DHCP address acquired");
    ESP_LOGI(TAG, "IP:      " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
}

static void ethernet_unregister_handlers(void)
{
    if (!s_handlers_registered)
    {
        return;
    }

    esp_event_handler_instance_unregister(ETH_EVENT,
                                          ESP_EVENT_ANY_ID,
                                          s_eth_event_instance);

    esp_event_handler_instance_unregister(IP_EVENT,
                                          IP_EVENT_ETH_GOT_IP,
                                          s_ip_event_instance);

    s_handlers_registered = false;
}

static void ethernet_release_driver_objects(void)
{
    if (s_eth_glue != NULL)
    {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }

    if (s_driver_installed && s_eth_handle != NULL)
    {
        esp_err_t err = esp_eth_driver_uninstall(s_eth_handle);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_eth_driver_uninstall failed: %s", esp_err_to_name(err));
        }
        else
        {
            s_driver_installed = false;
            s_eth_handle = NULL;
        }
    }

    if (!s_driver_installed)
    {
        if (s_phy != NULL)
        {
            s_phy->del(s_phy);
            s_phy = NULL;
        }

        if (s_mac != NULL)
        {
            s_mac->del(s_mac);
            s_mac = NULL;
        }
    }

    if (s_eth_netif != NULL)
    {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    /*
     * SPI2 is shared with the SD card. Do not free it here even when this
     * module initialized it first; another SPI device may still be attached.
     */
}

esp_err_t ethernet_init(void)
{
    const ethernet_cfg_t cfg = ethernet_default_config();
    return ethernet_init_ex(&cfg);
}

static esp_err_t ethernet_init_ex(const ethernet_cfg_t *cfg)
{
    if (s_started || s_driver_installed)
    {
        return ESP_OK;
    }

    if (cfg == NULL ||
        cfg->spi_clock_hz <= 0 ||
        cfg->pin_mosi == GPIO_NUM_NC ||
        cfg->pin_miso == GPIO_NUM_NC ||
        cfg->pin_sclk == GPIO_NUM_NC ||
        cfg->pin_cs == GPIO_NUM_NC)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_event_group == NULL)
    {
        s_event_group = xEventGroupCreate();
        if (s_event_group == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err;

    /*
     * esp_netif_init() and esp_event_loop_create_default() must already have
     * been called once by app_main before Ethernet or Wi-Fi is initialized.
     */

    /* Keep CS inactive while resetting and before the SPI device is created. */
    gpio_set_direction(cfg->pin_cs, GPIO_MODE_OUTPUT);
    gpio_set_level(cfg->pin_cs, 1);

    err = ethernet_hardware_reset(cfg);
    if (err != ESP_OK)
    {
        goto fail;
    }

    err = ethernet_prepare_spi_bus(cfg);
    if (err != ESP_OK)
    {
        goto fail;
    }

    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = cfg->spi_clock_hz,
        .spics_io_num = cfg->pin_cs,
        .queue_size = 16,
    };

    eth_w6100_config_t w6100_config =
        ETH_W6100_DEFAULT_CONFIG(cfg->spi_host, &spi_devcfg);

    if (cfg->pin_interrupt != GPIO_NUM_NC)
    {
        w6100_config.base.int_gpio_num = cfg->pin_interrupt;
        w6100_config.base.poll_period_ms = 0;
    }
    else
    {
        w6100_config.base.int_gpio_num = -1;
        w6100_config.base.poll_period_ms =
            (cfg->poll_period_ms > 0) ? cfg->poll_period_ms : ETHERNET_POLL_PERIOD_MS;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    /* Reset is performed manually above. */
    phy_config.reset_gpio_num = -1;

    s_mac = esp_eth_mac_new_w6100(&w6100_config, &mac_config);
    if (s_mac == NULL)
    {
        ESP_LOGE(TAG, "esp_eth_mac_new_w6100 failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_phy = esp_eth_phy_new_w6100(&phy_config);
    if (s_phy == NULL)
    {
        ESP_LOGE(TAG, "esp_eth_phy_new_w6100 failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(s_mac, s_phy);

    err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned int)err);
        goto fail;
    }
    s_driver_installed = true;

    /*
     * SPI Ethernet chips do not contain an ESP32 factory MAC address.
     * Assign a unique MAC before esp-netif attachment and before start.
     */
    err = ethernet_assign_mac_address();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to assign W6100 MAC address: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (s_eth_netif == NULL)
    {
        ESP_LOGE(TAG, "esp_netif_new failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_glue == NULL)
    {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed");
        err = ESP_FAIL;
        goto fail;
    }

    err = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_netif_set_hostname(s_eth_netif, "hmi-card-v3");
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s", esp_err_to_name(err));
    }

    err = esp_event_handler_instance_register(ETH_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              ethernet_event_handler,
                                              NULL,
                                              &s_eth_event_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ETH event handler registration failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_ETH_GOT_IP,
                                              ethernet_got_ip_handler,
                                              NULL,
                                              &s_ip_event_instance);
    if (err != ESP_OK)
    {
        esp_event_handler_instance_unregister(ETH_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              s_eth_event_instance);
        ESP_LOGE(TAG, "IP event handler registration failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_handlers_registered = true;

    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_started = true;

    ESP_LOGI(TAG,
             "W6100 initialized: host=%d CS=%d INT=%d RST=%d clock=%d Hz mode=%s",
             (int)cfg->spi_host,
             (int)cfg->pin_cs,
             (int)cfg->pin_interrupt,
             (int)cfg->pin_reset,
             cfg->spi_clock_hz,
             (cfg->pin_interrupt == GPIO_NUM_NC) ? "polling" : "interrupt");

    return ESP_OK;

fail:
    ethernet_unregister_handlers();

    if (s_started && s_eth_handle != NULL)
    {
        esp_eth_stop(s_eth_handle);
    }

    ethernet_release_driver_objects();

    s_started = false;
    s_link_up = false;
    s_has_ip = false;

    if (s_event_group != NULL)
    {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    return err;
}

esp_err_t ethernet_wait_for_ip(uint32_t timeout_ms)
{
    if (s_event_group == NULL || !s_driver_installed)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_has_ip)
    {
        return ESP_OK;
    }

    const TickType_t wait_ticks =
        (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           ETH_IP_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           wait_ticks);

    return ((bits & ETH_IP_BIT) != 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}


esp_err_t ethernet_log_dhcp_status(void)
{
    if (s_eth_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    esp_err_t err = esp_netif_dhcpc_get_status(s_eth_netif, &status);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Unable to read DHCP status: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "DHCP client status: %s", ethernet_dhcp_status_name(status));
    return ESP_OK;
}

esp_err_t ethernet_restart_dhcp(void)
{
    if (s_eth_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_eth_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGE(TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    s_has_ip = false;
    if (s_event_group != NULL)
    {
        xEventGroupClearBits(s_event_group, ETH_IP_BIT);
    }

    err = esp_netif_dhcpc_start(s_eth_netif);
    if (err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
    {
        ESP_LOGI(TAG, "DHCP discovery restarted");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to restart DHCP: %s", esp_err_to_name(err));
    return err;
}

bool ethernet_is_link_up(void)
{
    return s_link_up;
}

bool ethernet_has_ip(void)
{
    return s_has_ip;
}

void ethernet_log_status(void)
{
    ESP_LOGI(TAG,
             "started=%s link=%s dhcp_ip=%s",
             s_started ? "yes" : "no",
             s_link_up ? "up" : "down",
             s_has_ip ? "yes" : "no");

    if (s_eth_netif != NULL)
    {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK)
        {
            ESP_LOGI(TAG, "IP:      " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip_info.netmask));
            ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info.gw));
        }
    }
}
