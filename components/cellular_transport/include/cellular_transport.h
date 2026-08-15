#pragma once

/*
 * This component wraps DPTechnics' `dptechnics/walter-modem` C++
 * library (declared as a managed dependency in main/idf_component.yml).
 * cellular_transport.cpp's WalterModem calls (begin/opstate/PDP/SIM
 * unlock/network selection/GNSS) and backend_walter_mqtt.cpp's on-modem
 * MQTT calls have been verified against the real
 * managed_components/dptechnics__walter-modem/src/WalterModem.h and its
 * examples/positioning/main/positioning.cpp, via an actual esp32s3
 * build - not yet against real network/GNSS behavior on hardware.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the modem, activates a PDP context using
 * net_settings.cellular_apn (+ SIM PIN if set), and starts a
 * background task that tracks registration status. Only call this
 * when net_settings.transport == TRANSPORT_CELLULAR. */
esp_err_t cellular_transport_init(void);

bool cellular_transport_is_registered(void);
bool cellular_transport_is_pdp_active(void);

typedef struct {
    double latitude;
    double longitude;
    float altitude_m;
    float horizontal_accuracy_m; /* WalterModem's estimatedConfidence - estimated horizontal confidence of the fix, in meters */
    int64_t timestamp_unix;
    bool valid;
} gnss_fix_t;

/* Blocks (up to timeout_ms) requesting a fresh GNSS fix from the
 * modem's onboard GNSS receiver. On success, also updates the cached
 * "last fix" returned by cellular_transport_get_last_fix(). Walter's
 * GNSS is part of the cellular modem chip - only meaningful when the
 * modem has been initialized (transport == TRANSPORT_CELLULAR). */
esp_err_t cellular_transport_acquire_gnss_fix(gnss_fix_t *out_fix, uint32_t timeout_ms);

/* Most recently acquired fix (out_fix->valid = false if none yet). */
void cellular_transport_get_last_fix(gnss_fix_t *out_fix);

/* True for the duration of cellular_transport_acquire_gnss_fix() - the
 * modem's RF front-end is shared between LTE and GNSS, so that call
 * drops LTE to NO_RF, waits for GNSS, then reconnects (up to
 * GNSS_FIX_TIMEOUT_MS = 60s total). All modem access goes through one
 * shared command queue (WalterModem's own _cmdProcessingTask), so
 * anything else queuing a command during that window - most notably
 * mqtt_client_bridge's connect/publish - would either be delayed behind
 * it or genuinely fail (no RF = no network) rather than corrupt
 * anything, but either way it's wasted work with its own confusing
 * failure mode. mqtt_client_bridge checks this before attempting a
 * connect and simply skips that cycle instead - see its call site for
 * the real symptom this was fixed for (concurrent "WalterModem: Command
 * time-out" + spurious "MQTT connect rejected" on real hardware).
 * Always false on non-cellular targets/transports. */
bool cellular_transport_gnss_busy(void);

#ifdef __cplusplus
}
#endif
