idf_component_register(SRCS "./src/main.c" "./src/global.c" "./src/file_io.c"
                            "./src/modbus_master.c" "./src/hmi.c" "./src/esp_log_tags.c"
                             "./src/littlefs.c"
                             "./src/modbus_slave.c" "./src/date_time.c" "./src/event_log.c" "./src/modbus_slave_register.c" "./src/mqtt_file_uploader.c" 
                    INCLUDE_DIRS 
                    "./inc"
                    REQUIRES 
                    ota_mgr
                    driver
                    esp_timer 
                    esp_netif 
                    esp_event 
                    nvs_flash 
                    mqtt_lib 
                    json
                    joltwallet__littlefs 
                    sdcard
                    ethernet
                    file_manager)
