#pragma once

/*
 * Bit-banged SDI-12 driver (single half-duplex GPIO line, per SDI-12
 * v1.4: 1200 baud, 7 data bits, even parity, 1 stop bit, with inverted
 * mark/space polarity vs. standard UART).
 *
 * *** UNVERIFIED AGAINST REAL HARDWARE ***
 * This was written without access to a Walter Feels board, an
 * oscilloscope, or a real SDI-12 sensor - it implements the spec as
 * documented, but timing/polarity assumptions should be checked with a
 * logic analyzer against one known-good sensor before trusting
 * readings. In particular:
 *  - Assumes the carrier board's SDI-12 transceiver presents TRUE
 *    (non-inverted) SDI-12 electrical levels on the GPIO: idle/mark =
 *    LOW, break/space = HIGH. If the transceiver inverts, flip the
 *    level in every gpio_set_level()/gpio_get_level() call in
 *    sdi12_bus.c (search for "SDI-12 polarity").
 *  - Assumes BOARD_PIN_SDI12_DIR_ENABLE (if present) is active-HIGH to
 *    enable the transmitter; verify against the schematic.
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
