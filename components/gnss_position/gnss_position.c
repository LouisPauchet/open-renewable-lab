#include "gnss_position.h"

#include "cellular_transport.h"
#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client_bridge.h"
#include "sd_logger.h"
#include "time_sync.h"

static const char *TAG = "gnss_position";

#define GNSS_FIX_TIMEOUT_MS 60000 /* a cold GNSS fix can legitimately take up to ~1 min */
#define IDLE_CHECK_MS 5000

/* Deliberately NOT registered with esp_task_wdt: a GNSS fix can
 * legitimately block for up to GNSS_FIX_TIMEOUT_MS, well past the
 * watchdog timeout - same reasoning as sampling_engine's SDI-12/I2C
 * bus scheduler tasks. */
static void gnss_position_task(void *pvParams)
{
    (void)pvParams;

    for (;;) {
        position_settings_t pos;
        config_store_get_position_settings(&pos);

        if (!pos.enabled || pos.interval_ms == 0) {
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
            ESP_LOGI(TAG, "fix: lat=%.6f lon=%.6f alt=%.1fm", fix.latitude, fix.longitude, fix.altitude_m);
            sd_logger_log_position(fix.timestamp_unix, time_synced, fix.latitude, fix.longitude, fix.altitude_m);
            mqttc_publish_position(&fix);
        } else {
            ESP_LOGW(TAG, "GNSS fix failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(pos.interval_ms));
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
