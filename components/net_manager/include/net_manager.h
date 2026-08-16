#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the SoftAP (open network, SSID "WalterSensor-XXXX" derived
 * from the MAC) and starts the always-on-policy state machine: AP stays
 * on for a 5 minute boot grace period, then stays on for as long as at
 * least one client is associated, otherwise turns off. This is
 * independent of the data-plane transport (WiFi STA / cellular) - full
 * STA connection handling is added on top of this in a later build
 * stage; for now the radio only ever runs in AP, STA, APSTA, or NULL
 * mode.
 *
 * suppress_ap_boot_grace: pass power_manager_is_sleep_wake() (false if
 * power_manager isn't in use, or always pass false if deep sleep is
 * never enabled) - true skips the normal forced-AP boot grace window,
 * since a device briefly waking from a power_manager-initiated deep
 * sleep to take a reading shouldn't force its portal on every time. See
 * ap_policy_init(). */
esp_err_t net_manager_init(bool suppress_ap_boot_grace);

/* Extension point for a future physical "force setup mode" button -
 * immediately (re)enters the boot-grace-equivalent forced-on state. */
void net_manager_force_ap_on(void);

bool net_manager_ap_is_active(void);
uint8_t net_manager_ap_client_count(void);

/* True once the STA interface has an IP (only meaningful when
 * net_settings.transport == TRANSPORT_WIFI; always false otherwise). */
bool net_manager_sta_is_connected(void);

#ifdef __cplusplus
}
#endif
