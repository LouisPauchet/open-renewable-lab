#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the periodic battery-voltage monitor task. Unlike
 * gnss_position_init() (which requires cellular transport
 * specifically), this always spawns the task - the LTC4015 is read
 * over the external I2C bus, which both board variants provide; a
 * board with no LTC4015 present just fails each read gracefully
 * (logged, non-fatal), same as any other I2C sensor with nothing at
 * its address. battery.enabled/interval_ms (config_store) are checked
 * every cycle and can be changed without a reboot. Only call after
 * i2c_bus_init(). */
esp_err_t battery_monitor_init(void);

/* ---------------------------------------------------------------------
 * Deep sleep support (power_manager) - mirrors sampling_engine's and
 * gnss_position's own sleep-state APIs. Nothing below does anything
 * unless power_manager calls it. Unlike those two, there's no
 * accumulator to persist here (a battery reading isn't aggregated) -
 * just the next-poll-due deadline.
 * ------------------------------------------------------------------- */

typedef struct {
    int64_t next_poll_due_unix; /* wall-clock deadline for the next voltage poll */
    bool next_poll_due_is_synced;
} battery_sleep_entry_t;

/* Snapshots the next-poll-due deadline (converted to wall-clock time)
 * so power_manager can persist it across a deep sleep. */
void battery_monitor_get_sleep_state(battery_sleep_entry_t *out);

/* Hands back state captured by a previous boot's
 * battery_monitor_get_sleep_state(). Must be called before
 * battery_monitor_init() - consumed once by the task's first iteration. */
void battery_monitor_restore_sleep_state(const battery_sleep_entry_t *in);

typedef struct {
    /* Always false - battery polling has no sample/log split to
     * silently degrade (see battery_settings_t's allow_skip_during_sleep
     * comment), so it never forbids deep sleep outright. Kept for API
     * symmetry with sampling_engine/gnss_position. */
    bool blocked;
    char blocked_reason[40];

    /* False whenever battery monitoring is disabled, or
     * allow_skip_during_sleep is set (the device may freely sleep
     * through polls, so there's nothing to bound sleep by) - in either
     * case next_due_unix is meaningless. */
    bool has_schedulable;
    bool next_due_is_synced; /* false: system clock wasn't synced, next_due_unix is meaningless */
    int64_t next_due_unix;   /* wall-clock deadline power_manager must wake by */
} battery_monitor_sleep_status_t;

/* Reports the wall-clock deadline power_manager must wake by to keep
 * battery polling on schedule, if any. */
battery_monitor_sleep_status_t battery_monitor_get_sleep_status(void);

#ifdef __cplusplus
}
#endif
