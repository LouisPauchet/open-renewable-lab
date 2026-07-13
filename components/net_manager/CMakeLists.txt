idf_component_register(
    SRCS "net_manager.c" "ap_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash config_store
)
