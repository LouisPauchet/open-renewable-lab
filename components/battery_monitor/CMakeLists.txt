idf_component_register(
    SRCS "battery_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store i2c_sensors mqtt_client freertos
)
