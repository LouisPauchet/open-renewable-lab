#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the periodic battery-voltage monitor task. Unlike
 * gnss_position_init() (which requires cellular transport
 * specifically), this always spawns the task - the LTC4015 is read
 * over the external I2C bus, which both board variants provide; a
 * board with no LTC4015 present just fails each read gracefully
 * (logged, non-fatal), same as any other I2C sensor with nothing at
 * its address. battery.enabled/interval_ms (config_store) are checked
 * every cycle and can be changed without a reboot. Only call after
 * i2c_bus_init(). */
esp_err_t battery_monitor_init(void);

#ifdef __cplusplus
}
#endif
