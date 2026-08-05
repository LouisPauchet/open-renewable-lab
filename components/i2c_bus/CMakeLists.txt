idf_component_register(
    SRCS "i2c_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES board_pins esp_driver_i2c esp_rom freertos
)
