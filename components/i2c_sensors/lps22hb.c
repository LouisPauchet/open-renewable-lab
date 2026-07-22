/* ST LPS22HB I2C barometric pressure + temperature sensor (Walter
 * Feels onboard sensor bus - see BOARD_PIN_ONBOARD_I2C_* in
 * board_pins.h). I2C address 0x5C (SA0 strapped low) or 0x5D (SA0
 * high) - confirm which with a bus scan if these reads fail.
 *
 * Register map (ST's own driver header + community drivers, cross-
 * checked): CTRL_REG2 (0x11) bit0 = ONE_SHOT, triggers a single
 * pressure+temperature conversion in the default power-down mode;
 * self-clears when done. STATUS (0x27) bit0/bit1 = P_DA/T_DA (data
 * ready). PRESS_OUT_XL/L/H (0x28-0x2A) = 24-bit signed pressure,
 * TEMP_OUT_L/H (0x2B-0x2C) = 16-bit signed temperature; auto-increment
 * across the burst read is enabled by CTRL_REG2's default reset value
 * (IF_ADD_INC bit, on by default), so a single write-then-read of the
 * starting address returns all 5 bytes.
 *
 * Conversion formulas: pressure(hPa) = raw24 / 4096, temperature(C) =
 * raw16 / 100. One-shot completion is polled via STATUS rather than a
 * fixed delay - the datasheet's exact one-shot timing figure wasn't
 * available when this was written; polling is the datasheet-
 * recommended approach regardless. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onboard_i2c_bus.h"

#define LPS22HB_REG_CTRL_REG2 0x11
#define LPS22HB_REG_STATUS 0x27
#define LPS22HB_REG_PRESS_OUT_XL 0x28

#define LPS22HB_ONE_SHOT_BIT 0x01
#define LPS22HB_STATUS_DATA_READY (0x01 | 0x02) /* P_DA | T_DA */

#define LPS22HB_POLL_INTERVAL_MS 5
#define LPS22HB_POLL_TIMEOUT_MS 100

static esp_err_t lps22hb_trigger_and_read(uint8_t i2c_addr, int32_t *out_press_raw, int16_t *out_temp_raw)
{
    uint8_t trigger[2] = { LPS22HB_REG_CTRL_REG2, LPS22HB_ONE_SHOT_BIT };
    esp_err_t err = onboard_i2c_bus_write(i2c_addr, trigger, sizeof(trigger), 100);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t status_reg = LPS22HB_REG_STATUS;
    uint8_t status = 0;
    int waited_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LPS22HB_POLL_INTERVAL_MS));
        waited_ms += LPS22HB_POLL_INTERVAL_MS;

        err = onboard_i2c_bus_write_read(i2c_addr, &status_reg, 1, &status, 1, 100);
        if (err != ESP_OK) {
            return err;
        }
        if ((status & LPS22HB_STATUS_DATA_READY) == LPS22HB_STATUS_DATA_READY) {
            break;
        }
        if (waited_ms >= LPS22HB_POLL_TIMEOUT_MS) {
            return ESP_ERR_TIMEOUT;
        }
    }

    uint8_t out_reg = LPS22HB_REG_PRESS_OUT_XL;
    uint8_t buf[5]; /* PRESS_OUT_XL, _L, _H, TEMP_OUT_L, TEMP_OUT_H */
    err = onboard_i2c_bus_write_read(i2c_addr, &out_reg, 1, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        return err;
    }

    int32_t press_raw = (int32_t)(((uint32_t)buf[2] << 16) | ((uint32_t)buf[1] << 8) | buf[0]);
    if (press_raw & 0x00800000) {
        press_raw |= (int32_t)0xFF000000; /* sign-extend 24-bit to 32-bit */
    }
    int16_t temp_raw = (int16_t)(((uint16_t)buf[4] << 8) | buf[3]);

    *out_press_raw = press_raw;
    *out_temp_raw = temp_raw;
    return ESP_OK;
}

esp_err_t lps22hb_read_pressure(uint8_t i2c_addr, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    int32_t press_raw;
    int16_t temp_raw;
    esp_err_t err = lps22hb_trigger_and_read(i2c_addr, &press_raw, &temp_raw);
    if (err != ESP_OK) {
        return err;
    }
    *out_value = (double)press_raw / 4096.0;
    return ESP_OK;
}

esp_err_t lps22hb_read_temperature(uint8_t i2c_addr, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    int32_t press_raw;
    int16_t temp_raw;
    esp_err_t err = lps22hb_trigger_and_read(i2c_addr, &press_raw, &temp_raw);
    if (err != ESP_OK) {
        return err;
    }
    *out_value = (double)temp_raw / 100.0;
    return ESP_OK;
}
