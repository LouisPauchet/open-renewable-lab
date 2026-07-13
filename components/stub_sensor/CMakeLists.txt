idf_component_register(
    SRCS "stub_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store esp_timer esp_hw_support
)
