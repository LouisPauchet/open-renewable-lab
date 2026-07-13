#pragma once

/*
 * Bit-banged SDI-12 driver, per SDI-12 v1.4: 1200 baud, 7 data bits,
 * even parity, 1 stop bit, inverted mark/space polarity vs. standard
 * UART. Matches Walter Feels' actual topology (confirmed from
 * schematic): separate TXD/RXD GPIOs through two SN74LV1T126 tri-state
 * buffers (BOARD_PIN_SDI12_TXD/RXD), each gated by its own enable pin
 * (BOARD_PIN_SDI12_TX_EN/RX_EN) - not a single shared bidirectional
 * line. SER_TX/SER_RX are also muxed with RS485/RS232 transceivers on
 * this board; sdi12_bus_init() holds those disabled.
 *
 * *** STILL UNVERIFIED AGAINST REAL HARDWARE ***
 * Pin numbers come from the user-supplied schematic and are
 * confirmed, but this was written without an oscilloscope or a real
 * SDI-12 sensor to test against - the bit timing and the SN74LV1T126's
 * assumed active-HIGH enable polarity should be checked with a logic
 * analyzer against one known-good sensor before trusting readings. If
 * the enable polarity is actually inverted, flip the `1`/`0` levels
 * used for BOARD_PIN_SDI12_TX_EN/RX_EN in sdi12_bus.c.
 *
 * The bus is intended to be called from a single task at a time
 * (sampling_engine's SDI-12 scheduler task in normal operation); an
 * internal mutex serializes any other caller (e.g. web_portal's
 * preview/scan actions running on the HTTP server's task).
 */

#include <stddef.h>
#include <stdint.h>

#include "config_schema.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sdi12_bus_init(void);

/* Sends "<addr><cmd_body>!" and reads the response up to <CR><LF> (or
 * response_timeout_ms with no response at all). resp_buf receives the
 * response with the trailing <CR><LF> stripped is NOT guaranteed -
 * callers that care should strip themselves; the NUL-terminated raw
 * response (address char + payload + \r\n, if seen) is stored as-is. */
esp_err_t sdi12_send_command(char addr, const char *cmd_body, char *resp_buf, size_t resp_buf_len,
                              uint32_t response_timeout_ms);

/* Probes every valid SDI-12 address (0-9, A-Z, a-z) with the
 * "Acknowledge Active" command and reports which ones respond. Takes
 * up to a few seconds on an empty bus (62 addresses x timeout each). */
esp_err_t sdi12_scan_addresses(char *found, size_t max_found, size_t *out_count);

esp_err_t sdi12_change_address(char old_addr, char new_addr);

/* High-level "aM! -> wait -> aD0!" sequence, extracting the
 * parameter_index'th returned value. Only reads aD0! (not aD1!..aD9!),
 * so sensors returning more values than fit in one aD0! response
 * aren't fully supported yet - a known v1 limitation. */
esp_err_t sdi12_measure_and_read(char addr, uint8_t parameter_index, double *out_value);

/* Matches sensor_bus_read_fn_t (sampling_engine.h) - register this
 * directly with sampling_engine_register_bus_driver(BUS_TYPE_SDI12, ...). */
esp_err_t sdi12_variable_read(const variable_config_t *var, double *out_value);

#ifdef __cplusplus
}
#endif
