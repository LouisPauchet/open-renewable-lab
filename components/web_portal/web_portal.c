#include "web_portal.h"

#include "dns_hijack.h"
#include "esp_log.h"
#include "web_portal_internal.h"

static const char *TAG = "web_portal";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* Catch-all for every GET request not matched by a more specific /api/
 * route above it in registration order - including the OS captive-
 * portal probe paths (/generate_204, /hotspot-detect.html, ncsi.txt,
 * ...). Serving the SPA (rather than each probe's expected "no portal
 * here" response) is what makes phones/laptops pop the login browser;
 * we always want that, since the portal should always be reachable. */
static esp_err_t spa_catch_all_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

esp_err_t web_portal_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 32; /* bump generously past the actual handler count when adding new
                                    * /api/... routes - httpd_register_uri_handler() fails silently
                                    * (just an "no slots left" warning) past this cap, not an error
                                    * anyone would notice without reading the boot log closely */
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    auth_init();
    auth_register_routes(server);
    api_variables_register_routes(server);
    api_settings_register_routes(server);
    api_status_register_routes(server);
    api_bus_register_routes(server);

    httpd_uri_t catch_all = { .uri = "/*", .method = HTTP_GET, .handler = spa_catch_all_handler };
    httpd_register_uri_handler(server, &catch_all);

    err = dns_hijack_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "captive-portal DNS hijack failed to start: %s", esp_err_to_name(err));
        /* non-fatal: the portal is still reachable at 192.168.4.1 directly */
    }

    ESP_LOGI(TAG, "web portal ready");
    return ESP_OK;
}
