idf_component_register(
    SRCS "i2c_sensor_registry.c" "ads111x.c" "generic_i2c.c"
    INCLUDE_DIRS "include"
    REQUIRES i2c_bus config_store freertos
)
