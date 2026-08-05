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

static void battery_monitor_task(void *pvParams)
{
    (void)pvParams;

    for (;;) {
        battery_settings_t bat;
        config_store_get_battery_settings(&bat);

        if (!bat.enabled || bat.interval_ms == 0) {
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

        vTaskDelay(pdMS_TO_TICKS(bat.interval_ms));
    }
}

esp_err_t battery_monitor_init(void)
{
    BaseType_t ok = xTaskCreate(battery_monitor_task, "battery_mon", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
