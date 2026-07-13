/* Cellular-transport MQTT backend: wraps the on-modem MQTT client in
 * DPTechnics' WalterModem library.
 *
 * *** LOW CONFIDENCE / NEEDS VERIFICATION *** - see the warning in
 * cellular_transport/include/cellular_transport.h. Every WalterModem
 * call below (tlsConfigProfile, mqttConfig, mqttConnect, mqttPublish,
 * mqttDisconnect, and the WalterModemTlsVersion/TlsValidation enum
 * names) is a best-effort guess from a research summary, not verified
 * against the real header - fix these once
 * managed_components/dptechnics__walter-modem is fetched and its
 * WalterModem.h can be read directly. */

#include <cstdio>
#include <cstring>

#include "cellular_transport_cpp.h"
#include "esp_log.h"
#include "mqtt_backend.h"

static const char *TAG = "mqtt_walter";

static char s_host[64];
static uint16_t s_port;
static bool s_connected;

static esp_err_t be_init(const mqtt_settings_t *cfg)
{
    snprintf(s_host, sizeof(s_host), "%s", cfg->host);
    s_port = cfg->port;

    WalterModem &modem = cellular_transport_get_modem();

    uint8_t tls_profile = 1; /* VERIFY: valid profile slot range (research noted up to 6 profiles) */
    if (cfg->use_tls) {
        WalterModemTlsValidation validation =
            cfg->tls_allow_insecure ? WALTER_MODEM_TLS_VALIDATION_NONE : WALTER_MODEM_TLS_VALIDATION_CA;
        /* VERIFY: tlsConfigProfile method name/signature. */
        modem.tlsConfigProfile(tls_profile, WALTER_MODEM_TLS_VERSION_12, validation);
    }

    /* VERIFY: mqttConfig signature (client id / username / password / tls profile slot). */
    if (!modem.mqttConfig(cfg->client_id, cfg->username, cfg->password, cfg->use_tls ? tls_profile : 0)) {
        ESP_LOGE(TAG, "mqttConfig failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t be_connect(void)
{
    WalterModem &modem = cellular_transport_get_modem();
    /* VERIFY: mqttConnect signature/return type. */
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
    modem.mqttDisconnect(); /* VERIFY method name */
    s_connected = false;
    return ESP_OK;
}

static esp_err_t be_publish(const char *topic, const char *payload, int qos, bool retain)
{
    (void)retain; /* VERIFY: does mqttPublish take a retain flag? */
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    WalterModem &modem = cellular_transport_get_modem();
    /* VERIFY: mqttPublish signature (payload pointer/length, qos type). */
    if (!modem.mqttPublish(topic, (const uint8_t *)payload, strlen(payload), qos)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool be_is_connected(void)
{
    return s_connected;
}

static esp_err_t be_deinit(void)
{
    if (s_connected) {
        cellular_transport_get_modem().mqttDisconnect(); /* VERIFY */
        s_connected = false;
    }
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

const mqttc_backend_vtable_t *backend_walter_mqtt_get_vtable(void)
{
    return &s_vtable;
}
