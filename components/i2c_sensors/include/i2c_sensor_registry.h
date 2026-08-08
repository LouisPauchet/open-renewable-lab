#pragma once

#include "config_schema.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* variable_config_t.addr.i2c.device_type values. Each maps to a
 * driver in this component; add new device types here + a matching
 * driver file + a case in i2c_sensor_registry.c's dispatcher. */
#define I2C_DEVICE_TYPE_ADS111X 0     /* TI ADS1113/1114/1115 16-bit ADC; channel_index 0-7 = the chip's raw MUX select (0-3 differential pairs, 4-7 single-ended) - see ads111x.c */
#define I2C_DEVICE_TYPE_GENERIC_REG16 1 /* fallback: raw signed 16-bit big-endian register read, no scaling */

/* Walter Feels onboard sensors - all on the EXTERNAL I2C connector bus
 * (i2c_bus.h), same as LTC4015; see hdc1080.c's header comment for why
 * this isn't the separate BOARD_PIN_ONBOARD_I2C_* bus despite the
 * naming. channel_index is unused (0) for the fixed single-purpose
 * device types below. */
#define I2C_DEVICE_TYPE_HDC1080_TEMP 2     /* TI HDC1080 temperature - see hdc1080.c */
#define I2C_DEVICE_TYPE_HDC1080_HUMIDITY 3 /* TI HDC1080 humidity - see hdc1080.c */
#define I2C_DEVICE_TYPE_LPS22HB_PRESSURE 4 /* ST LPS22HB pressure - see lps22hb.c */
#define I2C_DEVICE_TYPE_LPS22HB_TEMP 5     /* ST LPS22HB temperature - see lps22hb.c */
/* LTC4015 battery monitor; channel_index selects which telemetry
 * register (0=VBAT, 1=VIN, 2=IBAT, 3=DIE_TEMP) - see ltc4015.c. */
#define I2C_DEVICE_TYPE_LTC4015 6

/* device_type-specific drivers, called by the registry dispatcher -
 * not normally called directly. */
esp_err_t ads111x_read_channel(uint8_t i2c_addr, uint8_t channel, uint8_t gain, double *out_value);
esp_err_t generic_read_register16(uint8_t i2c_addr, uint8_t reg, double *out_value);
esp_err_t hdc1080_read_temperature(uint8_t i2c_addr, double *out_value);
esp_err_t hdc1080_read_humidity(uint8_t i2c_addr, double *out_value);
esp_err_t lps22hb_read_pressure(uint8_t i2c_addr, double *out_value);
esp_err_t lps22hb_read_temperature(uint8_t i2c_addr, double *out_value);
esp_err_t ltc4015_read_channel(uint8_t i2c_addr, uint8_t channel, double *out_value);

/* Dispatches to the driver matching device_type. `gain` is only
 * meaningful for I2C_DEVICE_TYPE_ADS111X (see ads111x.c) and ignored
 * otherwise. */
esp_err_t i2c_sensor_read(uint8_t device_type, uint8_t i2c_addr, uint8_t channel_index, uint8_t gain,
                          double *out_value);

/* Matches sensor_bus_read_fn_t (sampling_engine.h) - register this
 * directly with sampling_engine_register_bus_driver(BUS_TYPE_I2C, ...). */
esp_err_t i2c_variable_read(const variable_config_t *var, double *out_value);

#ifdef __cplusplus
}
#endif
