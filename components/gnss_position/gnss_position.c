#include "gnss_position.h"

#include <stdio.h>
#include <time.h>

#include "aggregator.h"
#include "cellular_transport.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client_bridge.h"
#include "sd_logger.h"
#include "time_sync.h"

static const char *TAG = "gnss_position";

#define GNSS_FIX_TIMEOUT_MS 60000 /* a cold GNSS fix can legitimately take up to ~1 min */
#define IDLE_CHECK_MS 5000

/* Same sample/log-interval + aggregate model as a regular variable (see
 * position_settings_t's own comment): latitude, longitude, elevation, and
 * horizontal precision are each fed into their own Welford accumulator
 * (aggregator.h - the same one sampling_engine itself uses) once per
 * successful fix, and finalized/logged/published once per log_interval_ms
 * rather than after every single fix. Implemented locally here rather than
 * through sampling_engine's generic bus-driver path, since position keeps
 * its own dedicated CSV file / MQTT payload shape (one combined record)
 * instead of becoming four separate generic variables.
 *
 * File-scope (rather than local to gnss_position_task) and mutex-guarded
 * so the deep-sleep accessors at the bottom of this file - called from
 * power_manager's task - can read/seed it safely. */
typedef struct {
    aggregator_t lat_agg, lon_agg, alt_agg, hprec_agg;
    bool window_has_data;
    int64_t next_log_us; /* esp_timer_get_time() epoch */
    int64_t window_timestamp_unix;
    bool window_time_synced;
} gnss_runtime_state_t;

static gnss_runtime_state_t s_state;
static SemaphoreHandle_t s_state_mutex;

/* Populated by gnss_position_restore_sleep_state() before init(),
 * consumed by the task's first loop iteration and then discarded. */
static gnss_sleep_entry_t s_restore_entry;
static bool s_has_restore_entry;

static field_aggregate_t finalize(const aggregator_t *a)
{
    field_aggregate_t f = {
        .raw = a->last_raw,
        .mean = a->mean,
        .min = a->min,
        .max = a->max,
        .stddev = aggregator_stddev(a),
    };
    return f;
}

/* Deliberately NOT registered with esp_task_wdt: a GNSS fix can
 * legitimately block for up to GNSS_FIX_TIMEOUT_MS, well past the
 * watchdog timeout - same reasoning as sampling_engine's SDI-12/I2C
 * bus scheduler tasks. */
static void gnss_position_task(void *pvParams)
{
    (void)pvParams;

    aggregator_reset(&s_state.lat_agg);
    aggregator_reset(&s_state.lon_agg);
    aggregator_reset(&s_state.alt_agg);
    aggregator_reset(&s_state.hprec_agg);
    s_state.window_has_data = false;
    s_state.next_log_us = 0;
    s_state.window_timestamp_unix = 0;
    s_state.window_time_synced = false;

    if (s_has_restore_entry) {
        s_state.lat_agg = s_restore_entry.lat_agg;
        s_state.lon_agg = s_restore_entry.lon_agg;
        s_state.alt_agg = s_restore_entry.alt_agg;
        s_state.hprec_agg = s_restore_entry.hprec_agg;
        s_state.window_has_data = true; /* get_sleep_state() only ever captures a window with data */
        s_state.window_timestamp_unix = s_restore_entry.window_timestamp_unix;
        s_state.window_time_synced = s_restore_entry.window_time_synced;
        s_state.next_log_us = s_restore_entry.next_log_due_is_synced
            ? esp_timer_get_time() + (s_restore_entry.next_log_due_unix - (int64_t)time(NULL)) * 1000000LL
            : esp_timer_get_time(); /* no usable deadline - log ASAP rather than lose the accumulated data */
        s_has_restore_entry = false;
    }

    for (;;) {
        position_settings_t pos;
        config_store_get_position_settings(&pos);

        if (!pos.enabled || pos.sample_interval_ms == 0 || pos.log_interval_ms == 0) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            aggregator_reset(&s_state.lat_agg);
            aggregator_reset(&s_state.lon_agg);
            aggregator_reset(&s_state.alt_agg);
            aggregator_reset(&s_state.hprec_agg);
            s_state.window_has_data = false;
            s_state.next_log_us = 0;
            xSemaphoreGive(s_state_mutex);
            vTaskDelay(pdMS_TO_TICKS(IDLE_CHECK_MS));
            continue;
        }

        gnss_fix_t fix;
        esp_err_t err = cellular_transport_acquire_gnss_fix(&fix, GNSS_FIX_TIMEOUT_MS);
        if (err == ESP_OK && fix.valid) {
            bool time_synced = fix.timestamp_unix >= TIME_SYNC_EPOCH_THRESHOLD;
            if (time_synced) {
                /* GNSS is the preferred time source whenever position
                 * reporting is enabled (see cellular_transport.cpp's
                 * cellular_task() for the NITZ fallback used when it
                 * isn't) - a satellite-derived timestamp, no less
                 * accurate than what NITZ would give. */
                time_sync_set_from_epoch(fix.timestamp_unix, "GNSS");
            }
            ESP_LOGI(TAG, "fix: lat=%.6f lon=%.6f alt=%.1fm h_prec=%.1fm", fix.latitude, fix.longitude,
                     fix.altitude_m, fix.horizontal_accuracy_m);

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            aggregator_add_sample(&s_state.lat_agg, fix.latitude);
            aggregator_add_sample(&s_state.lon_agg, fix.longitude);
            aggregator_add_sample(&s_state.alt_agg, (double)fix.altitude_m);
            aggregator_add_sample(&s_state.hprec_agg, (double)fix.horizontal_accuracy_m);
            s_state.window_timestamp_unix = fix.timestamp_unix;
            s_state.window_time_synced = time_synced;
            if (!s_state.window_has_data) {
                s_state.window_has_data = true;
                s_state.next_log_us = esp_timer_get_time() + (int64_t)pos.log_interval_ms * 1000;
            }
            xSemaphoreGive(s_state_mutex);
        } else {
            ESP_LOGW(TAG, "GNSS fix failed: %s", esp_err_to_name(err));
        }

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        bool should_log = s_state.window_has_data && esp_timer_get_time() >= s_state.next_log_us;
        field_aggregate_t lat_a = { 0 }, lon_a = { 0 }, alt_a = { 0 }, hprec_a = { 0 };
        uint32_t sample_count = 0;
        int64_t log_timestamp_unix = 0;
        bool log_time_synced = false;
        if (should_log) {
            lat_a = finalize(&s_state.lat_agg);
            lon_a = finalize(&s_state.lon_agg);
            alt_a = finalize(&s_state.alt_agg);
            hprec_a = finalize(&s_state.hprec_agg);
            sample_count = s_state.lat_agg.count;
            log_timestamp_unix = s_state.window_timestamp_unix;
            log_time_synced = s_state.window_time_synced;

            aggregator_reset(&s_state.lat_agg);
            aggregator_reset(&s_state.lon_agg);
            aggregator_reset(&s_state.alt_agg);
            aggregator_reset(&s_state.hprec_agg);
            s_state.window_has_data = false;
            s_state.next_log_us = 0;
        }
        xSemaphoreGive(s_state_mutex);

        /* Publish outside the critical section - sd_logger/MQTT can block
         * on I/O and must not hold up a concurrent sleep-status query. */
        if (should_log) {
            sd_logger_log_position(log_timestamp_unix, log_time_synced, sample_count, pos.aggregate_mask, &lat_a,
                                    &lon_a, &alt_a, &hprec_a);
            mqttc_publish_position(log_timestamp_unix, log_time_synced, sample_count, pos.aggregate_mask, &lat_a,
                                    &lon_a, &alt_a, &hprec_a);
        }

        vTaskDelay(pdMS_TO_TICKS(pos.sample_interval_ms));
    }
}

esp_err_t gnss_position_init(void)
{
    net_settings_t net;
    config_store_get_net_settings(&net);
    if (net.transport != TRANSPORT_CELLULAR) {
        return ESP_OK; /* GNSS lives on the cellular modem chip - nothing to do without it */
    }

    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (!s_state_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ok = xTaskCreate(gnss_position_task, "gnss_pos", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ---------------------------------------------------------------------
 * deep sleep support (power_manager)
 * ------------------------------------------------------------------- */

bool gnss_position_get_sleep_state(gnss_sleep_entry_t *out)
{
    if (!out || !s_state_mutex) {
        return false;
    }

    int64_t now_unix = time(NULL);
    bool synced = now_unix >= TIME_SYNC_EPOCH_THRESHOLD;
    int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool has_data = s_state.window_has_data;
    if (has_data) {
        out->lat_agg = s_state.lat_agg;
        out->lon_agg = s_state.lon_agg;
        out->alt_agg = s_state.alt_agg;
        out->hprec_agg = s_state.hprec_agg;
        out->window_timestamp_unix = s_state.window_timestamp_unix;
        out->window_time_synced = s_state.window_time_synced;
        out->next_log_due_is_synced = synced;
        out->next_log_due_unix = synced ? now_unix + (s_state.next_log_us - now_us) / 1000000LL : 0;
    }
    xSemaphoreGive(s_state_mutex);

    return has_data;
}

void gnss_position_restore_sleep_state(const gnss_sleep_entry_t *in)
{
    if (!in) {
        return;
    }
    s_restore_entry = *in;
    s_has_restore_entry = true;
}

gnss_position_sleep_status_t gnss_position_get_sleep_status(void)
{
    gnss_position_sleep_status_t status = { 0 };

    position_settings_t pos;
    config_store_get_position_settings(&pos);
    if (!pos.enabled || pos.sample_interval_ms == 0 || pos.log_interval_ms == 0) {
        return status; /* has_schedulable=false: position reporting is off, nothing to wait on */
    }

    if (pos.sample_interval_ms < pos.log_interval_ms && !pos.allow_skip_during_sleep) {
        status.blocked = true;
        snprintf(status.blocked_reason, sizeof(status.blocked_reason), "Position");
        return status;
    }

    status.has_schedulable = true;

    int64_t now_unix = time(NULL);
    bool synced = now_unix >= TIME_SYNC_EPOCH_THRESHOLD;
    if (!synced || !s_state_mutex) {
        return status; /* next_due_is_synced stays false - no usable wall-clock bound yet */
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool window_has_data = s_state.window_has_data;
    int64_t next_log_us = s_state.next_log_us;
    xSemaphoreGive(s_state_mutex);

    int64_t now_us = esp_timer_get_time();
    status.next_due_is_synced = true;
    status.next_due_unix = window_has_data ? now_unix + (next_log_us - now_us) / 1000000LL
                                            : now_unix + (int64_t)pos.sample_interval_ms / 1000;
    return status;
}
