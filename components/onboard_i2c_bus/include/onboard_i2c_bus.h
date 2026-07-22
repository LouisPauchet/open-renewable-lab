#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Thin wrapper over ESP-IDF's driver/i2c_master.h for Walter Feels'
 * onboard sensor I2C bus (HDC1080 temp/humidity, LPS22HB pressure/temp
 * - see BOARD_PIN_ONBOARD_I2C_* in board_pins.h) - a separate physical
 * bus from i2c_bus.h's external connector, so a device on one bus
 * never collides with a device at the same address on the other.
 * Mirrors i2c_bus.h's shape; see there for the general design notes. */
esp_err_t onboard_i2c_bus_init(void);

esp_err_t onboard_i2c_bus_write(uint8_t addr7, const uint8_t *wbuf, size_t wlen, uint32_t timeout_ms);
esp_err_t onboard_i2c_bus_write_read(uint8_t addr7, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf, size_t rlen,
                                      uint32_t timeout_ms);

/* Plain read (no preceding write phase) - needed for HDC1080's
 * trigger-then-read-later protocol, where the write (pointer only, no
 * data) and the read happen as two separate I2C transactions with a
 * conversion delay in between. */
esp_err_t onboard_i2c_bus_read(uint8_t addr7, uint8_t *rbuf, size_t rlen, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
