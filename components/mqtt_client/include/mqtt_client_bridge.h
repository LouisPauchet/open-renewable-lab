#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers as a sampling_engine result sink and spawns the publish
 * task. Selects the active MQTT backend once, based on
 * net_settings.transport (esp-mqtt over WiFi; the on-modem WalterModem
 * MQTT client over cellular, once cellular_transport lands) - the rest
 * of the app only ever talks to this transport-agnostic bridge. Stays
 * fully inert (no connection attempted) until MQTT is enabled via the
 * portal, per spec. Reacts to MQTT settings changes (enable/host/
 * credentials/...) without requiring a reboot; a transport change does
 * require one, same as the rest of net_manager. */
esp_err_t mqttc_init(void);

bool mqttc_is_ready(void);

#ifdef __cplusplus
}
#endif
