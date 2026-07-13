#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Thin wrapper over ESP-IDF's driver/i2c_master.h: one shared bus
 * (pins/port from board_pins.h) with a small cache of per-address
 * device handles (the new i2c_master API requires an explicit "device"
 * handle per address before transacting). Internally mutex-guarded so
 * sampling_engine's periodic I2C scheduler task and web_portal's
 * on-demand scan/preview requests can safely share the bus from
 * different tasks. */
esp_err_t i2c_bus_init(void);

esp_err_t i2c_bus_write(uint8_t addr7, const uint8_t *wbuf, size_t wlen, uint32_t timeout_ms);
esp_err_t i2c_bus_write_read(uint8_t addr7, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf, size_t rlen,
                              uint32_t timeout_ms);

/* Probes every 7-bit address in the standard scan range (0x03-0x77)
 * and reports which ones ACK. */
esp_err_t i2c_bus_scan(uint8_t *found, size_t max_found, size_t *out_count);

#ifdef __cplusplus
}
#endif
