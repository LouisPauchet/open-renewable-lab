#pragma once

/* Private to mqtt_client - not part of the component's public include/.
 * Each transport (WiFi/esp-mqtt, cellular/WalterModem) implements this
 * same vtable so mqtt_client_bridge.c stays transport-agnostic. */

#include <stdbool.h>

#include "config_schema.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*init)(const mqtt_settings_t *cfg);
    esp_err_t (*connect)(void);
    esp_err_t (*disconnect)(void);
    esp_err_t (*publish)(const char *topic, const char *payload, int qos, bool retain);
    bool (*is_connected)(void);
    esp_err_t (*deinit)(void);
} mqttc_backend_vtable_t;

const mqttc_backend_vtable_t *backend_esp_mqtt_get_vtable(void);

#ifdef __cplusplus
}
#endif
