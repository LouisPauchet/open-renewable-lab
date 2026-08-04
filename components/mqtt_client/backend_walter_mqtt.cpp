/* Cellular-transport MQTT backend: wraps the on-modem MQTT client in
 * DPTechnics' WalterModem library.
 *
 * WalterModem API usage below was verified against the actual fetched
 * managed_components/dptechnics__walter-modem/src/WalterModem.h.
 *
 * Only meaningful on esp32s3 (the Walter module's chip); on any other
 * target every operation below fails cleanly (ESP_ERR_NOT_SUPPORTED),
 * since dptechnics/walter-modem isn't even fetched for other targets
 * (main/idf_component.yml). */

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "mqtt_backend.h"

static const char *TAG = "mqtt_walter";
static bool s_connected;

#if CONFIG_IDF_TARGET_ESP32S3

#include "cellular_transport_cpp.h"

static char s_host[64];
static uint16_t s_port;

static esp_err_t be_init(const mqtt_settings_t *cfg)
{
    snprintf(s_host, sizeof(s_host), "%s", cfg->host);
    s_port = cfg->port;

    WalterModem &modem = cellular_transport_get_modem();

    uint8_t tls_profile = 1; /* profile slots are 1-6 per tlsConfigProfile()'s doc comment */
    if (cfg->use_tls) {
        /* WALTER_MODEM_TLS_VALIDATION_CA requires a CA certificate
         * already uploaded via tlsWriteCredential() at a matching
         * ca_cert_id (see the SDK's own examples/mqtts/main/mqtts.cpp)
         * - this firmware has no certificate-upload feature yet, so CA
         * validation would always fail here. Until that's built,
         * "Allow insecure TLS" is the only working option for the
         * cellular/on-modem MQTT backend, not just a self-signed-broker
         * convenience like it is for the WiFi backend. */
        if (!cfg->tls_allow_insecure) {
            ESP_LOGE(TAG, "cellular MQTT backend has no CA certificate upload yet - "
                          "enable 'Allow insecure TLS' or use the WiFi transport for CA-validated TLS");
            return ESP_FAIL;
        }
        if (!modem.tlsConfigProfile(tls_profile, WALTER_MODEM_TLS_VALIDATION_NONE, WALTER_MODEM_TLS_VERSION_12)) {
            ESP_LOGE(TAG, "tlsConfigProfile failed");
            return ESP_FAIL;
        }
    }

    if (!modem.mqttConfig(cfg->client_id, cfg->username, cfg->password, cfg->use_tls ? tls_profile : 0)) {
        ESP_LOGE(TAG, "mqttConfig failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t be_connect(void)
{
    WalterModem &modem = cellular_transport_get_modem();
    if (!modem.mqttConnect(s_host, s_port)) {
        ESP_LOGE(TAG, "mqttConnect failed");
        return ESP_FAIL;
    }
    s_connected = true;
    return ESP_OK;
}

static esp_err_t be_disconnect(void)
{
    WalterModem &modem = cellular_transport_get_modem();
    modem.mqttDisconnect();
    s_connected = false;
    return ESP_OK;
}

static esp_err_t be_publish(const char *topic, const char *payload, int qos, bool retain)
{
    (void)retain; /* mqttPublish() has no retain parameter - the on-modem MQTT client doesn't support it */
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    WalterModem &modem = cellular_transport_get_modem();
    /* mqttPublish() takes a non-const uint8_t* even though it only reads
     * the buffer (confirmed via WalterModem.h) - cast away const rather
     * than copy, we own this buffer for the duration of the call. */
    if (!modem.mqttPublish(topic, (uint8_t *)(const void *)payload, strlen(payload), (uint8_t)qos)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t be_deinit(void)
{
    if (s_connected) {
        cellular_transport_get_modem().mqttDisconnect();
        s_connected = false;
    }
    return ESP_OK;
}

#else /* !CONFIG_IDF_TARGET_ESP32S3 */

static esp_err_t be_init(const mqtt_settings_t *cfg)
{
    (void)cfg;
    ESP_LOGE(TAG, "cellular MQTT backend is only supported on esp32s3 (the Walter module)");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t be_connect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t be_disconnect(void)
{
    s_connected = false;
    return ESP_OK;
}

static esp_err_t be_publish(const char *topic, const char *payload, int qos, bool retain)
{
    (void)topic;
    (void)payload;
    (void)qos;
    (void)retain;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t be_deinit(void)
{
    s_connected = false;
    return ESP_OK;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */

static bool be_is_connected(void)
{
    return s_connected;
}

static const mqttc_backend_vtable_t s_vtable = {
    .init = be_init,
    .connect = be_connect,
    .disconnect = be_disconnect,
    .publish = be_publish,
    .is_connected = be_is_connected,
    .deinit = be_deinit,
};

const mqttc_backend_vtable_t *backend_walter_mqtt_get_vtable(void)
{
    return &s_vtable;
}
