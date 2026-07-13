idf_component_register(
    SRCS "web_portal.c" "wp_common.c" "auth.c" "api_variables.c" "api_settings.c" "api_status.c" "api_bus.c"
         "dns_hijack.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/index.html"
    REQUIRES esp_http_server config_store sampling_engine sd_logger net_manager time_sync mqtt_client sdi12_bus
              i2c_bus json esp_timer lwip esp_hw_support
)
