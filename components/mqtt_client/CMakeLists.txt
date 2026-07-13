idf_component_register(
    SRCS "mqtt_client_bridge.c" "backend_esp_mqtt.c" "backend_walter_mqtt.cpp"
    INCLUDE_DIRS "include"
    REQUIRES config_store sampling_engine mqtt mbedtls json esp_event esp_system esp_timer cellular_transport
              device_id
)
