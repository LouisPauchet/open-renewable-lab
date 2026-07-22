#include "i2c_sensor_registry.h"

esp_err_t i2c_sensor_read(uint8_t device_type, uint8_t i2c_addr, uint8_t channel_index, uint8_t gain,
                          double *out_value)
{
    switch (device_type) {
        case I2C_DEVICE_TYPE_ADS111X:
            return ads111x_read_channel(i2c_addr, channel_index, gain, out_value);
        case I2C_DEVICE_TYPE_GENERIC_REG16:
            return generic_read_register16(i2c_addr, channel_index, out_value);
        case I2C_DEVICE_TYPE_HDC1080_TEMP:
            return hdc1080_read_temperature(i2c_addr, out_value);
        case I2C_DEVICE_TYPE_HDC1080_HUMIDITY:
            return hdc1080_read_humidity(i2c_addr, out_value);
        case I2C_DEVICE_TYPE_LPS22HB_PRESSURE:
            return lps22hb_read_pressure(i2c_addr, out_value);
        case I2C_DEVICE_TYPE_LPS22HB_TEMP:
            return lps22hb_read_temperature(i2c_addr, out_value);
        case I2C_DEVICE_TYPE_LTC4015:
            return ltc4015_read_channel(i2c_addr, channel_index, out_value);
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t i2c_variable_read(const variable_config_t *var, double *out_value)
{
    if (!var || var->bus_type != BUS_TYPE_I2C) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_sensor_read(var->addr.i2c.device_type, var->addr.i2c.i2c_addr, var->addr.i2c.channel_index,
                            var->addr.i2c.gain, out_value);
}
