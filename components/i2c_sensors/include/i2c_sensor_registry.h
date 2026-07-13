#pragma once

#include "config_schema.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* variable_config_t.addr.i2c.device_type values. Each maps to a
 * driver in this component; add new device types here + a matching
 * driver file + a case in i2c_sensor_registry.c's dispatcher. */
#define I2C_DEVICE_TYPE_ADS111X 0     /* TI ADS1113/1114/1115 16-bit ADC, single-ended channels 0-3 */
#define I2C_DEVICE_TYPE_GENERIC_REG16 1 /* fallback: raw signed 16-bit big-endian register read, no scaling */

/* device_type-specific drivers, called by the registry dispatcher -
 * not normally called directly. */
esp_err_t ads111x_read_channel(uint8_t i2c_addr, uint8_t channel, double *out_value);
esp_err_t generic_read_register16(uint8_t i2c_addr, uint8_t reg, double *out_value);

/* Dispatches to the driver matching device_type. */
esp_err_t i2c_sensor_read(uint8_t device_type, uint8_t i2c_addr, uint8_t channel_index, double *out_value);

/* Matches sensor_bus_read_fn_t (sampling_engine.h) - register this
 * directly with sampling_engine_register_bus_driver(BUS_TYPE_I2C, ...). */
esp_err_t i2c_variable_read(const variable_config_t *var, double *out_value);

#ifdef __cplusplus
}
#endif
