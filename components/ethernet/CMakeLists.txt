idf_component_register(
    SRCS "src/ethernet.c"
    INCLUDE_DIRS "inc"
    REQUIRES driver esp_netif esp_eth w6100 esp_driver_spi)
