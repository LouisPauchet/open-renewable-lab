/* TI HDC1080 I2C temperature + humidity sensor. Fixed I2C address 0x40
 * (no address pin).
 *
 * Lives on the EXTERNAL I2C connector bus (i2c_bus.h, BOARD_PIN_I2C_*
 * in board_pins.h), the same bus as LTC4015 - not the separate
 * onboard_i2c_bus (BOARD_PIN_ONBOARD_I2C_*, "CO2_SDA"/"CO2_SCL" per the
 * schematic labels), despite that pin's own comment block suggesting
 * otherwise. Corrected from a real-hardware I2C scan: a factory-fresh
 * board with nothing plugged into the external connector still found
 * 0x40 (this chip), 0x5C (LPS22HB), 0x68 (LTC4015, already known to be
 * on this bus) and 0x6A (LSM6DSM IMU, per board_pins.h documented to
 * share HDC1080/LPS22HB's segment) all by scanning the external bus -
 * while onboard_i2c_bus cleanly NACKed (not timed out) on every read,
 * consistent with that pin pair genuinely having nothing attached (it's
 * most likely only wired to the optional, unpopulated CO2/IMU header).
 * Re-verify with a meter if a future board revision moves this back.
 *
 * Register map (TI datasheet SNAS672A): 0x00 = temperature, 0x01 =
 * humidity, both 16-bit. Each channel is triggered independently
 * (config register's default MODE=0): write the register pointer with
 * no data byte to start a conversion, wait the conversion time, then
 * read 2 bytes. Reset defaults already select 14-bit resolution for
 * both channels, so the config register is never touched here.
 *
 * Conversion formulas (datasheet Tables 2/3):
 *   temperature(C) = (raw / 65536) * 165 - 40
 *   humidity(%RH)  = (raw / 65536) * 100
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#define HDC1080_REG_TEMPERATURE 0x00
#define HDC1080_REG_HUMIDITY 0x01

/* Datasheet: ~6.35ms (temp) / ~6.5ms (humidity) typical at the default
 * 14-bit resolution; padded for margin, same convention as
 * ads111x.c's ADS111X_CONVERSION_DELAY_MS. */
#define HDC1080_CONVERSION_DELAY_MS 20

static esp_err_t hdc1080_trigger_and_read(uint8_t i2c_addr, uint8_t reg, uint16_t *out_raw)
{
    uint8_t ptr = reg;
    esp_err_t err = i2c_bus_write(i2c_addr, &ptr, 1, 100);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(HDC1080_CONVERSION_DELAY_MS));

    uint8_t buf[2];
    err = i2c_bus_read(i2c_addr, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        return err;
    }

    *out_raw = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

esp_err_t hdc1080_read_temperature(uint8_t i2c_addr, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t raw;
    esp_err_t err = hdc1080_trigger_and_read(i2c_addr, HDC1080_REG_TEMPERATURE, &raw);
    if (err != ESP_OK) {
        return err;
    }
    *out_value = ((double)raw / 65536.0) * 165.0 - 40.0;
    return ESP_OK;
}

esp_err_t hdc1080_read_humidity(uint8_t i2c_addr, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t raw;
    esp_err_t err = hdc1080_trigger_and_read(i2c_addr, HDC1080_REG_HUMIDITY, &raw);
    if (err != ESP_OK) {
        return err;
    }
    *out_value = ((double)raw / 65536.0) * 100.0;
    return ESP_OK;
}
