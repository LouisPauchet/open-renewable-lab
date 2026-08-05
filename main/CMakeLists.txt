idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES config_store sampling_engine stub_sensor sd_logger net_manager web_portal time_sync mqtt_client
              sdi12_bus i2c_bus onboard_i2c_bus i2c_sensors cellular_transport gnss_position device_id spi_flash
              board_pins battery_monitor
)
