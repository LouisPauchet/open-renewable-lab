#pragma once

#include <stdbool.h>

#include "cellular_transport.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers as a sampling_engine result sink and spawns the publish
 * task. Selects the active MQTT backend once, based on
 * net_settings.transport (esp-mqtt over WiFi; the on-modem WalterModem
 * MQTT client over cellular) - the rest of the app only ever talks to
 * this transport-agnostic bridge. Stays fully inert (no connection
 * attempted) until MQTT is enabled via the portal, per spec. Reacts to
 * MQTT settings changes (enable/host/credentials/batch mode/...)
 * without requiring a reboot; a transport change does require one,
 * same as the rest of net_manager.
 *
 * When mqtt.batch_enabled is set, publishes are buffered and only
 * actually sent (connect -> drain buffer -> disconnect) once per
 * mqtt.batch_interval_ms, trading latency for radio-on time. When
 * false, the previous always-connected/immediate-publish behavior is
 * used. Either way, topics are "<topic_prefix>/<device_id>/<name>". */
esp_err_t mqttc_init(void);

bool mqttc_is_ready(void);

/* Queues a GNSS fix for publish to "<topic_prefix>/<device_id>/position"
 * - respects the same batching behavior as regular sensor publishes.
 * Only the most recent pending fix is kept if called again before the
 * previous one is sent. */
esp_err_t mqttc_publish_position(const gnss_fix_t *fix);

#ifdef __cplusplus
}
#endif
