#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aggregator.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the periodic GNSS position task. No-ops (returns ESP_OK
 * without spawning anything) unless net_settings.transport ==
 * TRANSPORT_CELLULAR, since Walter's GNSS lives on the cellular modem
 * chip - only call this after cellular_transport_init() has
 * succeeded. position.enabled/sample_interval_ms/log_interval_ms/
 * aggregate_mask (config_store) are checked every cycle and can be
 * changed without a reboot; only the transport check itself is fixed
 * at boot, consistent with the rest of the network stack.
 *
 * Requires a physical GNSS antenna connected to the modem - without
 * one, gnssPerformAction() calls just fail/time out repeatedly
 * (logged, harmless) rather than ever producing a fix. Firmware has no
 * way to detect antenna presence itself; the portal surfaces this as a
 * plain warning next to the Position reporting settings instead. */
esp_err_t gnss_position_init(void);

/* ---------------------------------------------------------------------
 * Deep sleep support (power_manager) - mirrors sampling_engine's own
 * sleep-state API (see sampling_engine.h). Nothing below does anything
 * unless power_manager calls it.
 * ------------------------------------------------------------------- */

typedef struct {
    aggregator_t lat_agg;
    aggregator_t lon_agg;
    aggregator_t alt_agg;
    aggregator_t hprec_agg;
    int64_t window_timestamp_unix; /* timestamp of the most recent fix folded into this window */
    bool window_time_synced;
    int64_t next_log_due_unix; /* wall-clock deadline to finalize/log this window */
    bool next_log_due_is_synced;
} gnss_sleep_entry_t;

/* Snapshots the in-flight position window (four Welford accumulators +
 * its next-log-due deadline, converted to wall-clock time) so
 * power_manager can persist it across a deep sleep. Returns false (out
 * left untouched) if no window is currently in flight - i.e. there is
 * nothing worth persisting, same as an idle variable in sampling_engine. */
bool gnss_position_get_sleep_state(gnss_sleep_entry_t *out);

/* Hands back state captured by a previous boot's
 * gnss_position_get_sleep_state() so an in-flight window isn't lost
 * across a deep sleep. Must be called before gnss_position_init() -
 * consumed once by the task's first loop iteration. */
void gnss_position_restore_sleep_state(const gnss_sleep_entry_t *in);

typedef struct {
    /* True: position reporting is enabled and samples faster than it
     * logs without allow_skip_during_sleep - deep sleep must not engage
     * at all right now. blocked_reason is always "Position". */
    bool blocked;
    char blocked_reason[40];

    bool has_schedulable;    /* false: position reporting is disabled, next_due_unix is meaningless */
    bool next_due_is_synced; /* false: system clock wasn't synced, next_due_unix is meaningless */
    int64_t next_due_unix;   /* wall-clock deadline power_manager must wake by */
} gnss_position_sleep_status_t;

/* Reports whether position reporting currently forbids deep sleep
 * outright, and otherwise the wall-clock deadline power_manager must
 * wake by to keep it on schedule. */
gnss_position_sleep_status_t gnss_position_get_sleep_status(void);

#ifdef __cplusplus
}
#endif
