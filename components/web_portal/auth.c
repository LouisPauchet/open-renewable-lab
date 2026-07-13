#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "web_portal_internal.h"

#define MAX_SESSIONS 4
#define SESSION_IDLE_TIMEOUT_US ((int64_t)30 * 60 * 1000000)

typedef struct {
    char token[33]; /* 16 random bytes, hex-encoded */
    int64_t expires_at_us;
    bool in_use;
} session_t;

static session_t s_sessions[MAX_SESSIONS];
static SemaphoreHandle_t s_mutex;

void auth_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    s_mutex = xSemaphoreCreateMutex();
}

static void generate_token(char out[33])
{
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 0x0F];
    }
    out[32] = '\0';
}

/* Caller must hold s_mutex. Expired sessions are lazily reclaimed here. */
static session_t *find_session_locked(const char *token)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].in_use && strcmp(s_sessions[i].token, token) == 0) {
            if (now > s_sessions[i].expires_at_us) {
                s_sessions[i].in_use = false;
                return NULL;
            }
            return &s_sessions[i];
        }
    }
    return NULL;
}

/* Caller must hold s_mutex. */
static session_t *allocate_session_locked(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].in_use) {
            return &s_sessions[i];
        }
    }
    /* All slots busy: evict the one closest to expiring. */
    int oldest = 0;
    for (int i = 1; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].expires_at_us < s_sessions[oldest].expires_at_us) {
            oldest = i;
        }
    }
    return &s_sessions[oldest];
}

static bool extract_cookie_token(httpd_req_t *req, char *out, size_t out_size)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0) {
        return false;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, len + 1) != ESP_OK) {
        free(buf);
        return false;
    }

    bool found = false;
    char *p = strstr(buf, "session=");
    if (p) {
        p += strlen("session=");
        size_t i = 0;
        while (p[i] && p[i] != ';' && i < out_size - 1) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        found = (i > 0);
    }

    free(buf);
    return found;
}

bool wp_auth_require(httpd_req_t *req)
{
    char token[64];
    if (!extract_cookie_token(req, token, sizeof(token))) {
        wp_send_error(req, "401 Unauthorized", "not logged in");
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    session_t *s = find_session_locked(token);
    bool ok = (s != NULL);
    if (ok) {
        s->expires_at_us = esp_timer_get_time() + SESSION_IDLE_TIMEOUT_US; /* sliding expiry */
    }
    xSemaphoreGive(s_mutex);

    if (!ok) {
        wp_send_error(req, "401 Unauthorized", "session expired");
        return false;
    }
    return true;
}

static esp_err_t login_handler(httpd_req_t *req)
{
    cJSON *body;
    if (wp_read_json_body(req, &body) != ESP_OK) {
        return ESP_OK; /* wp_read_json_body already sent an error response */
    }

    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(body, "password");
    bool ok = pw && cJSON_IsString(pw) && config_store_verify_portal_password(pw->valuestring);
    cJSON_Delete(body);

    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* naive brute-force slow-down */
        return wp_send_error(req, "401 Unauthorized", "invalid password");
    }

    char token_copy[33];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    session_t *s = allocate_session_locked();
    generate_token(s->token);
    s->in_use = true;
    s->expires_at_us = esp_timer_get_time() + SESSION_IDLE_TIMEOUT_US;
    strcpy(token_copy, s->token);
    xSemaphoreGive(s_mutex);

    char cookie[80];
    snprintf(cookie, sizeof(cookie), "session=%s; Path=/; HttpOnly; Max-Age=1800", token_copy);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    char token[64];
    if (extract_cookie_token(req, token, sizeof(token))) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        session_t *s = find_session_locked(token);
        if (s) {
            s->in_use = false;
        }
        xSemaphoreGive(s_mutex);
    }

    httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; HttpOnly; Max-Age=0");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return wp_send_json(req, resp);
}

void auth_register_routes(httpd_handle_t server)
{
    httpd_uri_t login_uri = { .uri = "/api/login", .method = HTTP_POST, .handler = login_handler };
    httpd_register_uri_handler(server, &login_uri);

    httpd_uri_t logout_uri = { .uri = "/api/logout", .method = HTTP_POST, .handler = logout_handler };
    httpd_register_uri_handler(server, &logout_uri);
}
