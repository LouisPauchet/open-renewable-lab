#include <inttypes.h>

#include "config_store.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sampling_engine.h"
#include "sd_logger.h"
#include "stub_sensor.h"

static const char *TAG = "app_main";

/* Temporary stand-in for sd_logger/mqtt_publish_task sinks (added in
 * later build stages) - just logs every finalized aggregate so the
 * sampling/aggregation pipeline is observable before real sinks exist. */
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
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Walter sensor node booting");
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

    sampling_engine_register_bus_driver(BUS_TYPE_SDI12, stub_sensor_read);
    sampling_engine_register_bus_driver(BUS_TYPE_I2C, stub_sensor_read);
    ESP_ERROR_CHECK(sampling_engine_init());

    QueueHandle_t debug_sink = xQueueCreate(16, sizeof(aggregate_result_t));
    ESP_ERROR_CHECK(sampling_engine_add_result_sink(debug_sink));
    xTaskCreate(debug_result_sink_task, "debug_sink", 4096, debug_sink, tskIDLE_PRIORITY + 1, NULL);

    if (sd_logger_init() == ESP_OK) {
        ESP_ERROR_CHECK(sampling_engine_add_result_sink(sd_logger_get_sink_queue()));
    } else {
        ESP_LOGW(TAG, "SD logging unavailable (no card, or board_pins.h SD pins not yet configured)");
    }

    /* Further component init/task spawn is added incrementally as each
     * subsystem (web_portal, net_manager, mqtt_client) lands - see the
     * project plan for build order. */
}
