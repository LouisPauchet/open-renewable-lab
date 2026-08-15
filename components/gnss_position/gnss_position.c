#include "gnss_position.h"

#include "aggregator.h"
#include "cellular_transport.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client_bridge.h"
#include "sd_logger.h"
#include "time_sync.h"

static const char *TAG = "gnss_position";

#define GNSS_FIX_TIMEOUT_MS 60000 /* a cold GNSS fix can legitimately take up to ~1 min */
#define IDLE_CHECK_MS 5000

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

    /* Same sample/log-interval + aggregate model as a regular variable
     * (see position_settings_t's own comment): latitude, longitude,
     * elevation, and horizontal precision are each fed into their own
     * Welford accumulator (aggregator.h - the same one sampling_engine
     * itself uses) once per successful fix, and finalized/logged/
     * published once per log_interval_ms rather than after every single
     * fix. Implemented locally here rather than through
     * sampling_engine's generic bus-driver path, since position keeps
     * its own dedicated CSV file / MQTT payload shape (one combined
     * record) instead of becoming four separate generic variables. */
    aggregator_t lat_agg, lon_agg, alt_agg, hprec_agg;
    aggregator_reset(&lat_agg);
    aggregator_reset(&lon_agg);
    aggregator_reset(&alt_agg);
    aggregator_reset(&hprec_agg);
    bool window_has_data = false;
    int64_t next_log_us = 0;
    int64_t window_timestamp_unix = 0;
    bool window_time_synced = false;

    for (;;) {
        position_settings_t pos;
        config_store_get_position_settings(&pos);

        if (!pos.enabled || pos.sample_interval_ms == 0 || pos.log_interval_ms == 0) {
            aggregator_reset(&lat_agg);
            aggregator_reset(&lon_agg);
            aggregator_reset(&alt_agg);
            aggregator_reset(&hprec_agg);
            window_has_data = false;
            next_log_us = 0;
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

            aggregator_add_sample(&lat_agg, fix.latitude);
            aggregator_add_sample(&lon_agg, fix.longitude);
            aggregator_add_sample(&alt_agg, (double)fix.altitude_m);
            aggregator_add_sample(&hprec_agg, (double)fix.horizontal_accuracy_m);
            window_timestamp_unix = fix.timestamp_unix;
            window_time_synced = time_synced;
            if (!window_has_data) {
                window_has_data = true;
                next_log_us = esp_timer_get_time() + (int64_t)pos.log_interval_ms * 1000;
            }
        } else {
            ESP_LOGW(TAG, "GNSS fix failed: %s", esp_err_to_name(err));
        }

        if (window_has_data && esp_timer_get_time() >= next_log_us) {
            field_aggregate_t lat_a = finalize(&lat_agg);
            field_aggregate_t lon_a = finalize(&lon_agg);
            field_aggregate_t alt_a = finalize(&alt_agg);
            field_aggregate_t hprec_a = finalize(&hprec_agg);

            sd_logger_log_position(window_timestamp_unix, window_time_synced, lat_agg.count, pos.aggregate_mask,
                                    &lat_a, &lon_a, &alt_a, &hprec_a);
            mqttc_publish_position(window_timestamp_unix, window_time_synced, lat_agg.count, pos.aggregate_mask,
                                    &lat_a, &lon_a, &alt_a, &hprec_a);

            aggregator_reset(&lat_agg);
            aggregator_reset(&lon_agg);
            aggregator_reset(&alt_agg);
            aggregator_reset(&hprec_agg);
            window_has_data = false;
            next_log_us = 0;
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

    BaseType_t ok = xTaskCreate(gnss_position_task, "gnss_pos", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
