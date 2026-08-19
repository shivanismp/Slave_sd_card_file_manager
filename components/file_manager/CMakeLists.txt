idf_component_register(
    SRCS "file_manager.c" "sd_card_file_manager.c"
    INCLUDE_DIRS "."
    REQUIRES spiffs vfs fatfs esp_http_server joltwallet__littlefs
)
