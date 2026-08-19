idf_component_register(
    SRCS "src/sdcard.c"
    INCLUDE_DIRS "inc"
    REQUIRES driver fatfs sdmmc esp_driver_spi esp_driver_sdspi)
