/* WiFi-transport MQTT backend: wraps ESP-IDF's esp-mqtt component over
 * WiFi/lwIP/esp-tls.
 *
 * NOTE: this local ESP-IDF v6.0.2 checkout is missing the mqtt/esp-mqtt
 * submodule source (components/mqtt/esp-mqtt only has test_apps/, no
 * include/ or src/), so mqtt_client.h's exact field layout could not be
 * verified against source in this environment - the checkout itself
 * needs `install.bat`/submodule update re-run before this component
 * will build. The API used below (broker.address.uri,
 * credentials.*, broker.verification.*) has been stable and documented
 * across IDF 5.x/6.x; double-check against mqtt_client.h once the
 * submodule is present. */

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_backend.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt_esp";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "connected");
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "disconnected");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "mqtt error event");
            break;
        default:
            break;
    }
}

static esp_err_t be_init(const mqtt_settings_t *cfg)
{
    if (s_client) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }

    char uri[80];
    snprintf(uri, sizeof(uri), "%s://%s:%u", cfg->use_tls ? "mqtts" : "mqtt", cfg->host, cfg->port);

    esp_mqtt_client_config_t mcfg = { 0 };
    mcfg.broker.address.uri = uri; /* esp-mqtt copies config strings at init time */

    if (strlen(cfg->client_id) > 0) {
        mcfg.credentials.client_id = cfg->client_id;
    }
    if (strlen(cfg->username) > 0) {
        mcfg.credentials.username = cfg->username;
    }
    if (strlen(cfg->password) > 0) {
        mcfg.credentials.authentication.password = cfg->password;
    }

    if (cfg->use_tls) {
        if (cfg->tls_allow_insecure) {
            /* Lab-friendly escape hatch for self-signed brokers: skip
             * hostname verification. TLS is still negotiated/encrypted,
             * just without validating the server's identity. */
            mcfg.broker.verification.skip_cert_common_name_check = true;
        } else {
            mcfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    s_client = esp_mqtt_client_init(&mcfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    return esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

static esp_err_t be_connect(void)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_start(s_client);
}

static esp_err_t be_disconnect(void)
{
    if (!s_client) {
        return ESP_OK;
    }
    esp_err_t err = esp_mqtt_client_stop(s_client);
    s_connected = false;
    return err;
}

static esp_err_t be_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

static bool be_is_connected(void)
{
    return s_connected;
}

static esp_err_t be_deinit(void)
{
    if (s_client) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    return ESP_OK;
}

static const mqttc_backend_vtable_t s_vtable = {
    .init = be_init,
    .connect = be_connect,
    .disconnect = be_disconnect,
    .publish = be_publish,
    .is_connected = be_is_connected,
    .deinit = be_deinit,
};

const mqttc_backend_vtable_t *backend_esp_mqtt_get_vtable(void)
{
    return &s_vtable;
}
