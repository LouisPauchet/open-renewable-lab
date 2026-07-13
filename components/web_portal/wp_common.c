#include <stdlib.h>
#include <string.h>

#include "web_portal_internal.h"

#define MAX_BODY_LEN 4096

esp_err_t wp_send_json_status(httpd_req_t *req, const char *status, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!str) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, str);
    cJSON_free(str);
    return err;
}

esp_err_t wp_send_json(httpd_req_t *req, cJSON *json)
{
    return wp_send_json_status(req, "200 OK", json);
}

esp_err_t wp_send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    return wp_send_json_status(req, status, o);
}

esp_err_t wp_read_json_body(httpd_req_t *req, cJSON **out_json)
{
    if (req->content_len <= 0 || req->content_len > MAX_BODY_LEN) {
        wp_send_error(req, "400 Bad Request", "invalid or too large request body");
        return ESP_FAIL;
    }

    char *buf = malloc((size_t)req->content_len + 1);
    if (!buf) {
        wp_send_error(req, "500 Internal Server Error", "out of memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            wp_send_error(req, "400 Bad Request", "failed to read request body");
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        wp_send_error(req, "400 Bad Request", "invalid JSON");
        return ESP_FAIL;
    }

    *out_json = json;
    return ESP_OK;
}

bool wp_parse_uri_id(httpd_req_t *req, const char *prefix, const char *suffix, uint16_t *out_id)
{
    size_t prefix_len = strlen(prefix);
    const char *uri = req->uri;
    if (strncmp(uri, prefix, prefix_len) != 0) {
        wp_send_error(req, "400 Bad Request", "malformed URI");
        return false;
    }

    const char *id_start = uri + prefix_len;
    char id_buf[8];
    size_t i = 0;
    while (id_start[i] && id_start[i] != '?' && id_start[i] != '/' && i < sizeof(id_buf) - 1) {
        id_buf[i] = id_start[i];
        i++;
    }
    id_buf[i] = '\0';

    if (i == 0) {
        wp_send_error(req, "400 Bad Request", "missing id");
        return false;
    }

    char *endptr = NULL;
    long val = strtol(id_buf, &endptr, 10);
    if (*endptr != '\0' || val <= 0 || val > UINT16_MAX) {
        wp_send_error(req, "400 Bad Request", "invalid id");
        return false;
    }

    if (suffix && strlen(suffix) > 0) {
        const char *after_id = id_start + i;
        if (strcmp(after_id, suffix) != 0) {
            wp_send_error(req, "400 Bad Request", "malformed URI");
            return false;
        }
    }

    *out_id = (uint16_t)val;
    return true;
}
