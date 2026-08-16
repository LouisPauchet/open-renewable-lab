#include "sampling_engine.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aggregator.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "time_sync.h"

static const char *TAG = "sampling_engine";

#define SCHEDULER_TICK_MS 200
#define MAX_RESULT_SINKS 4

typedef struct {
    uint16_t variable_id;
    double value;
    int64_t sampled_at_us; /* esp_timer_get_time() at sample time */
} sample_msg_t;

typedef struct {
    uint16_t variable_id;
    char name[32];
    uint8_t aggregate_mask;
    uint32_t sample_interval_ms;
    uint32_t log_interval_ms;
    bool allow_skip_during_sleep;
    int64_t next_log_due_us;
    aggregator_t agg;
    bool in_use;
} agg_entry_t;

typedef struct {
    uint16_t variable_id;
    int64_t next_sample_due_us;
    bool in_use;
} sched_entry_t;

static sensor_bus_read_fn_t s_bus_drivers[2]; /* indexed by bus_type_t */

static QueueHandle_t s_sample_queue;
static QueueHandle_t s_sinks[MAX_RESULT_SINKS];
static size_t s_sink_count;

static agg_entry_t s_agg_table[MAX_VARIABLES];
static size_t s_agg_count;
static uint32_t s_agg_last_generation = UINT32_MAX;
/* Guards s_agg_table/s_agg_count/s_agg_last_generation: written by
 * aggregation_task, also read by the sleep-state/status queries below
 * which may be called from power_manager's own task. */
static SemaphoreHandle_t s_agg_mutex;

/* Populated by sampling_engine_restore_sleep_state() before init(),
 * consumed by the first rebuild_agg_table() call and then discarded -
 * see that function. */
static sampling_sleep_entry_t s_restore_table[MAX_VARIABLES];
static size_t s_restore_count;

static void get_wall_clock(int64_t *out_unix_s, bool *out_synced)
{
    time_t now = time(NULL);
    *out_unix_s = (int64_t)now;
    *out_synced = now >= TIME_SYNC_EPOCH_THRESHOLD;
}

/* ---------------------------------------------------------------------
 * bus scheduler task - one instance per bus_type_t
 *
 * Deliberately NOT registered with esp_task_wdt: a single sensor read
 * (especially SDI-12's aM!-then-wait-then-aD0! sequence) can
 * legitimately block for many seconds depending on what the sensor
 * itself advertises, which doesn't fit a fixed watchdog timeout
 * without risking false trips. aggregation_task/sd_writer_task/
 * mqtt_publish_task have much more predictable per-iteration timing
 * and are registered instead.
 * ------------------------------------------------------------------- */

static void bus_scheduler_task(void *pvParams)
{
    bus_type_t bus_type = (bus_type_t)(intptr_t)pvParams;
    const char *bus_name = (bus_type == BUS_TYPE_SDI12) ? "sdi12" : "i2c";

    /* MAX_VARIABLES=32 makes these full-struct arrays too large to keep
     * as locals on this task's stack (two concurrent instances of this
     * task run - one per bus type - so they can't be `static` without
     * the two racing on shared storage); heap-allocate once instead. */
    sched_entry_t *sched = calloc(MAX_VARIABLES, sizeof(sched_entry_t));
    variable_config_t *vars = calloc(MAX_VARIABLES, sizeof(variable_config_t));
    if (!sched || !vars) {
        ESP_LOGE(TAG, "%s scheduler: out of memory allocating schedule tables", bus_name);
        vTaskDelete(NULL);
        return;
    }
    uint32_t last_generation = UINT32_MAX;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SCHEDULER_TICK_MS));

        uint32_t generation = config_store_get_generation();
        size_t count = config_store_get_variables(vars, MAX_VARIABLES);
        int64_t now_us = esp_timer_get_time();

        if (generation != last_generation) {
            /* Config changed: rebuild the schedule from scratch. Newly
             * added/changed variables sample immediately. */
            memset(sched, 0, MAX_VARIABLES * sizeof(sched_entry_t));
            size_t n = 0;
            for (size_t i = 0; i < count && n < MAX_VARIABLES; i++) {
                if (vars[i].bus_type == bus_type) {
                    sched[n].variable_id = vars[i].id;
                    sched[n].next_sample_due_us = now_us;
                    sched[n].in_use = true;
                    n++;
                }
            }
            last_generation = generation;
        }

        sensor_bus_read_fn_t read_fn = s_bus_drivers[bus_type];

        for (size_t i = 0; i < count; i++) {
            const variable_config_t *var = &vars[i];
            if (var->bus_type != bus_type || !var->enabled) {
                continue;
            }

            sched_entry_t *entry = NULL;
            for (size_t j = 0; j < MAX_VARIABLES; j++) {
                if (sched[j].in_use && sched[j].variable_id == var->id) {
                    entry = &sched[j];
                    break;
                }
            }
            if (!entry) {
                continue; /* added after the last generation rebuild; will show up next tick */
            }

            if (now_us < entry->next_sample_due_us) {
                continue;
            }
            entry->next_sample_due_us = now_us + (int64_t)var->sample_interval_ms * 1000;

            if (!read_fn) {
                continue; /* no driver registered yet for this bus type */
            }

            double value = 0.0;
            esp_err_t err = read_fn(var, &value);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "%s read failed for variable '%s' (id=%u): %s", bus_name, var->name,
                         (unsigned)var->id, esp_err_to_name(err));
                continue;
            }
            value = var->calibration_a * value + var->calibration_b;

            sample_msg_t msg = {
                .variable_id = var->id,
                .value = value,
                .sampled_at_us = esp_timer_get_time(),
            };
            if (xQueueSend(s_sample_queue, &msg, 0) != pdTRUE) {
                ESP_LOGW(TAG, "sample queue full, dropping sample for variable id=%u", (unsigned)var->id);
            }
        }
    }
}

/* ---------------------------------------------------------------------
 * aggregation task
 * ------------------------------------------------------------------- */

static agg_entry_t *find_agg_entry(uint16_t variable_id)
{
    for (size_t i = 0; i < MAX_VARIABLES; i++) {
        if (s_agg_table[i].in_use && s_agg_table[i].variable_id == variable_id) {
            return &s_agg_table[i];
        }
    }
    return NULL;
}

static const sampling_sleep_entry_t *find_restore_entry(uint16_t variable_id)
{
    for (size_t i = 0; i < s_restore_count; i++) {
        if (s_restore_table[i].variable_id == variable_id) {
            return &s_restore_table[i];
        }
    }
    return NULL;
}

static void rebuild_agg_table(void)
{
    /* MAX_VARIABLES=32 full-struct arrays are too large for aggregation_task's
     * stack; `static` is safe here since rebuild_agg_table only ever runs
     * serially from that single task (no concurrent callers). */
    static variable_config_t vars[MAX_VARIABLES];
    size_t count = config_store_get_variables(vars, MAX_VARIABLES);
    int64_t now_us = esp_timer_get_time();

    static agg_entry_t new_table[MAX_VARIABLES];
    memset(new_table, 0, sizeof(new_table));
    size_t n = 0;

    for (size_t i = 0; i < count && n < MAX_VARIABLES; i++) {
        const variable_config_t *var = &vars[i];
        if (!var->enabled) {
            continue;
        }

        agg_entry_t *existing = find_agg_entry(var->id);

        new_table[n].variable_id = var->id;
        snprintf(new_table[n].name, sizeof(new_table[n].name), "%s", var->name);
        new_table[n].aggregate_mask = var->aggregate_mask;
        new_table[n].sample_interval_ms = var->sample_interval_ms;
        new_table[n].log_interval_ms = var->log_interval_ms;
        new_table[n].allow_skip_during_sleep = var->allow_skip_during_sleep;
        new_table[n].in_use = true;

        if (existing) {
            /* preserve in-flight accumulation across unrelated config edits */
            new_table[n].agg = existing->agg;
            new_table[n].next_log_due_us = existing->next_log_due_us;
        } else {
            const sampling_sleep_entry_t *restored = find_restore_entry(var->id);
            if (restored) {
                /* Convert the wall-clock deadline captured before sleep back
                 * into this boot's esp_timer_get_time() epoch (which reset to
                 * 0 on wake). If time wasn't synced when captured, there's no
                 * usable deadline to restore - fall back to a fresh interval,
                 * same as a brand new variable. */
                new_table[n].agg = restored->agg;
                new_table[n].next_log_due_us = restored->time_is_synced
                    ? now_us + (restored->next_log_due_unix - (int64_t)time(NULL)) * 1000000LL
                    : now_us + (int64_t)var->log_interval_ms * 1000;
            } else {
                aggregator_reset(&new_table[n].agg);
                new_table[n].next_log_due_us = now_us + (int64_t)var->log_interval_ms * 1000;
            }
        }
        n++;
    }

    memcpy(s_agg_table, new_table, sizeof(s_agg_table));
    s_agg_count = n;
    /* Restored state is only meant to seed the very first rebuild after a
     * sleep-wake boot; later rebuilds (triggered by real config edits) must
     * use the normal find_agg_entry() preserve-in-place path above. */
    s_restore_count = 0;
}

static void publish_result(const agg_entry_t *entry)
{
    aggregate_result_t result = { 0 };
    result.variable_id = entry->variable_id;
    snprintf(result.name, sizeof(result.name), "%s", entry->name);
    result.aggregate_mask = entry->aggregate_mask;
    result.sample_count = entry->agg.count;
    result.raw = entry->agg.last_raw;
    result.mean = entry->agg.mean;
    result.min = entry->agg.min;
    result.max = entry->agg.max;
    result.stddev = aggregator_stddev(&entry->agg);
    get_wall_clock(&result.timestamp_unix, &result.time_is_synced);

    for (size_t i = 0; i < s_sink_count; i++) {
        if (xQueueSend(s_sinks[i], &result, 0) != pdTRUE) {
            ESP_LOGW(TAG, "result sink %u full, dropping aggregate for variable '%s'", (unsigned)i, entry->name);
        }
    }
}

static void aggregation_task(void *pvParams)
{
    (void)pvParams;
    esp_task_wdt_add(NULL);

    for (;;) {
        esp_task_wdt_reset();
        sample_msg_t msg;
        bool got_msg = xQueueReceive(s_sample_queue, &msg, pdMS_TO_TICKS(SCHEDULER_TICK_MS)) == pdTRUE;

        xSemaphoreTake(s_agg_mutex, portMAX_DELAY);

        uint32_t generation = config_store_get_generation();
        if (generation != s_agg_last_generation) {
            rebuild_agg_table();
            s_agg_last_generation = generation;
        }

        if (got_msg) {
            agg_entry_t *entry = find_agg_entry(msg.variable_id);
            if (entry) {
                aggregator_add_sample(&entry->agg, msg.value);
            }
        }

        int64_t now_us = esp_timer_get_time();
        for (size_t i = 0; i < MAX_VARIABLES; i++) {
            agg_entry_t *entry = &s_agg_table[i];
            if (!entry->in_use || now_us < entry->next_log_due_us) {
                continue;
            }
            if (entry->agg.has_data) {
                publish_result(entry);
            }
            aggregator_reset(&entry->agg);
            entry->next_log_due_us = now_us + (int64_t)entry->log_interval_ms * 1000;
        }

        xSemaphoreGive(s_agg_mutex);
    }
}

/* ---------------------------------------------------------------------
 * public API
 * ------------------------------------------------------------------- */

esp_err_t sampling_engine_init(void)
{
    s_sample_queue = xQueueCreate(32, sizeof(sample_msg_t));
    if (!s_sample_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_agg_mutex = xSemaphoreCreateMutex();
    if (!s_agg_mutex) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_agg_table, 0, sizeof(s_agg_table));
    s_agg_count = 0;
    s_agg_last_generation = UINT32_MAX;

    BaseType_t ok;
    ok = xTaskCreate(bus_scheduler_task, "sdi12_sched", 4096, (void *)(intptr_t)BUS_TYPE_SDI12, tskIDLE_PRIORITY + 3,
                      NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ok = xTaskCreate(bus_scheduler_task, "i2c_sched", 4096, (void *)(intptr_t)BUS_TYPE_I2C, tskIDLE_PRIORITY + 3,
                      NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ok = xTaskCreate(aggregation_task, "aggregation", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "sampling engine started");
    return ESP_OK;
}

void sampling_engine_register_bus_driver(bus_type_t bus_type, sensor_bus_read_fn_t read_fn)
{
    if (bus_type != BUS_TYPE_SDI12 && bus_type != BUS_TYPE_I2C) {
        ESP_LOGE(TAG, "invalid bus_type %d", (int)bus_type);
        return;
    }
    s_bus_drivers[bus_type] = read_fn;
}

esp_err_t sampling_engine_add_result_sink(QueueHandle_t queue)
{
    if (!queue) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sink_count >= MAX_RESULT_SINKS) {
        return ESP_ERR_NO_MEM;
    }
    s_sinks[s_sink_count++] = queue;
    return ESP_OK;
}

esp_err_t sampling_engine_read_once(uint16_t variable_id, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    variable_config_t var;
    if (!config_store_get_variable(variable_id, &var)) {
        return ESP_ERR_NOT_FOUND;
    }

    sensor_bus_read_fn_t read_fn = s_bus_drivers[var.bus_type];
    if (!read_fn) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = read_fn(&var, out_value);
    if (err == ESP_OK) {
        *out_value = var.calibration_a * (*out_value) + var.calibration_b;
    }
    return err;
}

/* ---------------------------------------------------------------------
 * deep sleep support (power_manager)
 * ------------------------------------------------------------------- */

size_t sampling_engine_get_sleep_state(sampling_sleep_entry_t *out, size_t max_count)
{
    if (!out || max_count == 0) {
        return 0;
    }

    int64_t now_unix;
    bool synced;
    get_wall_clock(&now_unix, &synced);
    int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(s_agg_mutex, portMAX_DELAY);
    size_t n = 0;
    for (size_t i = 0; i < MAX_VARIABLES && n < max_count; i++) {
        const agg_entry_t *entry = &s_agg_table[i];
        if (!entry->in_use) {
            continue;
        }
        out[n].variable_id = entry->variable_id;
        out[n].agg = entry->agg;
        out[n].time_is_synced = synced;
        out[n].next_log_due_unix = synced ? now_unix + (entry->next_log_due_us - now_us) / 1000000LL : 0;
        n++;
    }
    xSemaphoreGive(s_agg_mutex);
    return n;
}

void sampling_engine_restore_sleep_state(const sampling_sleep_entry_t *in, size_t count)
{
    if (!in) {
        return;
    }
    if (count > MAX_VARIABLES) {
        count = MAX_VARIABLES;
    }
    memcpy(s_restore_table, in, count * sizeof(sampling_sleep_entry_t));
    s_restore_count = count;
}

sampling_engine_sleep_status_t sampling_engine_get_sleep_status(void)
{
    sampling_engine_sleep_status_t status = { 0 };

    int64_t now_unix;
    bool synced;
    get_wall_clock(&now_unix, &synced);
    int64_t now_us = esp_timer_get_time();
    bool have_due = false;

    xSemaphoreTake(s_agg_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_VARIABLES; i++) {
        const agg_entry_t *entry = &s_agg_table[i];
        if (!entry->in_use) {
            continue;
        }
        if (entry->sample_interval_ms < entry->log_interval_ms && !entry->allow_skip_during_sleep) {
            status.blocked = true;
            snprintf(status.blocked_reason, sizeof(status.blocked_reason), "%s", entry->name);
            break;
        }
        status.has_schedulable = true;
        if (synced) {
            int64_t due_unix = now_unix + (entry->next_log_due_us - now_us) / 1000000LL;
            if (!have_due || due_unix < status.next_due_unix) {
                status.next_due_unix = due_unix;
                have_due = true;
            }
        }
    }
    xSemaphoreGive(s_agg_mutex);

    status.next_due_is_synced = have_due;
    return status;
}
