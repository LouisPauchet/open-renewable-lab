#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aggregator.h"
#include "config_schema.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t variable_id;
    char name[32];
    uint8_t aggregate_mask; /* which of the fields below are valid, mirrors variable_config_t.aggregate_mask */
    uint32_t sample_count;  /* number of raw samples folded into this result */
    double raw;             /* last raw sample in the interval (AGG_RAW) */
    double mean;            /* AGG_MEAN */
    double min;              /* AGG_MIN */
    double max;              /* AGG_MAX */
    double stddev;           /* AGG_STDDEV */
    int64_t timestamp_unix;  /* seconds since epoch; only meaningful if time_is_synced */
    bool time_is_synced;
} aggregate_result_t;

/* Same raw/mean/min/max/stddev shape as aggregate_result_t above, as a
 * reusable sub-struct rather than a full result in its own right -
 * gnss_position.c bundles four of these (latitude, longitude,
 * elevation, horizontal precision) into one position record instead of
 * four separate variable-shaped results, since GPS position keeps its
 * own dedicated CSV file / MQTT payload shape rather than becoming
 * generic variables. Populated the same way (an aggregator_t per
 * field, see aggregator.h), just outside sampling_engine's own
 * per-variable scheduling. */
typedef struct {
    double raw;
    double mean;
    double min;
    double max;
    double stddev;
} field_aggregate_t;

/* Bus drivers are how sampling_engine stays hardware-agnostic: each bus
 * type (SDI-12, I2C) registers a synchronous read function. Real drivers
 * (sdi12_bus, i2c_bus) and the stub_sensor test driver all implement
 * this same signature. Returning anything other than ESP_OK causes that
 * sample to be skipped (not folded into the aggregator). */
typedef esp_err_t (*sensor_bus_read_fn_t)(const variable_config_t *var, double *out_value);

esp_err_t sampling_engine_init(void);
void sampling_engine_register_bus_driver(bus_type_t bus_type, sensor_bus_read_fn_t read_fn);

/* Registers a queue that receives a copy of every finalized
 * aggregate_result_t (fan-out to sd_logger, mqtt_publish_task, etc). The
 * queue must be drained promptly - sampling_engine sends with a zero
 * timeout and drops (logging a warning) rather than ever blocking the
 * sampling/aggregation pipeline on a slow sink. */
esp_err_t sampling_engine_add_result_sink(QueueHandle_t queue);

/* One-shot synchronous read used by the web portal's "preview"/"test
 * sensor" actions. Bypasses aggregation entirely. */
esp_err_t sampling_engine_read_once(uint16_t variable_id, double *out_value);

/* ---------------------------------------------------------------------
 * Deep sleep support (power_manager) - see variable_config_t's
 * allow_skip_during_sleep comment and power_settings_t for the overall
 * design. Nothing below does anything unless power_manager calls it.
 * ------------------------------------------------------------------- */

typedef struct {
    uint16_t variable_id;
    aggregator_t agg;          /* in-flight accumulator - opaque to the caller, just persist and hand back */
    int64_t next_log_due_unix; /* wall-clock deadline for this variable's next finalize/log */
    bool time_is_synced;       /* if false, next_log_due_unix is meaningless - system clock wasn't synced when captured */
} sampling_sleep_entry_t;

/* Snapshots every active variable's in-flight aggregator + next-log-due
 * deadline (converted to wall-clock time) so power_manager can persist
 * it across a deep sleep. Returns the number of entries written; out
 * must hold at least MAX_VARIABLES entries. */
size_t sampling_engine_get_sleep_state(sampling_sleep_entry_t *out, size_t max_count);

/* Hands back state captured by a previous boot's
 * sampling_engine_get_sleep_state() so in-flight accumulation isn't
 * lost across a deep sleep. Must be called before sampling_engine_init()
 * - entries are consumed by the very first aggregation-table build and
 * discarded after, so later config-driven rebuilds behave normally. */
void sampling_engine_restore_sleep_state(const sampling_sleep_entry_t *in, size_t count);

typedef struct {
    /* True: at least one enabled variable samples faster than it logs
     * (sample_interval_ms < log_interval_ms) without opting into
     * allow_skip_during_sleep - deep sleep must not engage at all right
     * now, regardless of timing, or that variable's data silently
     * degrades. blocked_reason names it. */
    bool blocked;
    char blocked_reason[40];

    bool has_schedulable;     /* false: no enabled variables at all, next_due_unix is meaningless */
    bool next_due_is_synced;  /* false: system clock wasn't synced, next_due_unix is meaningless */
    int64_t next_due_unix;    /* earliest wall-clock deadline across all non-blocking variables */
} sampling_engine_sleep_status_t;

/* Reports whether any enabled variable currently forbids deep sleep
 * outright, and otherwise the earliest wall-clock deadline
 * power_manager must wake by to keep every variable's logging on
 * schedule. */
sampling_engine_sleep_status_t sampling_engine_get_sleep_status(void);

#ifdef __cplusplus
}
#endif
