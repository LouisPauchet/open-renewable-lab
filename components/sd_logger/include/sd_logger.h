#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_LOGGER_MOUNT_POINT "/sdcard"

/* Mounts the microSD card (SDMMC 1-bit mode, pins from board_pins.h)
 * and starts the CSV writer task. If the required pins aren't configured yet, or
 * the mount fails (no card present, card unreadable, ...), this logs an
 * error and returns the failure - it does NOT abort boot. Callers
 * should treat SD logging as best-effort: check sd_logger_is_ready()
 * rather than ESP_ERROR_CHECK-ing this. */
esp_err_t sd_logger_init(void);

bool sd_logger_is_ready(void);

/* Queue to pass to sampling_engine_add_result_sink(). Returns NULL if
 * the SD card isn't mounted. */
QueueHandle_t sd_logger_get_sink_queue(void);

/* Logs a GNSS fix to a separate position_YYYYMMDD.csv (day-bucketed,
 * same convention as sensor data). Non-blocking; drops (counted in
 * sd_logger_get_drop_count()) rather than ever blocking the caller. */
esp_err_t sd_logger_log_position(int64_t timestamp_unix, bool time_is_synced, double latitude, double longitude,
                                  float altitude_m);

/* For the web portal's /api/status. Returns ESP_ERR_INVALID_STATE if
 * not mounted. */
esp_err_t sd_logger_get_space(uint64_t *out_total_bytes, uint64_t *out_free_bytes);

/* Count of rows dropped because the queue was full or a write failed
 * (card removed, filesystem error, ...) - surfaced in /api/status so a
 * student can notice a flaky card. */
uint32_t sd_logger_get_drop_count(void);

#ifdef __cplusplus
}
#endif
