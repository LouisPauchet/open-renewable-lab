#pragma once

/*
 * Bit-banged SDI-12 driver, per SDI-12 v1.4: 1200 baud, 7 data bits,
 * EVEN parity, 1 stop bit (confirmed against the spec and multiple
 * independent implementations - EnviroDIY/Arduino-SDI-12,
 * ESPSoftwareSerial-based drivers using SWSERIAL_7E1), inverted
 * mark/space polarity vs. standard UART. Matches Walter Feels' actual
 * topology (confirmed from schematic): separate TXD/RXD GPIOs through
 * two SN74LV1T126 tri-state buffers (BOARD_PIN_SDI12_TXD/RXD), each
 * gated by its own enable pin (BOARD_PIN_SDI12_TX_EN/RX_EN) - not a
 * single shared bidirectional line. SER_TX/SER_RX are also muxed with
 * RS485/RS232 transceivers on this board; sdi12_bus_init() holds those
 * disabled.
 *
 * Real hardware bring-up so far, on two separate Walter Feels boards:
 * TX_EN/TXD genuinely reaches the bus (confirmed by multimeter, 0-5V
 * swing, via sdi12_bus_debug_tx_toggle_test() below) and RX_EN sits at
 * the expected enabled level - but the receive path is dead on both
 * boards: a loopback edge-count of our own TX transitions on RXD reads
 * 0, and a direct meter reading on the RXD GPIO pin itself shows no
 * voltage change at all despite the bus swinging. That isolates the
 * fault to the RX_EN-to-U6-OE or U6-output-to-RXD physical connection
 * (a schematic/PCB-trace question), not firmware - parity, TX_EN
 * polarity, and TXD/RXD pin order are all independently confirmed
 * correct at this point.
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

/* TEMPORARY hardware bring-up diagnostic - not part of normal
 * operation, not called anywhere by default. A real SDI-12 transaction
 * (833us/bit) is far too fast for a multimeter to see, so this instead
 * asserts TX_EN and slowly (`half_period_ms`, e.g. 1000) toggles TXD
 * `cycles` times so the actual DATA line voltage change can be watched
 * directly with a meter - confirms whether the TX signal path (TX_EN
 * polarity + TXD wiring, through U5) actually reaches the bus at all,
 * independent of SDI-12 protocol timing/parity. Remove once the
 * enable-pin polarity / TXD-RXD pin assignment is confirmed. */
void sdi12_bus_debug_tx_toggle_test(int cycles, uint32_t half_period_ms);

#ifdef __cplusplus
}
#endif
