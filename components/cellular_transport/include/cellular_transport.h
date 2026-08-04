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

#ifdef __cplusplus
}
#endif
