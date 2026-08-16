#include "battery_monitor.h"

#include <time.h>

#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_sensor_registry.h"
#include "mqtt_client_bridge.h"
#include "time_sync.h"

static const char *TAG = "battery_monitor";

/* LTC4015 has no address-select pins - fixed per its datasheet, same
 * constant ltc4015.c's own header comment documents. */
#define LTC4015_I2C_ADDR 0x68
#define IDLE_CHECK_MS 5000

/* Poll cadence used when battery.sync_with_mqtt is set instead of a
 * user-configured interval_ms. mqttc_publish_battery() only ever
 * queues a "pending" value that's actually transmitted later, at
 * mqtt_client_bridge's own publish points (each non-batch publish, or
 * each batch transmit window) - see its doc comment. So "report at
 * each MQTT publish" doesn't require reading the sensor at that exact
 * moment, just keeping the pending value fresh enough that it's never
 * meaningfully stale by the time it actually gets sent: 5s is fresh
 * relative to any reasonable publish cadence (non-batch's ~500ms tick
 * or batch's typically-minutes-to-hours window) without re-reading a
 * slowly-varying quantity needlessly often. */
#define SYNC_POLL_MS 5000

/* When the next poll is due, in esp_timer_get_time()'s epoch. Guarded by a
 * mutex (rather than left as a task-local) so the deep-sleep accessors at
 * the bottom of this file - called from power_manager's task - can read
 * it safely; there's no aggregator to persist here (a battery reading
 * isn't aggregated), just this one deadline. */
static int64_t s_next_poll_due_us;
static SemaphoreHandle_t s_next_due_mutex;

/* Populated by battery_monitor_restore_sleep_state() before init(),
 * consumed by the task's first loop iteration and then discarded. */
static battery_sleep_entry_t s_restore_entry;
static bool s_has_restore_entry;

static void battery_monitor_task(void *pvParams)
{
    (void)pvParams;

    if (s_has_restore_entry) {
        int64_t now_us = esp_timer_get_time();
        xSemaphoreTake(s_next_due_mutex, portMAX_DELAY);
        s_next_poll_due_us = s_restore_entry.next_poll_due_is_synced
            ? now_us + (s_restore_entry.next_poll_due_unix - (int64_t)time(NULL)) * 1000000LL
            : now_us; /* no usable deadline - poll ASAP, same as a fresh boot */
        xSemaphoreGive(s_next_due_mutex);
        s_has_restore_entry = false;
    }

    for (;;) {
        battery_settings_t bat;
        config_store_get_battery_settings(&bat);

        uint32_t poll_ms = bat.sync_with_mqtt ? SYNC_POLL_MS : bat.interval_ms;
        if (!bat.enabled || poll_ms == 0) {
            vTaskDelay(pdMS_TO_TICKS(IDLE_CHECK_MS));
            continue;
        }

        double voltage;
        esp_err_t err = ltc4015_read_channel(LTC4015_I2C_ADDR, 0 /* VBAT */, &voltage);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "battery voltage: %.3f V", voltage);
            mqttc_publish_battery(voltage);
        } else {
            ESP_LOGW(TAG, "battery voltage read failed: %s", esp_err_to_name(err));
        }

        xSemaphoreTake(s_next_due_mutex, portMAX_DELAY);
        s_next_poll_due_us = esp_timer_get_time() + (int64_t)poll_ms * 1000;
        xSemaphoreGive(s_next_due_mutex);

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

esp_err_t battery_monitor_init(void)
{
    if (!s_next_due_mutex) {
        s_next_due_mutex = xSemaphoreCreateMutex();
        if (!s_next_due_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ok = xTaskCreate(battery_monitor_task, "battery_mon", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ---------------------------------------------------------------------
 * deep sleep support (power_manager)
 * ------------------------------------------------------------------- */

void battery_monitor_get_sleep_state(battery_sleep_entry_t *out)
{
    if (!out || !s_next_due_mutex) {
        return;
    }

    int64_t now_unix = time(NULL);
    bool synced = now_unix >= TIME_SYNC_EPOCH_THRESHOLD;
    int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(s_next_due_mutex, portMAX_DELAY);
    int64_t next_due_us = s_next_poll_due_us;
    xSemaphoreGive(s_next_due_mutex);

    out->next_poll_due_is_synced = synced;
    out->next_poll_due_unix = synced ? now_unix + (next_due_us - now_us) / 1000000LL : 0;
}

void battery_monitor_restore_sleep_state(const battery_sleep_entry_t *in)
{
    if (!in) {
        return;
    }
    s_restore_entry = *in;
    s_has_restore_entry = true;
}

battery_monitor_sleep_status_t battery_monitor_get_sleep_status(void)
{
    battery_monitor_sleep_status_t status = { 0 };

    battery_settings_t bat;
    config_store_get_battery_settings(&bat);

    uint32_t poll_ms = bat.sync_with_mqtt ? SYNC_POLL_MS : bat.interval_ms;
    if (!bat.enabled || poll_ms == 0 || bat.allow_skip_during_sleep || !s_next_due_mutex) {
        return status; /* has_schedulable=false: nothing mandatory from battery polling */
    }

    status.has_schedulable = true;

    int64_t now_unix = time(NULL);
    bool synced = now_unix >= TIME_SYNC_EPOCH_THRESHOLD;
    if (!synced) {
        return status; /* next_due_is_synced stays false - no usable wall-clock bound yet */
    }

    xSemaphoreTake(s_next_due_mutex, portMAX_DELAY);
    int64_t next_due_us = s_next_poll_due_us;
    xSemaphoreGive(s_next_due_mutex);

    status.next_due_is_synced = true;
    status.next_due_unix = now_unix + (next_due_us - esp_timer_get_time()) / 1000000LL;
    return status;
}
