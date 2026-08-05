#include <inttypes.h>

#include "battery_monitor.h"
#include "board_pins.h"
#include "cellular_transport.h"
#include "config_store.h"
#include "device_id.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gnss_position.h"
#include "i2c_bus.h"
#include "i2c_sensor_registry.h"
#include "mqtt_client_bridge.h"
#include "net_manager.h"
#include "onboard_i2c_bus.h"
#include "sampling_engine.h"
#include "sd_logger.h"
#include "sdi12_bus.h"
#include "stub_sensor.h"
#include "time_sync.h"
#include "web_portal.h"

static const char *TAG = "app_main";

/* Lightweight console visibility: logs every finalized aggregate
 * alongside the SD/MQTT sinks - handy during bring-up and for a
 * student watching the serial monitor. */
static void debug_result_sink_task(void *pvParams)
{
    QueueHandle_t queue = (QueueHandle_t)pvParams;
    aggregate_result_t result;
    for (;;) {
        if (xQueueReceive(queue, &result, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG,
                     "[%s] n=%" PRIu32 " raw=%.2f mean=%.2f min=%.2f max=%.2f stddev=%.2f synced=%d",
                     result.name, result.sample_count, result.raw, result.mean, result.min, result.max,
                     result.stddev, (int)result.time_is_synced);
        }
    }
}

void app_main(void)
{
    /* Must happen before any peripheral bus init below - the external
     * I2C connector, onboard I2C sensor bus, and SD card are all fed
     * from this switched rail on Walter Feels (see board_pins.h). */
    board_pins_enable_3v3_sw();

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Walter sensor node booting, device_id=%s", device_id_get());
    ESP_LOGI(TAG, "chip: %s, cores: %d, revision: v%d.%d",
             CONFIG_IDF_TARGET, chip_info.cores,
             chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "flash: %" PRIu32 "MB, free heap: %" PRIu32 " bytes",
             flash_size / (1024 * 1024), esp_get_free_heap_size());

    ESP_ERROR_CHECK(config_store_init());
    device_config_t cfg;
    config_store_get_snapshot(&cfg);
    ESP_LOGI(TAG, "config loaded: %u variable(s), generation=%" PRIu32,
             (unsigned)cfg.variable_count, cfg.generation);

    if (sdi12_bus_init() == ESP_OK) {
        sampling_engine_register_bus_driver(BUS_TYPE_SDI12, sdi12_variable_read);
    } else {
        ESP_LOGW(TAG, "SDI-12 bus unavailable (board_pins.h SDI-12 pins not configured), using stub sensor");
        sampling_engine_register_bus_driver(BUS_TYPE_SDI12, stub_sensor_read);
    }
    if (i2c_bus_init() == ESP_OK) {
        sampling_engine_register_bus_driver(BUS_TYPE_I2C, i2c_variable_read);
    } else {
        ESP_LOGW(TAG, "I2C bus unavailable (board_pins.h I2C pins not configured), using stub sensor");
        sampling_engine_register_bus_driver(BUS_TYPE_I2C, stub_sensor_read);
    }
    /* Separate physical bus from the one above (Walter Feels' onboard
     * HDC1080/LPS22HB sensors) - i2c_variable_read() already routes to
     * whichever bus a given I2C device_type actually lives on, so no
     * separate BUS_TYPE/driver registration is needed here, just init. */
    if (onboard_i2c_bus_init() != ESP_OK) {
        ESP_LOGW(TAG, "onboard sensor I2C bus unavailable (board_pins.h pins not configured) - "
                      "HDC1080/LPS22HB variables will fail to read");
    }
    ESP_ERROR_CHECK(sampling_engine_init());
    ESP_ERROR_CHECK(battery_monitor_init());

    QueueHandle_t debug_sink = xQueueCreate(16, sizeof(aggregate_result_t));
    ESP_ERROR_CHECK(sampling_engine_add_result_sink(debug_sink));
    xTaskCreate(debug_result_sink_task, "debug_sink", 4096, debug_sink, tskIDLE_PRIORITY + 1, NULL);

    if (sd_logger_init() == ESP_OK) {
        ESP_ERROR_CHECK(sampling_engine_add_result_sink(sd_logger_get_sink_queue()));
    } else {
        ESP_LOGW(TAG, "SD logging unavailable (no card, or board_pins.h SD pins not yet configured)");
    }

    ESP_ERROR_CHECK(net_manager_init());
    ESP_ERROR_CHECK(web_portal_init());
    ESP_ERROR_CHECK(time_sync_init());

    if (cfg.net.transport == TRANSPORT_CELLULAR) {
        if (cellular_transport_init() == ESP_OK) {
            ESP_ERROR_CHECK(gnss_position_init());
        } else {
            ESP_LOGE(TAG, "cellular_transport_init failed - see its header for the unverified-SDK-calls caveat");
        }
    }

    ESP_ERROR_CHECK(mqttc_init());
}
