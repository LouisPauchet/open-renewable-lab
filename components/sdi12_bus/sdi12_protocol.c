#include "sdi12_bus.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdi12_internal.h"

/* SDI-12 values are concatenated signed decimals with no separator
 * (e.g. "+12.3-4.56+0"); strtod's end-pointer naturally lands on the
 * next value's sign character, so repeated strtod calls split them. */
static size_t parse_values(const char *s, double *out, size_t max)
{
    size_t n = 0;
    const char *p = s;
    while (*p && n < max) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) {
            p++; /* unexpected character - skip and keep trying */
            continue;
        }
        out[n++] = v;
        p = end;
    }
    return n;
}

esp_err_t sdi12_measure_and_read(char addr, uint8_t parameter_index, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t num_values = 0;
    uint32_t wait_s = 0;
    esp_err_t err = sdi12_measure(addr, &num_values, &wait_s);
    if (err != ESP_OK) {
        return err;
    }

    if (wait_s > 0) {
        /* Simplification: sensors may also send an unsolicited service
         * request as soon as data is ready, before wait_s elapses - not
         * listened for here, so this always waits the full advertised
         * time. Fine for infrequent SDI-12 sampling; a future
         * optimization could shorten this. */
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }

    char resp[SDI12_MAX_RESPONSE_LEN];
    err = sdi12_read_data(addr, 0, resp, sizeof(resp)); /* only aD0! - known v1 limitation, see header */
    if (err != ESP_OK) {
        return err;
    }

    if (strlen(resp) < 2 || resp[0] != addr) {
        return ESP_FAIL;
    }

    double values[10];
    size_t count = parse_values(resp + 1, values, 10);
    if (parameter_index >= count) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_value = values[parameter_index];
    return ESP_OK;
}

esp_err_t sdi12_variable_read(const variable_config_t *var, double *out_value)
{
    if (!var || var->bus_type != BUS_TYPE_SDI12) {
        return ESP_ERR_INVALID_ARG;
    }
    return sdi12_measure_and_read(var->addr.sdi12.address, var->addr.sdi12.parameter_index, out_value);
}
