#include "sd_logger.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "board_pins.h"
#include "config_store.h"
#include "device_id.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/semphr.h"
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
    uint32_t sample_count;
    uint8_t aggregate_mask;
    field_aggregate_t latitude;
    field_aggregate_t longitude;
    field_aggregate_t elevation_m;
    field_aggregate_t h_precision_m;
} position_row_t;

/* ---------------------------------------------------------------------
 * Wide-format ("one row per scan", grouped by log_interval_ms) state.
 *
 * Each aggregate_result_t arrives independently, whenever ITS variable's
 * own aggregation cycle completes (sampling_engine.c's aggregation_task
 * checks every variable on the same 200ms poll tick, so variables
 * sharing a log_interval_ms and added/edited together stay tightly in
 * sync - see aggregation_task's rebuild_agg_table() preserving existing
 * schedules across unrelated config edits). A group's pending row is
 * flushed as soon as it's complete, or - to stay robust to a
 * disconnected/dead sensor never reporting - once (log_interval_ms +
 * a grace period) has passed since the row was opened, whichever comes
 * first. A variable reporting again while its slot in the current
 * pending row is already filled means a new scan has started; that
 * also flushes the (possibly incomplete) row before starting the next.
 * ------------------------------------------------------------------- */

#define MAX_INTERVAL_GROUPS 8
#define GROUP_TIMEOUT_GRACE_MS 5000

typedef struct {
    uint16_t variable_id;
    char name[32];
    char unit[16];
    uint8_t aggregate_mask;
    bool has_value;
    aggregate_result_t last_value;
} group_member_t;

typedef struct {
    bool in_use;
    uint32_t log_interval_ms;
    group_member_t members[MAX_VARIABLES];
    size_t member_count;
    int64_t pending_row_started_us; /* 0 = no pending row open */
    uint32_t record_number;         /* TOA5-style RECORD column; increments per flushed row */
} interval_group_t;

typedef struct {
    uint8_t bit;
    const char *simple_suffix; /* WIDE_SIMPLE column-name suffix, e.g. "..._mean_V" */
    const char *toa5_suffix;   /* WIDE_TOA5 field-name suffix + process-type row, e.g. "Avg" */
} agg_col_t;

static const agg_col_t AGG_COLUMNS[] = {
    { AGG_RAW, "raw", "Smp" },     { AGG_MEAN, "mean", "Avg" }, { AGG_MIN, "min", "Min" },
    { AGG_MAX, "max", "Max" },     { AGG_STDDEV, "stddev", "Std" },
};
#define AGG_COLUMN_COUNT (sizeof(AGG_COLUMNS) / sizeof(AGG_COLUMNS[0]))

static interval_group_t s_groups[MAX_INTERVAL_GROUPS];
static size_t s_group_count;
static uint32_t s_groups_generation = UINT32_MAX;

static QueueHandle_t s_queue;
static QueueHandle_t s_position_queue;
static sdmmc_card_t *s_card;
static bool s_ready;
static uint32_t s_drop_count;

/* Deep-sleep flush handshake (see sd_logger_flush_now()): power_manager
 * gives s_flush_request and blocks on s_flush_done; sd_writer_task polls
 * s_flush_request each iteration and, if set, drains both queues fully
 * and closes any open wide-format group row before giving s_flush_done
 * back. */
static SemaphoreHandle_t s_flush_request;
static SemaphoreHandle_t s_flush_done;
#define SD_FLUSH_TIMEOUT_MS 5000

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

/* For unquoted CSV contexts (WIDE_SIMPLE's baked-in column names) -
 * student-chosen variable names/units aren't guaranteed comma/quote
 * free, so sanitize before using one as a bare column-name segment. */
static void sanitize_csv_token(const char *in, char *out, size_t out_size)
{
    size_t i = 0;
    for (; in[i] && i < out_size - 1; i++) {
        char c = in[i];
        out[i] = (c == ',' || c == '"' || c == '\n' || c == '\r') ? '_' : c;
    }
    out[i] = '\0';
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

/* ---------------------------------------------------------------------
 * Wide-format ("scan"/TOA5-style) logging
 * ------------------------------------------------------------------- */

static void format_interval_suffix(uint32_t log_interval_ms, char *out, size_t out_size)
{
    if (log_interval_ms > 0 && log_interval_ms % 1000 == 0) {
        snprintf(out, out_size, "%us", (unsigned)(log_interval_ms / 1000));
    } else {
        snprintf(out, out_size, "%ums", (unsigned)log_interval_ms);
    }
}

/* FNV-1a - just needs to be deterministic and cheap, not cryptographic. */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

/* Builds the field-names row as a plain '|'-joined string (used only to
 * derive a stable hash of the current column set - never written to
 * the file directly). Embedding this hash in the filename means a
 * config change that alters a group's columns (variable added/removed/
 * renamed, aggregate mask changed) transparently rolls over to a new
 * file instead of corrupting an existing file's column alignment -
 * without needing to read back and compare any existing file's header. */
static void build_column_signature(const interval_group_t *g, char *out, size_t out_size)
{
    size_t off = 0;
    for (size_t mi = 0; mi < g->member_count && off < out_size; mi++) {
        const group_member_t *m = &g->members[mi];
        for (size_t k = 0; k < AGG_COLUMN_COUNT; k++) {
            if (!(m->aggregate_mask & AGG_COLUMNS[k].bit)) {
                continue;
            }
            off += snprintf(out + off, off < out_size ? out_size - off : 0, "%s|%s|%s|", m->name, m->unit,
                             AGG_COLUMNS[k].simple_suffix);
        }
    }
}

static void build_group_path(const interval_group_t *g, int64_t timestamp_unix, char *out, size_t out_size)
{
    char interval_suffix[16];
    format_interval_suffix(g->log_interval_ms, interval_suffix, sizeof(interval_suffix));

    /* static: too large for sd_writer_task's stack (a real stack
     * overflow, confirmed on real hardware) - safe since this function
     * is only ever called from that single task, never concurrently. */
    static char signature[2048];
    build_column_signature(g, signature, sizeof(signature));
    uint32_t sig_hash = fnv1a(signature);

    time_t t = (time_t)timestamp_unix;
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    snprintf(out, out_size, DATA_DIR "/sensors_iv%s_%08" PRIx32 "_%04d%02d%02d.csv", interval_suffix, sig_hash,
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
}

static void write_group_header(FILE *f, const interval_group_t *g, sd_log_format_t fmt, const char *station_name)
{
    if (fmt == SD_LOG_FORMAT_WIDE_TOA5) {
        char interval_suffix[16];
        format_interval_suffix(g->log_interval_ms, interval_suffix, sizeof(interval_suffix));
        fprintf(f, "\"TOA5\",\"%s\",\"WalterSensorNode\",\"%s\",\"IDF%s\",\"walter_sensor_node\",\"0\",\"Interval_%s\"\n",
                station_name, device_id_get(), IDF_VER, interval_suffix);

        fprintf(f, "\"TIMESTAMP\",\"RECORD\"");
        for (size_t mi = 0; mi < g->member_count; mi++) {
            const group_member_t *m = &g->members[mi];
            for (size_t k = 0; k < AGG_COLUMN_COUNT; k++) {
                if (m->aggregate_mask & AGG_COLUMNS[k].bit) {
                    fprintf(f, ",\"%s_%s\"", m->name, AGG_COLUMNS[k].toa5_suffix);
                }
            }
        }
        fprintf(f, "\n\"TS\",\"RN\"");
        for (size_t mi = 0; mi < g->member_count; mi++) {
            const group_member_t *m = &g->members[mi];
            for (size_t k = 0; k < AGG_COLUMN_COUNT; k++) {
                if (m->aggregate_mask & AGG_COLUMNS[k].bit) {
                    fprintf(f, ",\"%s\"", m->unit);
                }
            }
        }
        fprintf(f, "\n\"\",\"\"");
        for (size_t mi = 0; mi < g->member_count; mi++) {
            const group_member_t *m = &g->members[mi];
            for (size_t k = 0; k < AGG_COLUMN_COUNT; k++) {
                if (m->aggregate_mask & AGG_COLUMNS[k].bit) {
                    fprintf(f, ",\"%s\"", AGG_COLUMNS[k].toa5_suffix);
                }
            }
        }
        fprintf(f, "\n");
    } else { /* SD_LOG_FORMAT_WIDE_SIMPLE */
        fprintf(f, "# station_name=%s,device_id=%s,interval_ms=%u\n", station_name, device_id_get(),
                (unsigned)g->log_interval_ms);
        fprintf(f, "timestamp_unix,time_synced,record");
        for (size_t mi = 0; mi < g->member_count; mi++) {
            const group_member_t *m = &g->members[mi];
            char safe_name[32];
            char safe_unit[16];
            sanitize_csv_token(m->name, safe_name, sizeof(safe_name));
            sanitize_csv_token(m->unit, safe_unit, sizeof(safe_unit));
            for (size_t k = 0; k < AGG_COLUMN_COUNT; k++) {
                if (m->aggregate_mask & AGG_COLUMNS[k].bit) {
                    if (safe_unit[0]) {
                        fprintf(f, ",%s_%s_%s", safe_name, AGG_COLUMNS[k].simple_suffix, safe_unit);
                    } else {
                        fprintf(f, ",%s_%s", safe_name, AGG_COLUMNS[k].simple_suffix);
                    }
                }
            }
        }
        fprintf(f, "\n");
    }
}

static void write_group_data_row(FILE *f, const interval_group_t *g, sd_log_format_t fmt, int64_t timestamp_unix,
                                  bool time_is_synced)
{
    if (fmt == SD_LOG_FORMAT_WIDE_TOA5) {
        char ts_str[24];
        time_t t = (time_t)timestamp_unix;
        struct tm tm_utc;
        gmtime_r(&t, &tm_utc);
        strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M:%S", &tm_utc);
        fprintf(f, "\"%s\",%" PRIu32, ts_str, g->record_number);
    } else {
        fprintf(f, "%lld,%d,%" PRIu32, (long long)timestamp_unix, (int)time_is_synced, g->record_number);
    }

    for (size_t mi = 0; mi < g->member_count; mi++) {
        const group_member_t *m = &g->members[mi];
        for (size_t k = 0; k < AGG_COLUMN_COUNT; k++) {
            if (!(m->aggregate_mask & AGG_COLUMNS[k].bit)) {
                continue;
            }
            if (!m->has_value) {
                fputc(',', f); /* blank cell - this member never reported for this scan */
                continue;
            }
            double value = 0.0;
            switch (AGG_COLUMNS[k].bit) {
                case AGG_RAW: value = m->last_value.raw; break;
                case AGG_MEAN: value = m->last_value.mean; break;
                case AGG_MIN: value = m->last_value.min; break;
                case AGG_MAX: value = m->last_value.max; break;
                case AGG_STDDEV: value = m->last_value.stddev; break;
                default: break;
            }
            fprintf(f, ",%.6f", value);
        }
    }
    fprintf(f, "\n");
}

static void flush_group_row(interval_group_t *g)
{
    bool any_value = false;
    int64_t timestamp_unix = 0;
    bool time_is_synced = false;
    for (size_t mi = 0; mi < g->member_count; mi++) {
        if (g->members[mi].has_value) {
            timestamp_unix = g->members[mi].last_value.timestamp_unix;
            time_is_synced = g->members[mi].last_value.time_is_synced;
            any_value = true;
            break;
        }
    }

    if (any_value) {
        sd_settings_t sd_cfg;
        config_store_get_sd_settings(&sd_cfg);
        const char *station_name = sd_cfg.station_name[0] ? sd_cfg.station_name : device_id_get();

        char path[128];
        build_group_path(g, timestamp_unix, path, sizeof(path));

        struct stat st;
        bool is_new_file = stat(path, &st) != 0;

        FILE *f = fopen(path, "a");
        if (!f) {
            ESP_LOGE(TAG, "failed to open %s for append", path);
            s_drop_count++;
        } else {
            if (is_new_file) {
                write_group_header(f, g, sd_cfg.log_format, station_name);
            }
            write_group_data_row(f, g, sd_cfg.log_format, timestamp_unix, time_is_synced);
            fclose(f);
            g->record_number++;
        }
    }

    for (size_t mi = 0; mi < g->member_count; mi++) {
        g->members[mi].has_value = false;
    }
    g->pending_row_started_us = 0;
}

static void flush_all_groups(void)
{
    for (size_t gi = 0; gi < s_group_count; gi++) {
        if (s_groups[gi].pending_row_started_us != 0) {
            flush_group_row(&s_groups[gi]);
        }
    }
}

static void rebuild_groups_if_needed(void)
{
    uint32_t gen = config_store_get_generation();
    if (gen == s_groups_generation) {
        return;
    }

    /* Membership is about to change under any in-flight pending rows -
     * flush them first rather than silently losing partial data. */
    flush_all_groups();

    /* static: MAX_VARIABLES=32 makes this too large for sd_writer_task's
     * stack (a real stack overflow, confirmed on real hardware) - safe
     * since this function is only ever called from that single task,
     * never concurrently (same reasoning as sampling_engine.c's
     * rebuild_agg_table(), which hit the identical issue). */
    static variable_config_t vars[MAX_VARIABLES];
    size_t n = config_store_get_variables(vars, MAX_VARIABLES);

    memset(s_groups, 0, sizeof(s_groups));
    s_group_count = 0;

    for (size_t i = 0; i < n; i++) {
        if (!vars[i].enabled) {
            continue;
        }

        interval_group_t *g = NULL;
        for (size_t gi = 0; gi < s_group_count; gi++) {
            if (s_groups[gi].log_interval_ms == vars[i].log_interval_ms) {
                g = &s_groups[gi];
                break;
            }
        }
        if (!g) {
            if (s_group_count >= MAX_INTERVAL_GROUPS) {
                ESP_LOGW(TAG, "more than %d distinct log intervals in use - variable '%s' won't appear in wide-format logs",
                         MAX_INTERVAL_GROUPS, vars[i].name);
                continue;
            }
            g = &s_groups[s_group_count++];
            g->in_use = true;
            g->log_interval_ms = vars[i].log_interval_ms;
        }
        if (g->member_count >= MAX_VARIABLES) {
            continue; /* can't happen (MAX_VARIABLES total across all groups), defensive only */
        }

        group_member_t *m = &g->members[g->member_count++];
        m->variable_id = vars[i].id;
        snprintf(m->name, sizeof(m->name), "%s", vars[i].name);
        snprintf(m->unit, sizeof(m->unit), "%s", vars[i].unit);
        m->aggregate_mask = vars[i].aggregate_mask;
        m->has_value = false;
    }

    s_groups_generation = gen;
}

static void handle_wide_result(const aggregate_result_t *r)
{
    rebuild_groups_if_needed();

    interval_group_t *g = NULL;
    group_member_t *m = NULL;
    for (size_t gi = 0; gi < s_group_count && !m; gi++) {
        for (size_t mi = 0; mi < s_groups[gi].member_count; mi++) {
            if (s_groups[gi].members[mi].variable_id == r->variable_id) {
                g = &s_groups[gi];
                m = &s_groups[gi].members[mi];
                break;
            }
        }
    }
    if (!g || !m) {
        return; /* disabled/removed since the group was last (re)built */
    }

    if (m->has_value) {
        /* This variable is reporting again before every group member
         * has - a new scan has started; flush what we have first. */
        flush_group_row(g);
    }
    if (g->pending_row_started_us == 0) {
        g->pending_row_started_us = esp_timer_get_time();
    }
    m->last_value = *r;
    m->has_value = true;

    bool all_filled = true;
    for (size_t mi = 0; mi < g->member_count; mi++) {
        if (!g->members[mi].has_value) {
            all_filled = false;
            break;
        }
    }
    if (all_filled) {
        flush_group_row(g);
    }
}

static void check_group_timeouts(void)
{
    int64_t now_us = esp_timer_get_time();
    for (size_t gi = 0; gi < s_group_count; gi++) {
        interval_group_t *g = &s_groups[gi];
        if (g->pending_row_started_us == 0) {
            continue;
        }
        int64_t timeout_us = ((int64_t)g->log_interval_ms + GROUP_TIMEOUT_GRACE_MS) * 1000;
        if (now_us - g->pending_row_started_us > timeout_us) {
            flush_group_row(g);
        }
    }
}

/* ---------------------------------------------------------------------
 * Position logging (unaffected by the wide-format setting above - a
 * single fixed-schema record type, not per-variable/aggregate data).
 * ------------------------------------------------------------------- */

/* Every raw/mean/min/max/stddev column is always written regardless of
 * aggregate_mask, same convention as write_result_row()'s LONG format
 * for regular variables - aggregate_mask tells you which ones were
 * actually requested, the rest can be ignored. */
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
        fprintf(f,
                "timestamp_unix,time_synced,sample_count,aggregate_mask,"
                "lat_raw,lat_mean,lat_min,lat_max,lat_stddev,"
                "lon_raw,lon_mean,lon_min,lon_max,lon_stddev,"
                "elevation_m_raw,elevation_m_mean,elevation_m_min,elevation_m_max,elevation_m_stddev,"
                "h_precision_m_raw,h_precision_m_mean,h_precision_m_min,h_precision_m_max,h_precision_m_stddev\n");
    }

    fprintf(f, "%lld,%d,%" PRIu32 ",%u,", (long long)p->timestamp_unix, (int)p->time_is_synced, p->sample_count,
            (unsigned)p->aggregate_mask);
    fprintf(f, "%.6f,%.6f,%.6f,%.6f,%.6f,", p->latitude.raw, p->latitude.mean, p->latitude.min, p->latitude.max,
            p->latitude.stddev);
    fprintf(f, "%.6f,%.6f,%.6f,%.6f,%.6f,", p->longitude.raw, p->longitude.mean, p->longitude.min, p->longitude.max,
            p->longitude.stddev);
    fprintf(f, "%.2f,%.2f,%.2f,%.2f,%.2f,", p->elevation_m.raw, p->elevation_m.mean, p->elevation_m.min,
            p->elevation_m.max, p->elevation_m.stddev);
    fprintf(f, "%.2f,%.2f,%.2f,%.2f,%.2f\n", p->h_precision_m.raw, p->h_precision_m.mean, p->h_precision_m.min,
            p->h_precision_m.max, p->h_precision_m.stddev);

    fclose(f);
}

static void process_result(const aggregate_result_t *r)
{
    sd_settings_t sd_cfg;
    config_store_get_sd_settings(&sd_cfg);
    if (sd_cfg.log_format == SD_LOG_FORMAT_LONG) {
        write_result_row(r);
    } else {
        handle_wide_result(r);
    }
}

static void sd_writer_task(void *pvParams)
{
    (void)pvParams;
    esp_task_wdt_add(NULL);
    aggregate_result_t result;
    position_row_t position;
    for (;;) {
        esp_task_wdt_reset();
        bool flush_requested = s_flush_request && xSemaphoreTake(s_flush_request, 0) == pdTRUE;
        /* Don't block for up to 5s on an empty queue when a flush is
         * pending - the whole point is to drain and report back promptly. */
        TickType_t wait_ticks = flush_requested ? 0 : pdMS_TO_TICKS(5000);

        if (xQueueReceive(s_queue, &result, wait_ticks) == pdTRUE) {
            process_result(&result);
        }
        check_group_timeouts();
        while (xQueueReceive(s_position_queue, &position, 0) == pdTRUE) {
            write_position_row(&position);
        }

        if (flush_requested) {
            /* Catch anything still queued beyond the single item (if any)
             * already handled above, then close out any open wide-format
             * row rather than waiting for it to fill or time out. */
            while (xQueueReceive(s_queue, &result, 0) == pdTRUE) {
                process_result(&result);
            }
            while (xQueueReceive(s_position_queue, &position, 0) == pdTRUE) {
                write_position_row(&position);
            }
            flush_all_groups();
            if (s_flush_done) {
                xSemaphoreGive(s_flush_done);
            }
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

    /* The SD slot is fed from the same switched 3V3_SW rail as the
     * external I2C connector (see board_pins.h's BOARD_PIN_3V3_SW_EN
     * comment - real hardware testing found both failed without it).
     * i2c_bus_init() already needed a 10ms settle delay after enabling
     * its own switched rail before the bus would respond; sd_logger_init()
     * runs even earlier in app_main()'s boot sequence (right after the
     * bus inits, only tens of ms after board_pins_enable_3v3_sw()) with
     * no settle delay of its own, and real hardware reproduced exactly
     * the failure this rail-timing pattern predicts:
     * `sdmmc_init_ocr: send_op_cond (1) returned 0x107` (ESP_ERR_TIMEOUT -
     * the card never responds) on a boot where the card is physically
     * fine. A short wait here costs nothing at boot; if this doesn't
     * fully fix it, the card/slot/wiring itself needs checking next
     * rather than the timing. */
    vTaskDelay(pdMS_TO_TICKS(100));

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

    s_flush_request = xSemaphoreCreateBinary();
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_request || !s_flush_done) {
        esp_vfs_fat_sdcard_unmount(SD_LOGGER_MOUNT_POINT, s_card);
        return ESP_ERR_NO_MEM;
    }

    /* 6144, not the original 4096: the wide-format code path (see
     * section 11.1 in DEVELOPER_GUIDE.md) does substantially more
     * fprintf() work per row than the original long-format writer -
     * padded for margin on top of fixing the actual large-stack-local
     * bug that caused a real stack overflow on real hardware. */
    BaseType_t ok = xTaskCreate(sd_writer_task, "sd_writer", 6144, NULL, tskIDLE_PRIORITY + 2, NULL);
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

esp_err_t sd_logger_log_position(int64_t timestamp_unix, bool time_is_synced, uint32_t sample_count,
                                  uint8_t aggregate_mask, const field_aggregate_t *latitude,
                                  const field_aggregate_t *longitude, const field_aggregate_t *elevation_m,
                                  const field_aggregate_t *h_precision_m)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    position_row_t row = {
        .timestamp_unix = timestamp_unix,
        .time_is_synced = time_is_synced,
        .sample_count = sample_count,
        .aggregate_mask = aggregate_mask,
        .latitude = *latitude,
        .longitude = *longitude,
        .elevation_m = *elevation_m,
        .h_precision_m = *h_precision_m,
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

void sd_logger_flush_now(void)
{
    if (!s_ready || !s_flush_request || !s_flush_done) {
        return;
    }
    xSemaphoreGive(s_flush_request);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(SD_FLUSH_TIMEOUT_MS));
}
