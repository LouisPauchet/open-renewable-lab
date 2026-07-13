#pragma once

#include <stdbool.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
