#pragma once

#include "config_schema.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Synthetic sensor source used to exercise the sampling/aggregation
 * pipeline before real SDI-12/I2C drivers exist (and afterwards, as a
 * demo/offline-testing mode). Produces a deterministic-ish sine wave
 * (distinct phase per variable id) plus small noise - useful for
 * hand-checking the Welford aggregator's mean/min/max/stddev output. */
esp_err_t stub_sensor_read(const variable_config_t *var, double *out_value);

#ifdef __cplusplus
}
#endif
