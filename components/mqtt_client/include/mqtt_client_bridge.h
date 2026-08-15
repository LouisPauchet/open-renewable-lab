#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sampling_engine.h" /* field_aggregate_t */

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
 * used. Either way, topics are "<topic_prefix>/<device_id>/<name>" -
 * unless mqtt.flat_telemetry is set, in which case every publish goes
 * to the literal topic_prefix with flat "<name>_<aggregate>" JSON keys
 * (the shape platforms like ThingsBoard/AWS IoT expect from their one
 * fixed device telemetry topic). */
esp_err_t mqttc_init(void);

bool mqttc_is_ready(void);

/* Queues one aggregated GNSS position record (latitude/longitude/
 * elevation/horizontal-precision, each already reduced to
 * raw/mean/min/max/stddev over the log interval - see gnss_position.c)
 * for publish to "<topic_prefix>/<device_id>/position" (or the literal
 * topic_prefix under mqtt.flat_telemetry) - respects the same batching
 * behavior as regular sensor publishes. Only the most recent pending
 * record is kept if called again before the previous one is sent. */
esp_err_t mqttc_publish_position(int64_t timestamp_unix, bool time_is_synced, uint32_t sample_count,
                                  uint8_t aggregate_mask, const field_aggregate_t *latitude,
                                  const field_aggregate_t *longitude, const field_aggregate_t *elevation_m,
                                  const field_aggregate_t *h_precision_m);

/* Queues a battery voltage reading for publish to
 * "<topic_prefix>/<device_id>/battery" (or the literal topic_prefix
 * under mqtt.flat_telemetry) - same batching/most-recent-wins behavior
 * as mqttc_publish_position() above. See battery_monitor.c. */
esp_err_t mqttc_publish_battery(double voltage);

#ifdef __cplusplus
}
#endif
