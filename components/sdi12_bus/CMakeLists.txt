idf_component_register(
    SRCS "sdi12_bus.c" "sdi12_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES board_pins config_store esp_driver_gpio esp_rom freertos
)
