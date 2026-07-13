#include <string.h>

#include "config_store.h"
#include "sampling_engine.h"
#include "web_portal_internal.h"

static bool is_valid_sdi12_address(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* Returns NULL if valid, else a static error message. */
static const char *validate_variable(const variable_config_t *v)
{
    if (strlen(v->name) == 0) {
        return "name must not be empty";
    }
    if (v->sample_interval_ms == 0) {
        return "sample_interval_ms must be > 0";
    }
    if (v->log_interval_ms < v->sample_interval_ms) {
        return "log_interval_ms must be >= sample_interval_ms";
    }
    if (v->aggregate_mask == 0) {
        return "at least one aggregate (raw/mean/min/max/stddev) must be selected";
    }
    if (v->bus_type == BUS_TYPE_SDI12 && !is_valid_sdi12_address(v->addr.sdi12.address)) {
        return "SDI-12 address must be one of 0-9, A-Z, a-z";
    }
    if (v->bus_type == BUS_TYPE_I2C && v->addr.i2c.i2c_addr > 0x77) {
        return "I2C address out of range";
    }
    return NULL;
}

static esp_err_t list_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    variable_config_t vars[MAX_VARIABLES];
    size_t n = config_store_get_variables(vars, MAX_VARIABLES);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, config_store_variable_to_json(&vars[i]));
    }
    return wp_send_json(req, arr);
}

static esp_err_t create_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    variable_config_t v;
    config_store_variable_from_json(body, &v);
    cJSON_Delete(body);

    const char *err = validate_variable(&v);
    if (err) {
        return wp_send_error(req, "400 Bad Request", err);
    }

    uint16_t id;
    esp_err_t ret = config_store_add_variable(&v, &id);
    if (ret != ESP_OK) {
        return wp_send_error(req, "507 Insufficient Storage", "maximum number of variables reached");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "id", id);
    return wp_send_json(req, resp);
}

static esp_err_t update_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    uint16_t id;
    if (!wp_parse_uri_id(req, "/api/variables/", "", &id)) {
        return ESP_OK;
    }

    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK;
    }

    variable_config_t v;
    config_store_variable_from_json(body, &v);
    cJSON_Delete(body);

    const char *err = validate_variable(&v);
    if (err) {
        return wp_send_error(req, "400 Bad Request", err);
    }

    esp_err_t ret = config_store_update_variable(id, &v);
    if (ret == ESP_ERR_NOT_FOUND) {
        return wp_send_error(req, "404 Not Found", "no such variable");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    uint16_t id;
    if (!wp_parse_uri_id(req, "/api/variables/", "", &id)) {
        return ESP_OK;
    }

    esp_err_t ret = config_store_delete_variable(id);
    if (ret == ESP_ERR_NOT_FOUND) {
        return wp_send_error(req, "404 Not Found", "no such variable");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

static esp_err_t preview_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    uint16_t id;
    if (!wp_parse_uri_id(req, "/api/variables/", "/preview", &id)) {
        return ESP_OK;
    }

    double value = 0.0;
    esp_err_t ret = sampling_engine_read_once(id, &value);
    if (ret == ESP_ERR_NOT_FOUND) {
        return wp_send_error(req, "404 Not Found", "no such variable");
    }
    if (ret != ESP_OK) {
        return wp_send_error(req, "503 Service Unavailable", "sensor read failed (bus driver missing or read error)");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "value", value);
    return wp_send_json(req, resp);
}

void api_variables_register_routes(httpd_handle_t server)
{
    httpd_uri_t list_uri = { .uri = "/api/variables", .method = HTTP_GET, .handler = list_handler };
    httpd_register_uri_handler(server, &list_uri);

    httpd_uri_t create_uri = { .uri = "/api/variables", .method = HTTP_POST, .handler = create_handler };
    httpd_register_uri_handler(server, &create_uri);

    httpd_uri_t update_uri = { .uri = "/api/variables/*", .method = HTTP_PUT, .handler = update_handler };
    httpd_register_uri_handler(server, &update_uri);

    httpd_uri_t delete_uri = { .uri = "/api/variables/*", .method = HTTP_DELETE, .handler = delete_handler };
    httpd_register_uri_handler(server, &delete_uri);

    httpd_uri_t preview_uri = { .uri = "/api/variables/*", .method = HTTP_POST, .handler = preview_handler };
    httpd_register_uri_handler(server, &preview_uri);
}
