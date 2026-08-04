#include "mqtt_client_bridge.h"

#include <inttypes.h>
#include <string.h>

#include "cJSON.h"
#include "cellular_transport.h"
#include "config_store.h"
#include "device_id.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_backend.h"
#include "sampling_engine.h"

static const char *TAG = "mqtt_client";

/* Sized generously, but bounded: with MAX_VARIABLES=32 and a long
 * batch_interval_ms relative to per-variable log_interval_ms, the
 * buffer can still fill up. SD logging is the durable record
 * regardless - MQTT batch overflow just drops (with a warning) the
 * oldest-pending publish, not the underlying data. Size
 * batch_interval_ms with your variable count/log intervals in mind. */
#define MAX_BATCH_ITEMS 256

static const mqttc_backend_vtable_t *s_backend;
static mqtt_settings_t s_current_settings;
static bool s_backend_connected;
static int64_t s_next_reconnect_attempt_us; /* non-batch mode only - see mqtt_publish_task() */
#define RECONNECT_BACKOFF_US (15 * 1000 * 1000) /* 15s - avoid hammering a broker/modem that's still failing */

static aggregate_result_t s_batch_buffer[MAX_BATCH_ITEMS];
static size_t s_batch_count;
static int64_t s_next_batch_transmit_us;

static SemaphoreHandle_t s_position_mutex;
static gnss_fix_t s_pending_position;
static bool s_pending_position_valid;

static bool mqtt_settings_equal(const mqtt_settings_t *a, const mqtt_settings_t *b)
{
    return a->enabled == b->enabled && a->port == b->port && a->use_tls == b->use_tls &&
           a->tls_allow_insecure == b->tls_allow_insecure && a->batch_enabled == b->batch_enabled &&
           a->batch_interval_ms == b->batch_interval_ms && a->flat_telemetry == b->flat_telemetry &&
           strcmp(a->host, b->host) == 0 && strcmp(a->client_id, b->client_id) == 0 &&
           strcmp(a->username, b->username) == 0 && strcmp(a->password, b->password) == 0 &&
           strcmp(a->topic_prefix, b->topic_prefix) == 0;
}

static void apply_settings_if_changed(const mqtt_settings_t *cur)
{
    if (!s_backend || mqtt_settings_equal(cur, &s_current_settings)) {
        return;
    }

    if (s_backend_connected) {
        s_backend->disconnect();
        s_backend_connected = false;
    }
    s_backend->deinit();
    s_current_settings = *cur;

    /* In batch mode, the connection is opened/closed per transmission
     * window (see mqtt_publish_task) rather than held open here.
     * Non-batch mode's actual connect attempt (and retries) happen in
     * mqtt_publish_task's main loop, not here - a single attempt at
     * settings-change time isn't enough on its own (e.g. over
     * cellular, this can run before network registration completes;
     * confirmed on real hardware), and there was previously no retry
     * at all if that one attempt failed, since s_current_settings was
     * already updated to match cur either way. Just prime the retry
     * loop to attempt promptly. */
    if (cur->enabled && !cur->batch_enabled && strlen(cur->host) > 0) {
        s_next_reconnect_attempt_us = 0;
    }
}

/* MQTT topic levels can't contain '/', '+', or '#' - sanitize a
 * student-chosen variable name before using it as a topic segment. */
static void sanitize_topic_segment(const char *in, char *out, size_t out_size)
{
    size_t i = 0;
    for (; in[i] && i < out_size - 1; i++) {
        char c = in[i];
        out[i] = (c == '/' || c == '+' || c == '#') ? '_' : c;
    }
    out[i] = '\0';
}

static void build_json_payload(const aggregate_result_t *r, char *out, size_t out_size)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ts", (double)r->timestamp_unix);
    cJSON_AddBoolToObject(o, "time_synced", r->time_is_synced);
    cJSON_AddNumberToObject(o, "n", r->sample_count);
    if (r->aggregate_mask & AGG_RAW) {
        cJSON_AddNumberToObject(o, "raw", r->raw);
    }
    if (r->aggregate_mask & AGG_MEAN) {
        cJSON_AddNumberToObject(o, "mean", r->mean);
    }
    if (r->aggregate_mask & AGG_MIN) {
        cJSON_AddNumberToObject(o, "min", r->min);
    }
    if (r->aggregate_mask & AGG_MAX) {
        cJSON_AddNumberToObject(o, "max", r->max);
    }
    if (r->aggregate_mask & AGG_STDDEV) {
        cJSON_AddNumberToObject(o, "stddev", r->stddev);
    }

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        snprintf(out, out_size, "%s", s);
        cJSON_free(s);
    } else {
        out[0] = '\0';
    }
}

/* Flat "<name>_<aggregate>" key shape platforms like ThingsBoard/AWS IoT
 * expect on their single fixed device telemetry topic - as opposed to
 * build_json_payload()'s generic {"mean":...,"stddev":...} shape, which
 * needs a distinct topic per variable (the topic itself carries the
 * variable name) to stay unambiguous. */
static void build_flat_json_payload(const aggregate_result_t *r, char *out, size_t out_size)
{
    cJSON *o = cJSON_CreateObject();
    char key[40];
    if (r->aggregate_mask & AGG_RAW) {
        snprintf(key, sizeof(key), "%s_raw", r->name);
        cJSON_AddNumberToObject(o, key, r->raw);
    }
    if (r->aggregate_mask & AGG_MEAN) {
        snprintf(key, sizeof(key), "%s_mean", r->name);
        cJSON_AddNumberToObject(o, key, r->mean);
    }
    if (r->aggregate_mask & AGG_MIN) {
        snprintf(key, sizeof(key), "%s_min", r->name);
        cJSON_AddNumberToObject(o, key, r->min);
    }
    if (r->aggregate_mask & AGG_MAX) {
        snprintf(key, sizeof(key), "%s_max", r->name);
        cJSON_AddNumberToObject(o, key, r->max);
    }
    if (r->aggregate_mask & AGG_STDDEV) {
        snprintf(key, sizeof(key), "%s_stddev", r->name);
        cJSON_AddNumberToObject(o, key, r->stddev);
    }

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        snprintf(out, out_size, "%s", s);
        cJSON_free(s);
    } else {
        out[0] = '\0';
    }
}

static void publish_result(const aggregate_result_t *r)
{
    char topic[128];
    char payload[256];

    if (s_current_settings.flat_telemetry) {
        snprintf(topic, sizeof(topic), "%s", s_current_settings.topic_prefix);
        build_flat_json_payload(r, payload, sizeof(payload));
    } else {
        char name_safe[32];
        sanitize_topic_segment(r->name, name_safe, sizeof(name_safe));
        snprintf(topic, sizeof(topic), "%s/%s/%s", s_current_settings.topic_prefix, device_id_get(), name_safe);
        build_json_payload(r, payload, sizeof(payload));
    }

    esp_err_t err = s_backend->publish(topic, payload, 1, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish failed for '%s': %s", r->name, esp_err_to_name(err));
    }
}

static void build_position_json_payload(const gnss_fix_t *fix, char *out, size_t out_size)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ts", (double)fix->timestamp_unix);
    cJSON_AddBoolToObject(o, "time_synced", fix->timestamp_unix > 0);
    cJSON_AddNumberToObject(o, "lat", fix->latitude);
    cJSON_AddNumberToObject(o, "lon", fix->longitude);
    cJSON_AddNumberToObject(o, "alt", fix->altitude_m);

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        snprintf(out, out_size, "%s", s);
        cJSON_free(s);
    } else {
        out[0] = '\0';
    }
}

static void publish_position_now(const gnss_fix_t *fix)
{
    char topic[96];
    if (s_current_settings.flat_telemetry) {
        snprintf(topic, sizeof(topic), "%s", s_current_settings.topic_prefix);
    } else {
        snprintf(topic, sizeof(topic), "%s/%s/position", s_current_settings.topic_prefix, device_id_get());
    }

    char payload[192];
    build_position_json_payload(fix, payload, sizeof(payload));

    esp_err_t err = s_backend->publish(topic, payload, 1, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "position publish failed: %s", esp_err_to_name(err));
    }
}

/* Publishes the pending position (if any) - only called while the
 * caller has already confirmed the backend is connected. */
static void maybe_publish_pending_position(void)
{
    if (!s_backend || !s_backend_connected || !s_position_mutex) {
        return;
    }

    xSemaphoreTake(s_position_mutex, portMAX_DELAY);
    bool have = s_pending_position_valid;
    gnss_fix_t fix = s_pending_position;
    s_pending_position_valid = false;
    xSemaphoreGive(s_position_mutex);

    if (have) {
        publish_position_now(&fix);
    }
}

static void mqtt_publish_task(void *pvParams)
{
    QueueHandle_t queue = (QueueHandle_t)pvParams;
    uint32_t last_generation = UINT32_MAX;
    esp_task_wdt_add(NULL);

    for (;;) {
        esp_task_wdt_reset();
        aggregate_result_t result;
        bool got_sample = xQueueReceive(queue, &result, pdMS_TO_TICKS(500)) == pdTRUE;

        uint32_t generation = config_store_get_generation();
        if (generation != last_generation) {
            mqtt_settings_t cur;
            config_store_get_mqtt_settings(&cur);
            bool batch_was_enabled = s_current_settings.batch_enabled;
            uint32_t old_interval_ms = s_current_settings.batch_interval_ms;
            apply_settings_if_changed(&cur);
            if (cur.batch_enabled && (!batch_was_enabled || cur.batch_interval_ms != old_interval_ms)) {
                s_next_batch_transmit_us = esp_timer_get_time(); /* send the first batch promptly */
            }
            last_generation = generation;
        }

        if (s_backend) {
            s_backend_connected = s_backend->is_connected();
        }

        if (!s_current_settings.batch_enabled) {
            int64_t now_us = esp_timer_get_time();
            if (s_backend && s_current_settings.enabled && !s_backend_connected &&
                strlen(s_current_settings.host) > 0 && now_us >= s_next_reconnect_attempt_us) {
                ESP_LOGI(TAG, "connecting to %s:%u (tls=%d, cellular registered=%d pdp_active=%d)",
                         s_current_settings.host, s_current_settings.port, (int)s_current_settings.use_tls,
                         (int)cellular_transport_is_registered(), (int)cellular_transport_is_pdp_active());
                if (s_backend->init(&s_current_settings) == ESP_OK && s_backend->connect() == ESP_OK) {
                    s_backend_connected = s_backend->is_connected();
                }
                if (!s_backend_connected) {
                    ESP_LOGW(TAG, "failed to start MQTT backend, retrying in %d s", RECONNECT_BACKOFF_US / 1000000);
                    s_next_reconnect_attempt_us = now_us + RECONNECT_BACKOFF_US;
                }
            }

            if (got_sample && s_backend && s_backend_connected) {
                publish_result(&result);
            }
            maybe_publish_pending_position();
            continue;
        }

        /* ---- batch mode ---- */
        if (got_sample) {
            if (s_batch_count < MAX_BATCH_ITEMS) {
                s_batch_buffer[s_batch_count++] = result;
            } else {
                ESP_LOGW(TAG, "batch buffer full (%d), dropping sample for '%s'", MAX_BATCH_ITEMS, result.name);
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (s_backend && s_current_settings.enabled && now_us >= s_next_batch_transmit_us) {
            if (!s_backend_connected) {
                if (s_backend->init(&s_current_settings) == ESP_OK && s_backend->connect() == ESP_OK) {
                    s_backend_connected = s_backend->is_connected();
                }
            }

            if (s_backend_connected) {
                ESP_LOGI(TAG, "batch transmit window: sending %u buffered sample(s)", (unsigned)s_batch_count);
                for (size_t i = 0; i < s_batch_count; i++) {
                    publish_result(&s_batch_buffer[i]);
                }
                s_batch_count = 0;
                maybe_publish_pending_position();

                s_backend->disconnect();
                s_backend_connected = false;
                s_next_batch_transmit_us = now_us + (int64_t)s_current_settings.batch_interval_ms * 1000;
                ESP_LOGI(TAG, "batch transmit complete, next in %" PRIu32 " ms", s_current_settings.batch_interval_ms);
            } else {
                ESP_LOGW(TAG, "batch transmit: failed to connect, will retry shortly");
                /* Retried on the next loop iteration - naturally
                 * rate-limited by the 500ms queue-receive timeout, so
                 * no separate backoff needed. */
            }
        }
    }
}

esp_err_t mqttc_init(void)
{
    net_settings_t net;
    config_store_get_net_settings(&net);

    if (net.transport == TRANSPORT_WIFI) {
        s_backend = backend_esp_mqtt_get_vtable();
    } else if (net.transport == TRANSPORT_CELLULAR) {
        /* backend_walter_mqtt.cpp is unverified against the real
         * walter-modem SDK (see cellular_transport.h) - selected here
         * so the architecture is wired end-to-end, but treat cellular
         * MQTT as untested until that's confirmed. */
        s_backend = backend_walter_mqtt_get_vtable();
    } else {
        s_backend = NULL; /* no transport configured yet */
    }

    memset(&s_current_settings, 0, sizeof(s_current_settings));
    s_backend_connected = false;
    s_batch_count = 0;
    s_next_batch_transmit_us = 0;
    s_next_reconnect_attempt_us = 0;

    s_position_mutex = xSemaphoreCreateMutex();
    if (!s_position_mutex) {
        return ESP_ERR_NO_MEM;
    }

    QueueHandle_t queue = xQueueCreate(16, sizeof(aggregate_result_t));
    if (!queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sampling_engine_add_result_sink(queue);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(mqtt_publish_task, "mqtt_pub", 4096, queue, tskIDLE_PRIORITY + 2, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool mqttc_is_ready(void)
{
    return s_backend != NULL && s_backend_connected;
}

esp_err_t mqttc_publish_position(const gnss_fix_t *fix)
{
    if (!fix) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_position_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_position_mutex, portMAX_DELAY);
    s_pending_position = *fix;
    s_pending_position_valid = true;
    xSemaphoreGive(s_position_mutex);
    return ESP_OK; /* actual publish happens from mqtt_publish_task */
}
