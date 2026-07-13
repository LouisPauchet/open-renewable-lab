#include "sdi12_bus.h"
#include "web_portal_internal.h"

/* Probing all 62 possible addresses can take a few seconds on an empty
 * bus (100ms timeout x 62) - acceptable for an admin "scan" button. */
static esp_err_t sdi12_scan_handler(httpd_req_t *req)
{
    if (!wp_auth_require(req)) {
        return ESP_OK;
    }

    char found[62];
    size_t count = 0;
    esp_err_t err = sdi12_scan_addresses(found, sizeof(found), &count);
    if (err != ESP_OK) {
        return wp_send_error(req, "503 Service Unavailable", "SDI-12 bus not available");
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        char s[2] = { found[i], '\0' };
        cJSON_AddItemToArray(arr, cJSON_CreateString(s));
    }
    return wp_send_json(req, arr);
}

void api_bus_register_routes(httpd_handle_t server)
{
    httpd_uri_t u = { .uri = "/api/bus/sdi12/scan", .method = HTTP_GET, .handler = sdi12_scan_handler };
    httpd_register_uri_handler(server, &u);
}
