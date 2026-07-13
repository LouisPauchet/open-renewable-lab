#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the periodic GNSS position task. No-ops (returns ESP_OK
 * without spawning anything) unless net_settings.transport ==
 * TRANSPORT_CELLULAR, since Walter's GNSS lives on the cellular modem
 * chip - only call this after cellular_transport_init() has
 * succeeded. position.enabled/interval_ms (config_store) are checked
 * every cycle and can be changed without a reboot; only the transport
 * check itself is fixed at boot, consistent with the rest of the
 * network stack. */
esp_err_t gnss_position_init(void);

#ifdef __cplusplus
}
#endif
