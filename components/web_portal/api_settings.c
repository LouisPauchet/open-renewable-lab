#include <string.h>

#include "config_store.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "web_portal_internal.h"

static void json_str(const cJSON *obj, const char *key, char *dst, size_t dst_size, const char *def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    snprintf(dst, dst_size, "%s", (j && cJSON_IsString(j)) ? j->valuestring : def);
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (j && cJSON_IsNumber(j)) ? (int)j->valuedouble : def;
}

static bool json_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (j && cJSON_IsBool(j)) ? cJSON_IsTrue(j) : def;
}

/* ---- MQTT settings ---- */

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    mqtt_settings_t m;
    config_store_get_mqtt_settings(&m);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", m.enabled);
    cJSON_AddStringToObject(o, "host", m.host);
    cJSON_AddNumberToObject(o, "port", m.port);
    cJSON_AddBoolToObject(o, "use_tls", m.use_tls);
    cJSON_AddBoolToObject(o, "tls_allow_insecure", m.tls_allow_insecure);
    cJSON_AddStringToObject(o, "client_id", m.client_id);
    cJSON_AddStringToObject(o, "username", m.username);
    cJSON_AddBoolToObject(o, "password_set", strlen(m.password) > 0); /* password itself is write-only */
    cJSON_AddStringToObject(o, "topic_prefix", m.topic_prefix);
    cJSON_AddBoolToObject(o, "flat_telemetry", m.flat_telemetry);
    cJSON_AddBoolToObject(o, "batch_enabled", m.batch_enabled);
    cJSON_AddNumberToObject(o, "batch_interval_ms", m.batch_interval_ms);
    return wp_send_json(req, o);
}

static esp_err_t mqtt_put_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    mqtt_settings_t current;
    config_store_get_mqtt_settings(&current);

    mqtt_settings_t m = current;
    m.enabled = json_bool(body, "enabled", current.enabled);
    json_str(body, "host", m.host, sizeof(m.host), current.host);
    m.port = (uint16_t)json_int(body, "port", current.port);
    m.use_tls = json_bool(body, "use_tls", current.use_tls);
    m.tls_allow_insecure = json_bool(body, "tls_allow_insecure", current.tls_allow_insecure);
    json_str(body, "client_id", m.client_id, sizeof(m.client_id), current.client_id);
    json_str(body, "username", m.username, sizeof(m.username), current.username);
    json_str(body, "topic_prefix", m.topic_prefix, sizeof(m.topic_prefix), current.topic_prefix);
    m.flat_telemetry = json_bool(body, "flat_telemetry", current.flat_telemetry);
    m.batch_enabled = json_bool(body, "batch_enabled", current.batch_enabled);
    m.batch_interval_ms = (uint32_t)json_int(body, "batch_interval_ms", (int)current.batch_interval_ms);

    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (pw && cJSON_IsString(pw) && strlen(pw->valuestring) > 0) {
        snprintf(m.password, sizeof(m.password), "%s", pw->valuestring);
    }
    cJSON_Delete(body);

    if (m.enabled && strlen(m.host) == 0) {
        return wp_send_error(req, "400 Bad Request", "host is required when MQTT is enabled");
    }
    if (m.enabled && m.port == 0) {
        return wp_send_error(req, "400 Bad Request", "invalid port");
    }
    if (m.batch_enabled && m.batch_interval_ms == 0) {
        return wp_send_error(req, "400 Bad Request", "batch_interval_ms must be > 0 when batching is enabled");
    }

    config_store_set_mqtt_settings(&m);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

/* ---- Network settings ---- */

static net_transport_t parse_transport(const cJSON *body, net_transport_t def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(body, "transport");
    if (!j || !cJSON_IsString(j)) {
        return def;
    }
    if (strcmp(j->valuestring, "wifi") == 0) {
        return TRANSPORT_WIFI;
    }
    if (strcmp(j->valuestring, "cellular") == 0) {
        return TRANSPORT_CELLULAR;
    }
    return TRANSPORT_UNCONFIGURED;
}

static const char *transport_str(net_transport_t t)
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

static esp_err_t network_get_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    net_settings_t n;
    config_store_get_net_settings(&n);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "transport", transport_str(n.transport));
    cJSON_AddStringToObject(o, "wifi_sta_ssid", n.wifi_sta_ssid);
    cJSON_AddBoolToObject(o, "wifi_sta_password_set", strlen(n.wifi_sta_password) > 0);
    cJSON_AddStringToObject(o, "cellular_apn", n.cellular_apn);
    cJSON_AddBoolToObject(o, "cellular_pin_set", strlen(n.cellular_pin) > 0);
    return wp_send_json(req, o);
}

static esp_err_t network_put_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    net_settings_t current;
    config_store_get_net_settings(&current);

    net_settings_t n = current;
    n.transport = parse_transport(body, current.transport);
    json_str(body, "wifi_sta_ssid", n.wifi_sta_ssid, sizeof(n.wifi_sta_ssid), current.wifi_sta_ssid);
    json_str(body, "cellular_apn", n.cellular_apn, sizeof(n.cellular_apn), current.cellular_apn);

    const cJSON *wpw = cJSON_GetObjectItemCaseSensitive(body, "wifi_sta_password");
    if (wpw && cJSON_IsString(wpw) && strlen(wpw->valuestring) > 0) {
        snprintf(n.wifi_sta_password, sizeof(n.wifi_sta_password), "%s", wpw->valuestring);
    }
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(body, "cellular_pin");
    if (pin && cJSON_IsString(pin) && strlen(pin->valuestring) > 0) {
        snprintf(n.cellular_pin, sizeof(n.cellular_pin), "%s", pin->valuestring);
    }
    cJSON_Delete(body);

    if (n.transport == TRANSPORT_WIFI && strlen(n.wifi_sta_ssid) == 0) {
        return wp_send_error(req, "400 Bad Request", "wifi_sta_ssid is required for WiFi transport");
    }

    bool transport_changed = (n.transport != current.transport);
    config_store_set_net_settings(&n);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "reboot_required", transport_changed);
    return wp_send_json(req, resp);
}

/* ---- Position (GNSS) reporting settings ---- */

static esp_err_t position_get_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    position_settings_t p;
    config_store_get_position_settings(&p);

    net_settings_t n;
    config_store_get_net_settings(&n);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", p.enabled);
    cJSON_AddNumberToObject(o, "interval_ms", p.interval_ms);
    cJSON_AddBoolToObject(o, "available", n.transport == TRANSPORT_CELLULAR);
    return wp_send_json(req, o);
}

static esp_err_t position_put_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    position_settings_t current;
    config_store_get_position_settings(&current);

    position_settings_t p = current;
    p.enabled = json_bool(body, "enabled", current.enabled);
    p.interval_ms = (uint32_t)json_int(body, "interval_ms", (int)current.interval_ms);
    cJSON_Delete(body);

    if (p.enabled && p.interval_ms == 0) {
        return wp_send_error(req, "400 Bad Request", "interval_ms must be > 0 when position reporting is enabled");
    }

    config_store_set_position_settings(&p);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

/* ---- Battery monitor (onboard LTC4015, Walter Feels only) settings ---- */

static esp_err_t battery_get_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    battery_settings_t b;
    config_store_get_battery_settings(&b);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "chemistry", b.chemistry);
    cJSON_AddNumberToObject(o, "cell_count", b.cell_count);
    cJSON_AddBoolToObject(o, "enabled", b.enabled);
    cJSON_AddNumberToObject(o, "interval_ms", b.interval_ms);
    cJSON_AddBoolToObject(o, "sync_with_mqtt", b.sync_with_mqtt);
    return wp_send_json(req, o);
}

static esp_err_t battery_put_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    battery_settings_t current;
    config_store_get_battery_settings(&current);

    battery_settings_t b = current;
    b.chemistry = (battery_chemistry_t)json_int(body, "chemistry", current.chemistry);
    b.cell_count = (uint8_t)json_int(body, "cell_count", current.cell_count);
    b.enabled = json_bool(body, "enabled", current.enabled);
    b.interval_ms = (uint32_t)json_int(body, "interval_ms", (int)current.interval_ms);
    b.sync_with_mqtt = json_bool(body, "sync_with_mqtt", current.sync_with_mqtt);
    cJSON_Delete(body);

    if (b.cell_count == 0) {
        return wp_send_error(req, "400 Bad Request", "cell_count must be > 0");
    }
    if (b.enabled && !b.sync_with_mqtt && b.interval_ms == 0) {
        return wp_send_error(req, "400 Bad Request", "interval_ms must be > 0 when enabled");
    }

    config_store_set_battery_settings(&b);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

/* ---- SD card logging format settings ---- */

static esp_err_t sd_get_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    sd_settings_t s;
    config_store_get_sd_settings(&s);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "log_format", s.log_format);
    cJSON_AddStringToObject(o, "station_name", s.station_name);
    return wp_send_json(req, o);
}

static esp_err_t sd_put_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    sd_settings_t current;
    config_store_get_sd_settings(&current);

    sd_settings_t s = current;
    s.log_format = (sd_log_format_t)json_int(body, "log_format", current.log_format);
    json_str(body, "station_name", s.station_name, sizeof(s.station_name), current.station_name);
    cJSON_Delete(body);

    if (s.log_format != SD_LOG_FORMAT_LONG && s.log_format != SD_LOG_FORMAT_WIDE_SIMPLE &&
        s.log_format != SD_LOG_FORMAT_WIDE_TOA5) {
        return wp_send_error(req, "400 Bad Request", "invalid log_format");
    }

    config_store_set_sd_settings(&s);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

/* ---- Portal password ---- */

static esp_err_t password_put_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    const cJSON *old_pw = cJSON_GetObjectItemCaseSensitive(body, "old");
    const cJSON *new_pw = cJSON_GetObjectItemCaseSensitive(body, "new");
    bool old_ok = old_pw && cJSON_IsString(old_pw) && config_store_verify_portal_password(old_pw->valuestring);
    bool new_valid = new_pw && cJSON_IsString(new_pw) && strlen(new_pw->valuestring) >= 4;

    char new_pw_copy[64] = { 0 };
    if (new_valid) {
        snprintf(new_pw_copy, sizeof(new_pw_copy), "%s", new_pw->valuestring);
    }
    cJSON_Delete(body);

    if (!old_ok) {
        return wp_send_error(req, "401 Unauthorized", "current password is incorrect");
    }
    if (!new_valid) {
        return wp_send_error(req, "400 Bad Request", "new password must be at least 4 characters");
    }

    config_store_set_portal_password(new_pw_copy);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

/* ---- Export / import ---- */

static esp_err_t export_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    device_config_t cfg;
    config_store_get_snapshot(&cfg);
    cJSON *root = config_store_to_json(&cfg);

    /* Redact secrets - a leaked backup file shouldn't hand out
     * credentials. Import preserves the device's existing values for
     * any of these left blank. */
    cJSON_DeleteItemFromObject(root, "portal_password_hash");
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (mqtt) {
        cJSON_ReplaceItemInObject(mqtt, "password", cJSON_CreateString(""));
    }
    cJSON *net = cJSON_GetObjectItemCaseSensitive(root, "net");
    if (net) {
        cJSON_ReplaceItemInObject(net, "wifi_sta_password", cJSON_CreateString(""));
        cJSON_ReplaceItemInObject(net, "cellular_pin", cJSON_CreateString(""));
    }

    return wp_send_json(req, root);
}

static esp_err_t import_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    device_config_t current;
    config_store_get_snapshot(&current);

    device_config_t imported;
    config_store_from_json(body, &imported);
    cJSON_Delete(body);

    /* Preserve secrets omitted from the (redacted) export format rather
     * than wiping them on import. */
    snprintf(imported.portal_password_hash, sizeof(imported.portal_password_hash), "%s",
             current.portal_password_hash);
    if (strlen(imported.mqtt.password) == 0) {
        snprintf(imported.mqtt.password, sizeof(imported.mqtt.password), "%s", current.mqtt.password);
    }
    if (strlen(imported.net.wifi_sta_password) == 0) {
        snprintf(imported.net.wifi_sta_password, sizeof(imported.net.wifi_sta_password), "%s",
                 current.net.wifi_sta_password);
    }
    if (strlen(imported.net.cellular_pin) == 0) {
        snprintf(imported.net.cellular_pin, sizeof(imported.net.cellular_pin), "%s", current.net.cellular_pin);
    }

    esp_err_t ret = config_store_replace_all(&imported);
    if (ret != ESP_OK) {
        return wp_send_error(req, "500 Internal Server Error", "failed to save imported config");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "reboot_required", true);
    return wp_send_json(req, resp);
}

/* ---- Reboot ---- */

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    esp_err_t ret = wp_send_json(req, resp);

    /* Reboot shortly after the response is flushed, rather than from
     * inside the HTTP handler itself. */
    const esp_timer_create_args_t targs = { .callback = reboot_timer_cb, .name = "reboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        esp_timer_start_once(t, 500000);
    }
    return ret;
}

/* ---- Factory reset ---- */

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    esp_err_t err = config_store_factory_reset();
    if (err != ESP_OK) {
        return wp_send_error(req, "500 Internal Server Error", "failed to erase stored settings");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    esp_err_t ret = wp_send_json(req, resp);

    /* config_store_factory_reset() only erases NVS - a reboot is
     * needed for config_store_init() to actually pick up the cleared
     * state (same reboot-after-response pattern as reboot_handler()). */
    const esp_timer_create_args_t targs = { .callback = reboot_timer_cb, .name = "factory_reset" };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        esp_timer_start_once(t, 500000);
    }
    return ret;
}

void api_settings_register_routes(httpd_handle_t server)
{
    httpd_uri_t u;

    u = (httpd_uri_t){ .uri = "/api/settings/mqtt", .method = HTTP_GET, .handler = mqtt_get_handler };
    httpd_register_uri_handler(server, &u);
    u = (httpd_uri_t){ .uri = "/api/settings/mqtt", .method = HTTP_PUT, .handler = mqtt_put_handler };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri = "/api/settings/network", .method = HTTP_GET, .handler = network_get_handler };
    httpd_register_uri_handler(server, &u);
    u = (httpd_uri_t){ .uri = "/api/settings/network", .method = HTTP_PUT, .handler = network_put_handler };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri = "/api/settings/position", .method = HTTP_GET, .handler = position_get_handler };
    httpd_register_uri_handler(server, &u);
    u = (httpd_uri_t){ .uri = "/api/settings/position", .method = HTTP_PUT, .handler = position_put_handler };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri = "/api/settings/battery", .method = HTTP_GET, .handler = battery_get_handler };
    httpd_register_uri_handler(server, &u);
    u = (httpd_uri_t){ .uri = "/api/settings/battery", .method = HTTP_PUT, .handler = battery_put_handler };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri = "/api/settings/sd", .method = HTTP_GET, .handler = sd_get_handler };
    httpd_register_uri_handler(server, &u);
    u = (httpd_uri_t){ .uri = "/api/settings/sd", .method = HTTP_PUT, .handler = sd_put_handler };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri = "/api/settings/password", .method = HTTP_PUT, .handler = password_put_handler };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri = "/api/settings/export", .method = HTTP_GET, .handler = export_handler };
    httpd_register_uri_handler(server, &u);
    u = (httpd_uri_t){ .uri = "/api/settings/import", .method = HTTP_POST, .handler = import_handler };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri = "/api/system/reboot", .method = HTTP_POST, .handler = reboot_handler };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = factory_reset_handler };
    httpd_register_uri_handler(server, &u);
}
