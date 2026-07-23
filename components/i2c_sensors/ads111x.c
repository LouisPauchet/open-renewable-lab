/* TI ADS1113/1114/1115 16-bit I2C ADC.
 *
 * Single-shot mode, 128SPS. Returns the measured voltage in volts - no
 * additional scaling, since the portal's per-variable "unit" label is
 * just a free-form display string the student sets, not interpreted by
 * firmware.
 *
 * `channel` is the ADS111x's raw 3-bit MUX select (datasheet table),
 * so a variable's I2C "channel index" (0-7) maps 1:1 onto the chip's
 * own input-multiplexer modes:
 *   0 = AIN0-AIN1 (differential)   4 = AIN0 vs GND (single-ended)
 *   1 = AIN0-AIN3 (differential)   5 = AIN1 vs GND (single-ended)
 *   2 = AIN1-AIN3 (differential)   6 = AIN2 vs GND (single-ended)
 *   3 = AIN2-AIN3 (differential)   7 = AIN3 vs GND (single-ended)
 *
 * `gain` is likewise the chip's raw 3-bit PGA select (datasheet table),
 * choosing the full-scale range (FSR) the 16-bit signed reading spans:
 *   0 = +/-6.144V   3 = +/-1.024V   6 = +/-0.256V
 *   1 = +/-4.096V   4 = +/-0.512V   7 = +/-0.256V (same as 6)
 *   2 = +/-2.048V   5 = +/-0.256V
 * Pick the smallest FSR that still comfortably covers the signal's
 * expected peak, for the best resolution - but note the ADS111x's
 * absolute maximum *input pin* voltage is VDD+0.3V regardless of PGA
 * setting, so a larger FSR does not make it safe to feed in a signal
 * that exceeds the chip's own supply voltage.
 *
 * One "sample" is actually an average of ADS111X_SAMPLES_PER_AVERAGE
 * independent single-shot conversions spread over
 * ADS111X_AVERAGING_WINDOW_MS (100ms): a boxcar average of length T has
 * spectral nulls at every multiple of 1/T, so 100ms nulls both 50Hz
 * and 60Hz mains ripple simultaneously (the same line-cycle-rejection
 * trick precision multimeters use), and generally attenuates anything
 * above ~10Hz that would otherwise alias into sample_interval_ms-
 * spaced single-shot readings. This assumes the signal itself varies
 * slowly relative to 100ms (true for the sensors this firmware
 * targets - panel voltage, pyranometer irradiance, etc.) - it is not
 * appropriate for a variable meant to capture fast transients.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "i2c_sensor_registry.h"

#define ADS111X_REG_CONVERSION 0x00
#define ADS111X_REG_CONFIG 0x01

#define ADS111X_CONVERSION_DELAY_MS 10 /* 128SPS ~7.8ms, padded for margin */

#define ADS111X_AVERAGING_WINDOW_MS 100
#define ADS111X_SAMPLES_PER_AVERAGE (ADS111X_AVERAGING_WINDOW_MS / ADS111X_CONVERSION_DELAY_MS)

static const double ADS111X_FSR_VOLTS_BY_GAIN[8] = {
    6.144, 4.096, 2.048, 1.024, 0.512, 0.256, 0.256, 0.256,
};

static esp_err_t ads111x_read_once(uint8_t i2c_addr, uint16_t mux, uint16_t pga, double *out_value)
{
    uint16_t config = 0x8000u        /* OS: start single conversion */
                       | (mux << 12) /* MUX */
                       | (pga << 9)  /* PGA */
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
    *out_value = ((double)raw / 32768.0) * ADS111X_FSR_VOLTS_BY_GAIN[pga];
    return ESP_OK;
}

esp_err_t ads111x_read_channel(uint8_t i2c_addr, uint8_t channel, uint8_t gain, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t mux = (uint16_t)(channel & 0x7); /* direct MUX select - see file header table */
    uint16_t pga = (uint16_t)(gain & 0x7);    /* direct PGA select - see file header table */

    double sum = 0.0;
    for (int i = 0; i < ADS111X_SAMPLES_PER_AVERAGE; i++) {
        double value;
        esp_err_t err = ads111x_read_once(i2c_addr, mux, pga, &value);
        if (err != ESP_OK) {
            return err;
        }
        sum += value;
    }

    *out_value = sum / ADS111X_SAMPLES_PER_AVERAGE;
    return ESP_OK;
}
