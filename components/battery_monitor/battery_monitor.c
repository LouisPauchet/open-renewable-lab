#include "battery_monitor.h"

#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_sensor_registry.h"
#include "mqtt_client_bridge.h"

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

static void battery_monitor_task(void *pvParams)
{
    (void)pvParams;

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

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

esp_err_t battery_monitor_init(void)
{
    BaseType_t ok = xTaskCreate(battery_monitor_task, "battery_mon", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
