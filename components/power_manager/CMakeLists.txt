idf_component_register(
    SRCS "power_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store sampling_engine gnss_position battery_monitor mqtt_client sd_logger net_manager time_sync esp_hw_support freertos
)
