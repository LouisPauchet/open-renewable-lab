idf_component_register(
    SRCS "gnss_position.c"
    INCLUDE_DIRS "include"
    REQUIRES cellular_transport config_store sd_logger mqtt_client sampling_engine time_sync freertos
)
