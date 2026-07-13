#pragma once

/* Private to sdi12_bus - shared between sdi12_bus.c and
 * sdi12_protocol.c, not part of the component's public include/. */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SDI12_MAX_RESPONSE_LEN 84 /* spec max data-response length incl. CRLF */
#define SDI12_MAX_MEASURE_WAIT_S 180 /* defensive cap on aM!'s advertised wait time (spec allows up to 999) */

esp_err_t sdi12_measure(char addr, uint8_t *num_values, uint32_t *wait_s);
esp_err_t sdi12_read_data(char addr, uint8_t data_index, char *resp_buf, size_t resp_buf_len);

#ifdef __cplusplus
}
#endif
