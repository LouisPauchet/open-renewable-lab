#pragma once

/*
 * *** LOW CONFIDENCE / NEEDS VERIFICATION ***
 *
 * This component wraps DPTechnics' `dptechnics/walter-modem` C++
 * library (declared as a managed dependency in main/idf_component.yml,
 * fetched from the ESP Component Registry at build time - not present
 * in this checkout, so its API could not be verified against source).
 * Method names/signatures used in cellular_transport.cpp are based on
 * a research summary (WalterModem class; PDP context activation;
 * WalterModemPSMMode/EDRXMode; on-modem MQTT with
 * WalterModemTlsVersion/TlsValidation profiles), not verified headers.
 *
 * Before relying on this: open the fetched
 * managed_components/dptechnics__walter-modem/src/WalterModem.h and
 * fix every call flagged "VERIFY" in cellular_transport.cpp and
 * mqtt_client's backend_walter_mqtt.cpp to match the real signatures.
 */

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
