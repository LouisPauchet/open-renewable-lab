#include <inttypes.h>

#include "config_store.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "app_main";

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

    /* Further component init/task spawn is added incrementally as each
     * subsystem (sampling_engine, sd_logger, web_portal, net_manager,
     * mqtt_client) lands - see the project plan for build order. */
}
