#include "mqtt_client_bridge.h"

#include <string.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_backend.h"
#include "sampling_engine.h"

static const char *TAG = "mqtt_client";

static const mqttc_backend_vtable_t *s_backend;
static mqtt_settings_t s_current_settings;
static bool s_backend_connected;

static bool mqtt_settings_equal(const mqtt_settings_t *a, const mqtt_settings_t *b)
{
    return a->enabled == b->enabled && a->port == b->port && a->use_tls == b->use_tls &&
           a->tls_allow_insecure == b->tls_allow_insecure && strcmp(a->host, b->host) == 0 &&
           strcmp(a->client_id, b->client_id) == 0 && strcmp(a->username, b->username) == 0 &&
           strcmp(a->password, b->password) == 0 && strcmp(a->topic_prefix, b->topic_prefix) == 0;
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

    if (cur->enabled && strlen(cur->host) > 0) {
        if (s_backend->init(cur) == ESP_OK && s_backend->connect() == ESP_OK) {
            ESP_LOGI(TAG, "connecting to %s:%u (tls=%d)", cur->host, cur->port, (int)cur->use_tls);
        } else {
            ESP_LOGE(TAG, "failed to start MQTT backend");
        }
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

static void mqtt_publish_task(void *pvParams)
{
    QueueHandle_t queue = (QueueHandle_t)pvParams;
    uint32_t last_generation = UINT32_MAX;

    for (;;) {
        aggregate_result_t result;
        bool got_sample = xQueueReceive(queue, &result, pdMS_TO_TICKS(500)) == pdTRUE;

        uint32_t generation = config_store_get_generation();
        if (generation != last_generation) {
            mqtt_settings_t cur;
            config_store_get_mqtt_settings(&cur);
            apply_settings_if_changed(&cur);
            last_generation = generation;
        }

        if (s_backend) {
            s_backend_connected = s_backend->is_connected();
        }

        if (got_sample && s_backend && s_backend_connected) {
            char name_safe[32];
            sanitize_topic_segment(result.name, name_safe, sizeof(name_safe));

            char topic[96];
            snprintf(topic, sizeof(topic), "%s/%s", s_current_settings.topic_prefix, name_safe);

            char payload[256];
            build_json_payload(&result, payload, sizeof(payload));

            esp_err_t err = s_backend->publish(topic, payload, 1, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "publish failed for '%s': %s", result.name, esp_err_to_name(err));
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
