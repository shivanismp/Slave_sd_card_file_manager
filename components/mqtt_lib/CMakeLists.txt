idf_component_register(
    SRCS "src/mqtt.c"
    INCLUDE_DIRS "inc"

    PRIV_INCLUDE_DIRS "../../main/inc"          

    REQUIRES mqtt esp_app_format ethernet)
