#include "stub_sensor.h"

#include <math.h>

#include "esp_random.h"
#include "esp_timer.h"

esp_err_t stub_sensor_read(const variable_config_t *var, double *out_value)
{
    if (!var || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    double t_s = (double)esp_timer_get_time() / 1.0e6;
    double phase = (double)(var->id % 16) * (M_PI / 8.0);
    double noise = ((double)(esp_random() % 1000) / 1000.0 - 0.5) * 0.4; /* +/- 0.2 */

    *out_value = 20.0 + 5.0 * sin(2.0 * M_PI * t_s / 30.0 + phase) + noise;
    return ESP_OK;
}
