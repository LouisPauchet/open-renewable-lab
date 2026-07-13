#include <time.h>

#include "config_store.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "net_manager.h"
#include "sd_logger.h"
#include "web_portal_internal.h"

/* Mirrors sampling_engine's TIME_SYNC_EPOCH_THRESHOLD - system time
 * before this (~2023-11-14) means SNTP/NITZ hasn't set the clock yet. */
#define TIME_SYNC_EPOCH_THRESHOLD 1700000000

static const char *transport_name(net_transport_t t)
{
    switch (t) {
        case TRANSPORT_WIFI:
            return "wifi";
        case TRANSPORT_CELLULAR:
            return "cellular";
        default:
            return "unconfigured";
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    device_config_t cfg;
    config_store_get_snapshot(&cfg);

    time_t now = time(NULL);
    bool time_synced = now >= TIME_SYNC_EPOCH_THRESHOLD;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(o, "free_heap_bytes", esp_get_free_heap_size());

    cJSON_AddBoolToObject(o, "ap_active", net_manager_ap_is_active());
    cJSON_AddNumberToObject(o, "ap_client_count", net_manager_ap_client_count());

    cJSON_AddStringToObject(o, "transport", transport_name(cfg.net.transport));
    /* WiFi-STA/cellular connection state is wired up once net_manager
     * grows STA + cellular handling in a later build stage. */
    cJSON_AddBoolToObject(o, "data_connection_up", false);

    cJSON_AddBoolToObject(o, "time_synced", time_synced);
    cJSON_AddNumberToObject(o, "time_unix", (double)now);

    cJSON_AddBoolToObject(o, "sd_ready", sd_logger_is_ready());
    uint64_t sd_total = 0, sd_free = 0;
    if (sd_logger_get_space(&sd_total, &sd_free) == ESP_OK) {
        cJSON_AddNumberToObject(o, "sd_total_bytes", (double)sd_total);
        cJSON_AddNumberToObject(o, "sd_free_bytes", (double)sd_free);
    }
    cJSON_AddNumberToObject(o, "sd_drop_count", sd_logger_get_drop_count());

    /* MQTT connection state is wired up once mqtt_client lands. */
    cJSON_AddBoolToObject(o, "mqtt_enabled", cfg.mqtt.enabled);
    cJSON_AddBoolToObject(o, "mqtt_connected", false);

    cJSON_AddNumberToObject(o, "variable_count", cfg.variable_count);
    cJSON_AddNumberToObject(o, "config_generation", cfg.generation);

    return wp_send_json(req, o);
}

void api_status_register_routes(httpd_handle_t server)
{
    httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(server, &status_uri);
}
