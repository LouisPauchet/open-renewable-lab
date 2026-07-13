/* Fallback driver for I2C devices without a dedicated driver: reads a
 * raw signed 16-bit big-endian value from an arbitrary register, no
 * scaling applied. `reg` is the variable's channel_index field
 * reinterpreted as a register address - lets a student wire up a
 * simple sensor by register address alone before a proper driver
 * exists. */

#include "i2c_bus.h"
#include "i2c_sensor_registry.h"

esp_err_t generic_read_register16(uint8_t i2c_addr, uint8_t reg, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t read_buf[2];
    esp_err_t err = i2c_bus_write_read(i2c_addr, &reg, 1, read_buf, sizeof(read_buf), 100);
    if (err != ESP_OK) {
        return err;
    }

    int16_t raw = (int16_t)((read_buf[0] << 8) | read_buf[1]);
    *out_value = (double)raw;
    return ESP_OK;
}
