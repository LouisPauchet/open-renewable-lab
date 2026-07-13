/* TI ADS1113/1114/1115 16-bit I2C ADC.
 *
 * Fixed configuration for simplicity: PGA +/-4.096V full-scale (a good
 * match for 3.3V-referenced sensor outputs), single-shot mode, 128SPS.
 * Returns the measured voltage in volts - no additional scaling, since
 * the portal's per-variable "unit" label is just a free-form display
 * string the student sets, not interpreted by firmware.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "i2c_sensor_registry.h"

#define ADS111X_REG_CONVERSION 0x00
#define ADS111X_REG_CONFIG 0x01

#define ADS111X_FSR_VOLTS 4.096
#define ADS111X_CONVERSION_DELAY_MS 10 /* 128SPS ~7.8ms, padded for margin */

esp_err_t ads111x_read_channel(uint8_t i2c_addr, uint8_t channel, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t mux = (uint16_t)(0x4 + (channel & 0x3)); /* single-ended AIN0-3 */
    uint16_t config = 0x8000u        /* OS: start single conversion */
                       | (mux << 12) /* MUX */
                       | (0x1 << 9)  /* PGA: 001 = +/-4.096V */
                       | (0x1 << 8)  /* MODE: single-shot */
                       | (0x4 << 5)  /* DR: 100 = 128SPS */
                       | 0x3;        /* COMP_QUE: 11 = disable comparator */

    uint8_t write_buf[3] = { ADS111X_REG_CONFIG, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF) };
    esp_err_t err = i2c_bus_write(i2c_addr, write_buf, sizeof(write_buf), 100);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(ADS111X_CONVERSION_DELAY_MS));

    uint8_t reg = ADS111X_REG_CONVERSION;
    uint8_t read_buf[2];
    err = i2c_bus_write_read(i2c_addr, &reg, 1, read_buf, sizeof(read_buf), 100);
    if (err != ESP_OK) {
        return err;
    }

    int16_t raw = (int16_t)((read_buf[0] << 8) | read_buf[1]);
    *out_value = ((double)raw / 32768.0) * ADS111X_FSR_VOLTS;
    return ESP_OK;
}
