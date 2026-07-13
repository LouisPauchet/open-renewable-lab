#include "sd_logger.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "board_pins.h"
#include "device_id.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "sampling_engine.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_logger";
#define DATA_DIR SD_LOGGER_MOUNT_POINT "/data"
#define RESULT_QUEUE_LEN 32
#define POSITION_QUEUE_LEN 4

typedef struct {
    int64_t timestamp_unix;
    bool time_is_synced;
    double latitude;
    double longitude;
    float altitude_m;
} position_row_t;

static QueueHandle_t s_queue;
static QueueHandle_t s_position_queue;
static sdmmc_card_t *s_card;
static bool s_ready;
static uint32_t s_drop_count;

static void write_csv_string_field(FILE *f, const char *s)
{
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"') {
            fputc('"', f);
        }
        fputc(*p, f);
    }
    fputc('"', f);
}

static void build_path_for_timestamp(const char *prefix, int64_t timestamp_unix, char *out, size_t out_size)
{
    time_t t = (time_t)timestamp_unix;
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    /* Unsynced results land in <prefix>_19700101.csv (epoch ~0) - a
     * well-defined, easy-to-spot bucket rather than special-cased
     * handling. */
    snprintf(out, out_size, DATA_DIR "/%s_%04d%02d%02d.csv", prefix, tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
             tm_utc.tm_mday);
}

static void write_result_row(const aggregate_result_t *r)
{
    char path[80];
    build_path_for_timestamp("sensors", r->timestamp_unix, path, sizeof(path));

    struct stat st;
    bool is_new_file = stat(path, &st) != 0;

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s for append", path);
        s_drop_count++;
        return;
    }

    if (is_new_file) {
        fprintf(f, "# device_id=%s\n", device_id_get());
        fprintf(f, "timestamp_unix,time_synced,variable_id,name,sample_count,aggregate_mask,raw,mean,min,max,stddev\n");
    }

    fprintf(f, "%lld,%d,%u,", (long long)r->timestamp_unix, (int)r->time_is_synced, (unsigned)r->variable_id);
    write_csv_string_field(f, r->name);
    fprintf(f, ",%" PRIu32 ",%u,%.6f,%.6f,%.6f,%.6f,%.6f\n", r->sample_count, (unsigned)r->aggregate_mask, r->raw,
            r->mean, r->min, r->max, r->stddev);

    fclose(f);
}

static void write_position_row(const position_row_t *p)
{
    char path[80];
    build_path_for_timestamp("position", p->timestamp_unix, path, sizeof(path));

    struct stat st;
    bool is_new_file = stat(path, &st) != 0;

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s for append", path);
        s_drop_count++;
        return;
    }

    if (is_new_file) {
        fprintf(f, "# device_id=%s\n", device_id_get());
        fprintf(f, "timestamp_unix,time_synced,latitude,longitude,altitude_m\n");
    }

    fprintf(f, "%lld,%d,%.6f,%.6f,%.2f\n", (long long)p->timestamp_unix, (int)p->time_is_synced, p->latitude,
            p->longitude, p->altitude_m);

    fclose(f);
}

static void sd_writer_task(void *pvParams)
{
    (void)pvParams;
    esp_task_wdt_add(NULL);
    aggregate_result_t result;
    position_row_t position;
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(s_queue, &result, pdMS_TO_TICKS(5000)) == pdTRUE) {
            write_result_row(&result);
        }
        while (xQueueReceive(s_position_queue, &position, 0) == pdTRUE) {
            write_position_row(&position);
        }
    }
}

static esp_err_t ensure_data_dir(void)
{
    struct stat st;
    if (stat(DATA_DIR, &st) == 0) {
        return ESP_OK;
    }
    if (mkdir(DATA_DIR, 0755) != 0) {
        ESP_LOGE(TAG, "failed to create %s", DATA_DIR);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_logger_init(void)
{
    if (!board_pin_is_set(BOARD_PIN_SD_CLK) || !board_pin_is_set(BOARD_PIN_SD_CMD) ||
        !board_pin_is_set(BOARD_PIN_SD_D0)) {
        ESP_LOGE(TAG, "SD SDMMC pins not configured in board_pins.h, SD logging disabled");
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1; /* only CMD/CLK/D0 are wired on Walter Feels - no 4-bit mode */
    slot_cfg.clk = BOARD_PIN_SD_CLK;
    slot_cfg.cmd = BOARD_PIN_SD_CMD;
    slot_cfg.d0 = BOARD_PIN_SD_D0;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (board_pin_is_set(BOARD_PIN_SD_CARD_DETECT)) {
        slot_cfg.cd = BOARD_PIN_SD_CARD_DETECT;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_LOGGER_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount SD card: %s", esp_err_to_name(err));
        return err;
    }

    err = ensure_data_dir();
    if (err != ESP_OK) {
        esp_vfs_fat_sdcard_unmount(SD_LOGGER_MOUNT_POINT, s_card);
        return err;
    }

    s_queue = xQueueCreate(RESULT_QUEUE_LEN, sizeof(aggregate_result_t));
    s_position_queue = xQueueCreate(POSITION_QUEUE_LEN, sizeof(position_row_t));
    if (!s_queue || !s_position_queue) {
        esp_vfs_fat_sdcard_unmount(SD_LOGGER_MOUNT_POINT, s_card);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(sd_writer_task, "sd_writer", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        esp_vfs_fat_sdcard_unmount(SD_LOGGER_MOUNT_POINT, s_card);
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "SD card mounted at %s, logging to %s", SD_LOGGER_MOUNT_POINT, DATA_DIR);
    return ESP_OK;
}

bool sd_logger_is_ready(void)
{
    return s_ready;
}

QueueHandle_t sd_logger_get_sink_queue(void)
{
    return s_ready ? s_queue : NULL;
}

esp_err_t sd_logger_log_position(int64_t timestamp_unix, bool time_is_synced, double latitude, double longitude,
                                  float altitude_m)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    position_row_t row = {
        .timestamp_unix = timestamp_unix,
        .time_is_synced = time_is_synced,
        .latitude = latitude,
        .longitude = longitude,
        .altitude_m = altitude_m,
    };
    if (xQueueSend(s_position_queue, &row, 0) != pdTRUE) {
        s_drop_count++;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sd_logger_get_space(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(SD_LOGGER_MOUNT_POINT, out_total_bytes, out_free_bytes);
}

uint32_t sd_logger_get_drop_count(void)
{
    return s_drop_count;
}
