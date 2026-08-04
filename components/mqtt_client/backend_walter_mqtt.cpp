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

#include "device_id.h"
#include "esp_log.h"
#include "mqtt_backend.h"

static const char *TAG = "mqtt_walter";
static bool s_connected;

#if CONFIG_IDF_TARGET_ESP32S3

#include "cellular_transport_cpp.h"

static char s_host[64];
static uint16_t s_port;

/* ISRG Root X1 - Let's Encrypt's root CA, by far the most common CA
 * behind self-hosted MQTT brokers (including this project's own
 * ThingsBoard test setup). The modem's TLS engine runs entirely inside
 * the Sequans chip with its own separate NVRAM certificate store - it
 * has no built-in public-CA trust bundle and can't reach into the
 * ESP32's own compiled-in esp_crt_bundle (confirmed: dptechnics'
 * own examples/mqtts and examples/https hardcode+upload a root CA the
 * same way, and github.com/QuickSpot/walter-esp-idf#163 reports the
 * modem doesn't build a chain from handshake-presented certs either -
 * so the exact CA that signed the broker's cert has to be uploaded
 * explicitly). This exact PEM is copied verbatim from ESP-IDF's own
 * bundled trusted-CA file (components/mbedtls/esp_crt_bundle/
 * cacrt_all.pem) - the same trust material the WiFi/esp-mqtt backend's
 * esp_crt_bundle already uses - rather than re-fetched or hand-typed,
 * so both backends trust the identical certificate bytes. Valid until
 * 2035-06-04. */
static const char *ISRG_ROOT_X1_PEM =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "m+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
    "-----END CERTIFICATE-----\n";

/* Slot 12, matching the SDK's own examples/mqtts - slots 0-10 are
 * reserved for Sequans/BlueCherry internal use per tlsWriteCredential()'s
 * doc comment. Uploaded once per boot (a modem NVRAM write, not
 * something to repeat on every be_init() call - batch mode calls
 * be_init() on every transmit window). */
#define LETS_ENCRYPT_CA_CERT_SLOT 12
static bool s_ca_cert_uploaded;

static esp_err_t be_init(const mqtt_settings_t *cfg)
{
    snprintf(s_host, sizeof(s_host), "%s", cfg->host);
    s_port = cfg->port;

    /* mqttConfig()'s AT+SQNSMQTTCFG command takes client_id verbatim,
     * quoted, with no built-in fallback for an empty string (unlike
     * the SDK's own C++ default parameter "walter-mqtt-client", which
     * we bypass by always passing cfg->client_id explicitly) - an
     * empty client_id field (never set in the portal) sends
     * `AT+SQNSMQTTCFG=0,""`, which the modem firmware rejects,
     * confirmed on real hardware. Fall back to a device-unique id. */
    char client_id[64];
    if (cfg->client_id[0] != '\0') {
        snprintf(client_id, sizeof(client_id), "%s", cfg->client_id);
    } else {
        snprintf(client_id, sizeof(client_id), "walter-%s", device_id_get());
    }

    WalterModem &modem = cellular_transport_get_modem();

    uint8_t tls_profile = 1; /* profile slots are 1-6 per tlsConfigProfile()'s doc comment */
    if (cfg->use_tls) {
        if (cfg->tls_allow_insecure) {
            if (!modem.tlsConfigProfile(tls_profile, WALTER_MODEM_TLS_VALIDATION_NONE, WALTER_MODEM_TLS_VERSION_12)) {
                ESP_LOGE(TAG, "tlsConfigProfile failed");
                return ESP_FAIL;
            }
        } else {
            /* CA validation needs the signing CA's certificate already
             * uploaded to the modem's NVRAM (see ISRG_ROOT_X1_PEM's
             * comment above) - only actually works if the broker's
             * certificate chains to Let's Encrypt. A broker using a
             * different CA still needs "Allow insecure TLS" until this
             * is generalized to more than one embedded/uploadable CA. */
            if (!s_ca_cert_uploaded) {
                if (!modem.tlsWriteCredential(false, LETS_ENCRYPT_CA_CERT_SLOT, ISRG_ROOT_X1_PEM)) {
                    ESP_LOGE(TAG, "tlsWriteCredential (CA cert) failed");
                    return ESP_FAIL;
                }
                s_ca_cert_uploaded = true;
            }
            if (!modem.tlsConfigProfile(tls_profile, WALTER_MODEM_TLS_VALIDATION_CA, WALTER_MODEM_TLS_VERSION_12,
                                         LETS_ENCRYPT_CA_CERT_SLOT)) {
                ESP_LOGE(TAG, "tlsConfigProfile failed - if your broker's certificate isn't signed by "
                              "Let's Encrypt, enable 'Allow insecure TLS' instead");
                return ESP_FAIL;
            }
        }
    }

    if (!modem.mqttConfig(client_id, cfg->username, cfg->password, cfg->use_tls ? tls_profile : 0)) {
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
