#pragma once

/* Private to web_portal - shared helpers between web_portal.c, auth.c,
 * and the api_*.c route handlers. Not part of the component's public
 * include/. */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- response helpers ---- */

/* Sends 200 OK + JSON body, then deletes json. */
esp_err_t wp_send_json(httpd_req_t *req, cJSON *json);

/* Sends an arbitrary status line (e.g. "400 Bad Request") + JSON body,
 * then deletes json. */
esp_err_t wp_send_json_status(httpd_req_t *req, const char *status, cJSON *json);

/* Sends {"error": message} with the given status line. */
esp_err_t wp_send_error(httpd_req_t *req, const char *status, const char *message);

/* Reads the request body (bounded to a few KB) and parses it as JSON.
 * On failure, sends a 400 response itself and returns ESP_FAIL - caller
 * should just `return ESP_OK` (the response has already been sent). */
esp_err_t wp_read_json_body(httpd_req_t *req, cJSON **out_json);

/* Parses a numeric id out of the URI: expects req->uri to start with
 * `prefix` followed by digits, optionally followed by `suffix` (e.g.
 * prefix="/api/variables/" suffix="/preview"). Returns false (and sends
 * a 400 response itself) if the id segment isn't a valid number. */
bool wp_parse_uri_id(httpd_req_t *req, const char *prefix, const char *suffix, uint16_t *out_id);

/* ---- auth ---- */

/* Returns true if the request carries a valid session cookie. On
 * failure, sends a 401 JSON response itself - caller should just
 * `return ESP_OK`. */
bool wp_auth_require(httpd_req_t *req);

void auth_init(void);
void auth_register_routes(httpd_handle_t server);

/* ---- route registration ---- */

void api_variables_register_routes(httpd_handle_t server);
void api_settings_register_routes(httpd_handle_t server);
void api_status_register_routes(httpd_handle_t server);
void api_bus_register_routes(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
