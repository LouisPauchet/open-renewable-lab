idf_component_register(
    SRCS "web_portal.c" "wp_common.c" "auth.c" "api_variables.c" "api_settings.c" "api_status.c" "dns_hijack.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/index.html"
    REQUIRES esp_http_server config_store sampling_engine sd_logger net_manager json esp_timer lwip esp_hw_support
)
