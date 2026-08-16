#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Must be called very early in app_main() - right after config_store_init()
 * and before any of sampling_engine_init()/battery_monitor_init()/
 * gnss_position_init(). Determines whether this boot was woken from a
 * power_manager-initiated deep sleep (vs. a genuine power-on/software
 * reset) and, if so, hands the RTC-persisted per-variable/position/
 * battery state back to each component via their own
 * *_restore_sleep_state() functions, so an in-flight aggregation window
 * isn't silently lost across the sleep. Always starts the periodic
 * eligibility-checking task, which stays idle (just re-checking
 * config_store each cycle) until power.enabled is turned on - safe to
 * call unconditionally regardless of the current setting; every default
 * leaves the device's behavior completely unaffected. */
esp_err_t power_manager_init(void);

/* True if this boot was woken from a power_manager-initiated deep sleep.
 * Pass to net_manager_init() so it can skip the normal AP boot-grace
 * window on a brief sleep-wake - see net_manager_init()'s own comment. */
bool power_manager_is_sleep_wake(void);

/* Portal action: holds the device in normal (non-sleeping) operation for
 * the next `minutes` (clamped to [1, 1440]; 0 cancels an active window
 * immediately), and force-opens the AP so the portal is actually
 * reachable - otherwise a deployed unit with deep sleep enabled has no
 * way to be reached short of a full power-cycle, since this hardware
 * has no physical force-AP button. The deadline is kept in RTC memory
 * (survives a deep sleep) so a sleep already in flight when this is
 * called self-corrects on its very next - normally brief - wake instead
 * of completing the full duration it had already committed to; see
 * power_manager_task() in power_manager.c. */
void power_manager_request_stay_awake(uint32_t minutes);

/* True while a stay-awake window (see above) is currently active. */
bool power_manager_stay_awake_active(void);

typedef struct {
    bool sleep_enabled; /* mirrors power_settings_t.enabled */

    /* True: something currently forbids deep sleep outright (checked on
     * the same ELIGIBILITY_CHECK_MS cadence as the sleep decision itself
     * - see power_manager_task()). blocked_reason is only meaningful
     * when this is true. */
    bool blocked;
    char blocked_reason[40];

    bool stay_awake_active;
    int64_t stay_awake_until_unix; /* only meaningful when stay_awake_active */
} power_manager_status_t;

/* For the web portal's /api/status and Power management section. */
power_manager_status_t power_manager_get_status(void);

#ifdef __cplusplus
}
#endif
