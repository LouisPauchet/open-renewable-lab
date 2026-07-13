idf_component_register(
    SRCS "sd_logger.c"
    INCLUDE_DIRS "include"
    REQUIRES board_pins sampling_engine fatfs sdmmc esp_driver_sdmmc esp_system device_id
)
