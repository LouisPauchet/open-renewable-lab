#pragma once

/* Private to net_manager - not part of the component's public include/. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AP_STATE_BOOT_GRACE = 0,   /* forced on, first AP_BOOT_GRACE_MS after boot */
    AP_STATE_ACTIVE_BY_CLIENT, /* on because >=1 client is associated */
    AP_STATE_OFF,
} ap_state_t;

typedef void (*ap_policy_state_change_cb_t)(ap_state_t old_state, ap_state_t new_state);

/* suppress_boot_grace: true on a power_manager-initiated sleep-wake boot
 * (see power_manager_is_sleep_wake()) - starts directly in AP_STATE_OFF
 * instead of AP_STATE_BOOT_GRACE, so a device that deep-sleeps and wakes
 * briefly to take a reading doesn't force its AP/portal on every single
 * time. Normal boots (false) behave exactly as before. */
esp_err_t ap_policy_init(bool suppress_boot_grace);
void ap_policy_set_callback(ap_policy_state_change_cb_t cb);
void ap_policy_force_on(void);
ap_state_t ap_policy_get_state(void);
uint8_t ap_policy_client_count(void);

#ifdef __cplusplus
}
#endif
