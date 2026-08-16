#include "power_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "battery_monitor.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gnss_position.h"
#include "mqtt_client_bridge.h"
#include "net_manager.h"
#include "sampling_engine.h"
#include "sd_logger.h"
#include "time_sync.h"

static const char *TAG = "power_manager";

#define ELIGIBILITY_CHECK_MS 5000
#define STAY_AWAKE_MAX_MINUTES (24 * 60)
#define RTC_MAGIC 0x504D5352u /* "PMSR" - guards against trusting stale/foreign RTC content */

/* RTC memory is small (a few KB total, shared with WiFi/BT/esp_system's
 * own retained state) and MAX_VARIABLES=32 worth of full aggregator_t
 * accumulators doesn't fit in it - confirmed by a real link failure
 * ("region rtc_slow_seg overflowed"). The deployments this feature
 * targets (a remote weather station, a PV logger) realistically use a
 * handful of variables, not anywhere near 32, so cap how many get their
 * in-flight aggregation state persisted across a sleep; any variable
 * beyond this cap just starts its aggregate fresh after a sleep instead
 * of resuming it, same as sampling_engine_get_sleep_state()'s own
 * max_count truncation behavior. Same style of realistic-cap-below-
 * MAX_VARIABLES already used by sd_logger.c's MAX_INTERVAL_GROUPS. */
#define MAX_TRACKED_VARIABLES 8

/* ---------------------------------------------------------------------
 * RTC-persisted state - survives a deep sleep (unlike regular RAM,
 * which is lost entirely) but is only trusted when power_manager_init()
 * confirms this boot was actually woken by our own sleep timer (see
 * s_is_sleep_wake below) - on any other reset, this content may be
 * stale or simply undefined and is discarded instead of restored.
 *
 * RTC_FAST_ATTR (RTC fast memory) rather than RTC_DATA_ATTR (RTC slow
 * memory, shared with ULP/WiFi/BT retained state and already tight) -
 * this project never uses the ULP coprocessor, so there's no reason to
 * compete for RTC slow memory specifically. Same deep-sleep-survival
 * guarantee either way, just a different physical memory bank.
 * ------------------------------------------------------------------- */

RTC_FAST_ATTR static uint32_t s_rtc_magic;
RTC_FAST_ATTR static uint32_t s_rtc_variable_count;
RTC_FAST_ATTR static sampling_sleep_entry_t s_rtc_variables[MAX_TRACKED_VARIABLES];
RTC_FAST_ATTR static bool s_rtc_gnss_valid;
RTC_FAST_ATTR static gnss_sleep_entry_t s_rtc_gnss;
RTC_FAST_ATTR static battery_sleep_entry_t s_rtc_battery;

/* Deliberately NOT gated behind s_is_sleep_wake / RTC_MAGIC: a stay-awake
 * request must survive even a sleep that starts moments after it was
 * set (see power_manager_request_stay_awake()'s doc comment), and a
 * stale value here is self-correcting - power_manager_stay_awake_active()
 * treats anything <= time(NULL) as inactive, so leftover RTC content
 * from a previous run just reads as "not active" until a new request
 * sets it again. */
RTC_FAST_ATTR static int64_t s_stay_awake_until_unix;

static bool s_is_sleep_wake;

static SemaphoreHandle_t s_status_mutex;
static bool s_last_blocked;
static char s_last_blocked_reason[40];

/* ---------------------------------------------------------------------
 * eligibility folding
 * ------------------------------------------------------------------- */

static void fold_status(bool status_blocked, const char *status_reason, bool status_has_schedulable,
                         bool status_next_due_is_synced, int64_t status_next_due_unix, bool *blocked,
                         char *blocked_reason, size_t blocked_reason_size, bool *have_due,
                         int64_t *earliest_due_unix)
{
    if (*blocked) {
        return; /* already blocked by an earlier source */
    }
    if (status_blocked) {
        *blocked = true;
        snprintf(blocked_reason, blocked_reason_size, "%s", status_reason);
        return;
    }
    if (status_has_schedulable && status_next_due_is_synced) {
        if (!*have_due || status_next_due_unix < *earliest_due_unix) {
            *earliest_due_unix = status_next_due_unix;
            *have_due = true;
        }
    }
}

/* ---------------------------------------------------------------------
 * deep sleep entry
 * ------------------------------------------------------------------- */

static void enter_deep_sleep(int64_t gap_ms)
{
    if (gap_ms < 1000) {
        gap_ms = 1000; /* defensive floor - never schedule a sub-second sleep */
    }

    /* Flush everything pending before losing all RAM state - see the
     * design note in power_settings_t. Both are safe/cheap no-ops when
     * disabled or nothing's actually pending. */
    mqttc_flush_now();
    sd_logger_flush_now();

    /* Snapshot every component's in-flight state to RTC memory so it
     * survives the sleep - see each *_get_sleep_state()'s own doc
     * comment. Must happen after the flush above (flushing can itself
     * finalize/reset an aggregation window, e.g. a batch send triggers
     * nothing here, but keeping the order explicit avoids relying on
     * that not mattering). */
    s_rtc_variable_count = (uint32_t)sampling_engine_get_sleep_state(s_rtc_variables, MAX_TRACKED_VARIABLES);

    gnss_sleep_entry_t gnss_entry;
    s_rtc_gnss_valid = gnss_position_get_sleep_state(&gnss_entry);
    if (s_rtc_gnss_valid) {
        s_rtc_gnss = gnss_entry;
    }

    battery_monitor_get_sleep_state(&s_rtc_battery);

    s_rtc_magic = RTC_MAGIC;

    ESP_LOGI(TAG, "entering deep sleep for %lld ms (%u variable(s), gnss=%d)", (long long)gap_ms,
             (unsigned)s_rtc_variable_count, (int)s_rtc_gnss_valid);

    esp_sleep_enable_timer_wakeup((uint64_t)gap_ms * 1000);
    esp_deep_sleep_start();
    /* never returns */
}

/* ---------------------------------------------------------------------
 * periodic task
 * ------------------------------------------------------------------- */

static void power_manager_task(void *pvParams)
{
    (void)pvParams;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ELIGIBILITY_CHECK_MS));

        power_settings_t pm;
        config_store_get_power_settings(&pm);
        if (!pm.enabled) {
            continue;
        }
        if (power_manager_stay_awake_active()) {
            continue;
        }

        int64_t now_unix = time(NULL);
        if (now_unix < TIME_SYNC_EPOCH_THRESHOLD) {
            continue; /* can't safely schedule a wall-clock wake yet */
        }

        bool blocked = false;
        char blocked_reason[40] = "";
        bool have_due = false;
        int64_t earliest_due_unix = 0;

        sampling_engine_sleep_status_t se = sampling_engine_get_sleep_status();
        fold_status(se.blocked, se.blocked_reason, se.has_schedulable, se.next_due_is_synced, se.next_due_unix,
                    &blocked, blocked_reason, sizeof(blocked_reason), &have_due, &earliest_due_unix);

        gnss_position_sleep_status_t gs = gnss_position_get_sleep_status();
        fold_status(gs.blocked, gs.blocked_reason, gs.has_schedulable, gs.next_due_is_synced, gs.next_due_unix,
                    &blocked, blocked_reason, sizeof(blocked_reason), &have_due, &earliest_due_unix);

        battery_monitor_sleep_status_t bs = battery_monitor_get_sleep_status();
        fold_status(bs.blocked, bs.blocked_reason, bs.has_schedulable, bs.next_due_is_synced, bs.next_due_unix,
                    &blocked, blocked_reason, sizeof(blocked_reason), &have_due, &earliest_due_unix);

        mqttc_sleep_status_t ms = mqttc_get_sleep_status();
        fold_status(ms.blocked, ms.blocked_reason, ms.has_schedulable, ms.next_due_is_synced, ms.next_due_unix,
                    &blocked, blocked_reason, sizeof(blocked_reason), &have_due, &earliest_due_unix);

        if (s_status_mutex) {
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_last_blocked = blocked;
            snprintf(s_last_blocked_reason, sizeof(s_last_blocked_reason), "%s", blocked_reason);
            xSemaphoreGive(s_status_mutex);
        }

        if (blocked || !have_due) {
            continue; /* something forbids sleep outright, or nothing schedulable to bound it by */
        }

        int64_t gap_ms = (earliest_due_unix - now_unix) * 1000;
        if (gap_ms < (int64_t)pm.min_sleep_duration_ms) {
            continue; /* not worth the reboot+reconnect overhead for this short a gap */
        }

        enter_deep_sleep(gap_ms);
    }
}

/* ---------------------------------------------------------------------
 * public API
 * ------------------------------------------------------------------- */

esp_err_t power_manager_init(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* esp_sleep_get_wakeup_cause() is deprecated in favor of this
     * bitmask form; ESP_SLEEP_WAKEUP_TIMER's bit position is the enum
     * value itself, per esp_sleep_get_wakeup_causes()'s own doc comment. */
    uint32_t causes = esp_sleep_get_wakeup_causes();
    s_is_sleep_wake = ((causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) != 0) && (s_rtc_magic == RTC_MAGIC);

    if (s_is_sleep_wake) {
        ESP_LOGI(TAG, "resuming from deep sleep (%u variable(s), gnss=%d)", (unsigned)s_rtc_variable_count,
                 (int)s_rtc_gnss_valid);
        if (s_rtc_variable_count > 0) {
            sampling_engine_restore_sleep_state(s_rtc_variables, s_rtc_variable_count);
        }
        if (s_rtc_gnss_valid) {
            gnss_position_restore_sleep_state(&s_rtc_gnss);
        }
        battery_monitor_restore_sleep_state(&s_rtc_battery);
    } else {
        /* Not a sleep-wake (power-on reset, software reset, a fresh
         * flash, ...) - any RTC memory content is stale or was never
         * ours (ESP-IDF only guarantees RTC memory survives deep sleep/
         * most resets, not a genuine power-on) - start from a clean
         * slate rather than risk restoring garbage into a component. */
        s_rtc_magic = 0;
        s_rtc_variable_count = 0;
        s_rtc_gnss_valid = false;
        memset(&s_rtc_gnss, 0, sizeof(s_rtc_gnss));
        memset(&s_rtc_battery, 0, sizeof(s_rtc_battery));
    }

    BaseType_t ok = xTaskCreate(power_manager_task, "power_mgr", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool power_manager_is_sleep_wake(void)
{
    return s_is_sleep_wake;
}

void power_manager_request_stay_awake(uint32_t minutes)
{
    if (minutes == 0) {
        s_stay_awake_until_unix = 0;
        ESP_LOGI(TAG, "stay-awake window cancelled");
        return;
    }
    if (minutes > STAY_AWAKE_MAX_MINUTES) {
        minutes = STAY_AWAKE_MAX_MINUTES;
    }

    /* Best-effort even before time sync (time(NULL) may read near-zero) -
     * in practice this is moot, since power_manager_task never attempts
     * a sleep before the wall clock is synced in the first place (see
     * its own TIME_SYNC_EPOCH_THRESHOLD check), so there is nothing yet
     * to override the very first time this could matter. */
    s_stay_awake_until_unix = time(NULL) + (int64_t)minutes * 60;
    net_manager_force_ap_on();
    ESP_LOGI(TAG, "stay-awake window: %u minute(s)", (unsigned)minutes);
}

bool power_manager_stay_awake_active(void)
{
    return s_stay_awake_until_unix != 0 && time(NULL) < s_stay_awake_until_unix;
}

power_manager_status_t power_manager_get_status(void)
{
    power_manager_status_t status = { 0 };

    power_settings_t pm;
    config_store_get_power_settings(&pm);
    status.sleep_enabled = pm.enabled;

    if (s_status_mutex) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        status.blocked = s_last_blocked;
        snprintf(status.blocked_reason, sizeof(status.blocked_reason), "%s", s_last_blocked_reason);
        xSemaphoreGive(s_status_mutex);
    }

    status.stay_awake_active = power_manager_stay_awake_active();
    status.stay_awake_until_unix = s_stay_awake_until_unix;
    return status;
}
