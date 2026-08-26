idf_component_register(
    SRCS "src/wifi_cfg.c" "src/wifi_db.c" "src/wifi_mgr.c"
    INCLUDE_DIRS "inc"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_http_client)
