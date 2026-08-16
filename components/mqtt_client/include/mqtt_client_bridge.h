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

/* ---------------------------------------------------------------------
 * Deep sleep support (power_manager) - mirrors the other components'
 * sleep-state APIs, but there's no accumulator to snapshot/restore here
 * (each publish is either already sent, or sitting in the batch buffer/
 * pending-position/pending-battery slots, all of which just get flushed
 * before sleeping rather than persisted across it - see the design note
 * in power_settings_t). Nothing below does anything unless power_manager
 * calls it.
 * ------------------------------------------------------------------- */

typedef struct {
    /* True: MQTT is enabled in non-batch (always-connected) mode - deep
     * sleep must not engage at all right now. Non-batch mode's whole
     * point is staying persistently connected for low-latency delivery,
     * which a sleep cycle would defeat; only batch mode's own
     * connect-drain-disconnect rhythm is compatible with sleeping in
     * between. blocked_reason is always "MQTT (non-batch)". */
    bool blocked;
    char blocked_reason[40];

    bool has_schedulable;     /* true: MQTT is enabled in batch mode - bounds sleep by the next transmit window */
    bool next_due_is_synced;  /* false: system clock wasn't synced, next_due_unix is meaningless */
    int64_t next_due_unix;    /* wall-clock deadline of the next batch transmit window */
} mqttc_sleep_status_t;

/* Reports whether MQTT currently forbids deep sleep outright, and
 * otherwise the wall-clock deadline of the next batch transmit window. */
mqttc_sleep_status_t mqttc_get_sleep_status(void);

/* True if there's a buffered batch sample or a pending position/battery
 * reading that hasn't been sent yet. Purely informational (e.g. for a
 * portal status line) - mqttc_flush_now() below should be called
 * unconditionally before a deep sleep regardless of this. */
bool mqttc_has_pending_data(void);

/* Forces an immediate batch transmit (connect, send everything
 * buffered/pending, disconnect) regardless of mqtt.batch_interval_ms,
 * so nothing is lost to a deep sleep. Blocks the calling task until the
 * attempt completes (or a generous internal timeout elapses) - safe to
 * call from any task, not just mqtt_publish_task itself. A no-op that
 * returns immediately when MQTT is disabled or in non-batch mode
 * (nothing to flush: non-batch mode publishes immediately and blocks
 * sleep outright anyway, see mqttc_get_sleep_status()). */
void mqttc_flush_now(void);

#ifdef __cplusplus
}
#endif
