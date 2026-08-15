#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the periodic GNSS position task. No-ops (returns ESP_OK
 * without spawning anything) unless net_settings.transport ==
 * TRANSPORT_CELLULAR, since Walter's GNSS lives on the cellular modem
 * chip - only call this after cellular_transport_init() has
 * succeeded. position.enabled/sample_interval_ms/log_interval_ms/
 * aggregate_mask (config_store) are checked every cycle and can be
 * changed without a reboot; only the transport check itself is fixed
 * at boot, consistent with the rest of the network stack.
 *
 * Requires a physical GNSS antenna connected to the modem - without
 * one, gnssPerformAction() calls just fail/time out repeatedly
 * (logged, harmless) rather than ever producing a fix. Firmware has no
 * way to detect antenna presence itself; the portal surfaces this as a
 * plain warning next to the Position reporting settings instead. */
esp_err_t gnss_position_init(void);

#ifdef __cplusplus
}
#endif
