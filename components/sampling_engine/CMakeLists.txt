idf_component_register(
    SRCS "sampling_engine.c" "aggregator.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store esp_timer
)
