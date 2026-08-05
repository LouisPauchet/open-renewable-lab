#include "config_store.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

static const char *TAG = "config_store";
static const char *NVS_NAMESPACE = "cfg";
static const char *NVS_KEY_BLOB = "device_json";

static device_config_t s_config;
static SemaphoreHandle_t s_mutex;
static uint16_t s_next_var_id = 1;

/* ---------------------------------------------------------------------
 * small helpers
 * ------------------------------------------------------------------- */

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out_hex)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out_hex[2 * i] = hex[bytes[i] >> 4];
        out_hex[2 * i + 1] = hex[bytes[i] & 0x0F];
    }
    out_hex[2 * len] = '\0';
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) {
            return false;
        }
        out[i] = (uint8_t)byte;
    }
    return true;
}

/* mbedtls's legacy per-algorithm API (mbedtls/sha256.h) has been
 * removed in this mbedtls version - mbedtls/include only exposes a
 * "private" subtree now. PSA Crypto (psa/crypto.h) is the only public
 * hashing API left; psa_crypto_init() is called once in
 * config_store_init(). */
static void hash_with_salt(const char *plaintext, const uint8_t salt[8], uint8_t out_hash[32])
{
    uint8_t buf[8 + 128];
    size_t plaintext_len = strlen(plaintext);
    if (plaintext_len > sizeof(buf) - 8) {
        plaintext_len = sizeof(buf) - 8; /* defensive clamp; passwords are always far shorter */
    }
    memcpy(buf, salt, 8);
    memcpy(buf + 8, plaintext, plaintext_len);

    size_t hash_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, buf, 8 + plaintext_len, out_hash, 32, &hash_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compute failed: %d", (int)status);
        memset(out_hash, 0, 32); /* should never happen; avoid leaving the hash uninitialized */
    }
}

static int json_get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (j && cJSON_IsNumber(j)) ? (int)j->valuedouble : def;
}

static double json_get_double(const cJSON *obj, const char *key, double def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (j && cJSON_IsNumber(j)) ? j->valuedouble : def;
}

static bool json_get_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (j && cJSON_IsBool(j)) ? cJSON_IsTrue(j) : def;
}

static void json_get_str(const cJSON *obj, const char *key, char *dst, size_t dst_size, const char *def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    copy_str(dst, dst_size, (j && cJSON_IsString(j)) ? j->valuestring : def);
}

/* ---------------------------------------------------------------------
 * defaults
 * ------------------------------------------------------------------- */

static void config_set_defaults(device_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->schema_version = CONFIG_SCHEMA_VERSION;

    c->net.transport = TRANSPORT_UNCONFIGURED;
    /* Deliberately blank, not a guessed default: most IoT/M2M SIMs ship
     * with PIN lock disabled entirely, and unlockSIM() with an
     * unneeded/wrong PIN risks decrementing the SIM's (usually 3-try)
     * PIN retry counter toward a PUK lock - real hardware testing
     * found this out the hard way with a "1111" default. Leave this
     * blank unless a student's specific SIM actually needs one. */

    c->mqtt.enabled = false;
    c->mqtt.port = 1883;
    copy_str(c->mqtt.topic_prefix, sizeof(c->mqtt.topic_prefix), "walter");
    c->mqtt.flat_telemetry = false;
    c->mqtt.batch_enabled = false;
    c->mqtt.batch_interval_ms = 30 * 60 * 1000; /* 30 min */

    c->position.enabled = false;
    c->position.interval_ms = 10 * 60 * 1000; /* 10 min */

    c->battery.chemistry = BATTERY_CHEM_LI_ION;
    c->battery.cell_count = 1;
    c->battery.enabled = false;
    c->battery.interval_ms = 10 * 60 * 1000; /* 10 min */

    c->sd.log_format = SD_LOG_FORMAT_LONG;

    c->variable_count = 0;
    c->generation = 0;

    uint8_t salt[8];
    esp_fill_random(salt, sizeof(salt));
    uint8_t hash[32];
    hash_with_salt(DEFAULT_PORTAL_PASSWORD, salt, hash);

    char salt_hex[17];
    char hash_hex[65];
    bytes_to_hex(salt, sizeof(salt), salt_hex);
    bytes_to_hex(hash, sizeof(hash), hash_hex);
    snprintf(c->portal_password_hash, sizeof(c->portal_password_hash), "%s%s", salt_hex, hash_hex);
}

/* ---------------------------------------------------------------------
 * JSON (de)serialization
 * ------------------------------------------------------------------- */

cJSON *config_store_variable_to_json(const variable_config_t *v)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", v->id);
    cJSON_AddStringToObject(o, "name", v->name);
    cJSON_AddNumberToObject(o, "bus_type", v->bus_type);

    cJSON *addr = cJSON_AddObjectToObject(o, "addr");
    if (v->bus_type == BUS_TYPE_SDI12) {
        char addr_str[2] = { v->addr.sdi12.address, '\0' };
        cJSON_AddStringToObject(addr, "address", addr_str);
        cJSON_AddNumberToObject(addr, "parameter_index", v->addr.sdi12.parameter_index);
    } else {
        cJSON_AddNumberToObject(addr, "i2c_addr", v->addr.i2c.i2c_addr);
        cJSON_AddNumberToObject(addr, "device_type", v->addr.i2c.device_type);
        cJSON_AddNumberToObject(addr, "channel_index", v->addr.i2c.channel_index);
        cJSON_AddNumberToObject(addr, "gain", v->addr.i2c.gain);
    }

    cJSON_AddStringToObject(o, "unit", v->unit);
    cJSON_AddNumberToObject(o, "sample_interval_ms", v->sample_interval_ms);
    cJSON_AddNumberToObject(o, "log_interval_ms", v->log_interval_ms);
    cJSON_AddNumberToObject(o, "aggregate_mask", v->aggregate_mask);
    cJSON_AddNumberToObject(o, "calibration_a", v->calibration_a);
    cJSON_AddNumberToObject(o, "calibration_b", v->calibration_b);
    cJSON_AddBoolToObject(o, "enabled", v->enabled);
    return o;
}

bool config_store_variable_from_json(const cJSON *o, variable_config_t *v)
{
    memset(v, 0, sizeof(*v));
    v->id = (uint16_t)json_get_int(o, "id", 0);
    json_get_str(o, "name", v->name, sizeof(v->name), "");
    v->bus_type = (bus_type_t)json_get_int(o, "bus_type", BUS_TYPE_SDI12);

    const cJSON *addr = cJSON_GetObjectItemCaseSensitive(o, "addr");
    if (v->bus_type == BUS_TYPE_SDI12) {
        char addr_str[2] = "0";
        json_get_str(addr, "address", addr_str, sizeof(addr_str), "0");
        v->addr.sdi12.address = addr_str[0];
        v->addr.sdi12.parameter_index = (uint8_t)json_get_int(addr, "parameter_index", 0);
    } else {
        v->addr.i2c.i2c_addr = (uint8_t)json_get_int(addr, "i2c_addr", 0);
        v->addr.i2c.device_type = (uint8_t)json_get_int(addr, "device_type", 0);
        v->addr.i2c.channel_index = (uint8_t)json_get_int(addr, "channel_index", 0);
        /* default matches this driver's old hardcoded PGA, for configs saved before gain was configurable */
        v->addr.i2c.gain = (uint8_t)json_get_int(addr, "gain", 1);
    }

    json_get_str(o, "unit", v->unit, sizeof(v->unit), "");
    v->sample_interval_ms = (uint32_t)json_get_int(o, "sample_interval_ms", 60000);
    v->log_interval_ms = (uint32_t)json_get_int(o, "log_interval_ms", 300000);
    v->aggregate_mask = (uint8_t)json_get_int(o, "aggregate_mask", AGG_RAW) & AGG_ALL_VALID_BITS;
    v->calibration_a = json_get_double(o, "calibration_a", 1.0);
    v->calibration_b = json_get_double(o, "calibration_b", 0.0);
    v->enabled = json_get_bool(o, "enabled", true);
    return true;
}

cJSON *config_store_to_json(const device_config_t *c)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema_version", c->schema_version);
    cJSON_AddStringToObject(root, "portal_password_hash", c->portal_password_hash);

    cJSON *net = cJSON_AddObjectToObject(root, "net");
    cJSON_AddNumberToObject(net, "transport", c->net.transport);
    cJSON_AddStringToObject(net, "wifi_sta_ssid", c->net.wifi_sta_ssid);
    cJSON_AddStringToObject(net, "wifi_sta_password", c->net.wifi_sta_password);
    cJSON_AddStringToObject(net, "cellular_apn", c->net.cellular_apn);
    cJSON_AddStringToObject(net, "cellular_pin", c->net.cellular_pin);

    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", c->mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "host", c->mqtt.host);
    cJSON_AddNumberToObject(mqtt, "port", c->mqtt.port);
    cJSON_AddBoolToObject(mqtt, "use_tls", c->mqtt.use_tls);
    cJSON_AddBoolToObject(mqtt, "tls_allow_insecure", c->mqtt.tls_allow_insecure);
    cJSON_AddStringToObject(mqtt, "client_id", c->mqtt.client_id);
    cJSON_AddStringToObject(mqtt, "username", c->mqtt.username);
    cJSON_AddStringToObject(mqtt, "password", c->mqtt.password);
    cJSON_AddStringToObject(mqtt, "topic_prefix", c->mqtt.topic_prefix);
    cJSON_AddBoolToObject(mqtt, "flat_telemetry", c->mqtt.flat_telemetry);
    cJSON_AddBoolToObject(mqtt, "batch_enabled", c->mqtt.batch_enabled);
    cJSON_AddNumberToObject(mqtt, "batch_interval_ms", c->mqtt.batch_interval_ms);

    cJSON *position = cJSON_AddObjectToObject(root, "position");
    cJSON_AddBoolToObject(position, "enabled", c->position.enabled);
    cJSON_AddNumberToObject(position, "interval_ms", c->position.interval_ms);

    cJSON *battery = cJSON_AddObjectToObject(root, "battery");
    cJSON_AddNumberToObject(battery, "chemistry", c->battery.chemistry);
    cJSON_AddNumberToObject(battery, "cell_count", c->battery.cell_count);
    cJSON_AddBoolToObject(battery, "enabled", c->battery.enabled);
    cJSON_AddNumberToObject(battery, "interval_ms", c->battery.interval_ms);

    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddNumberToObject(sd, "log_format", c->sd.log_format);
    cJSON_AddStringToObject(sd, "station_name", c->sd.station_name);

    cJSON *vars = cJSON_AddArrayToObject(root, "variables");
    for (uint8_t i = 0; i < c->variable_count; i++) {
        cJSON_AddItemToArray(vars, config_store_variable_to_json(&c->variables[i]));
    }

    cJSON_AddNumberToObject(root, "generation", c->generation);
    return root;
}

bool config_store_from_json(const cJSON *root, device_config_t *c)
{
    config_set_defaults(c);

    c->schema_version = (uint32_t)json_get_int(root, "schema_version", CONFIG_SCHEMA_VERSION);
    json_get_str(root, "portal_password_hash", c->portal_password_hash, sizeof(c->portal_password_hash),
                 c->portal_password_hash);

    const cJSON *net = cJSON_GetObjectItemCaseSensitive(root, "net");
    if (net) {
        c->net.transport = (net_transport_t)json_get_int(net, "transport", TRANSPORT_UNCONFIGURED);
        json_get_str(net, "wifi_sta_ssid", c->net.wifi_sta_ssid, sizeof(c->net.wifi_sta_ssid), "");
        json_get_str(net, "wifi_sta_password", c->net.wifi_sta_password, sizeof(c->net.wifi_sta_password), "");
        json_get_str(net, "cellular_apn", c->net.cellular_apn, sizeof(c->net.cellular_apn), "");
        json_get_str(net, "cellular_pin", c->net.cellular_pin, sizeof(c->net.cellular_pin), "");
    }

    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (mqtt) {
        c->mqtt.enabled = json_get_bool(mqtt, "enabled", false);
        json_get_str(mqtt, "host", c->mqtt.host, sizeof(c->mqtt.host), "");
        c->mqtt.port = (uint16_t)json_get_int(mqtt, "port", 1883);
        c->mqtt.use_tls = json_get_bool(mqtt, "use_tls", false);
        c->mqtt.tls_allow_insecure = json_get_bool(mqtt, "tls_allow_insecure", false);
        json_get_str(mqtt, "client_id", c->mqtt.client_id, sizeof(c->mqtt.client_id), "");
        json_get_str(mqtt, "username", c->mqtt.username, sizeof(c->mqtt.username), "");
        json_get_str(mqtt, "password", c->mqtt.password, sizeof(c->mqtt.password), "");
        json_get_str(mqtt, "topic_prefix", c->mqtt.topic_prefix, sizeof(c->mqtt.topic_prefix), "walter");
        c->mqtt.flat_telemetry = json_get_bool(mqtt, "flat_telemetry", false);
        c->mqtt.batch_enabled = json_get_bool(mqtt, "batch_enabled", false);
        c->mqtt.batch_interval_ms = (uint32_t)json_get_int(mqtt, "batch_interval_ms", (int)c->mqtt.batch_interval_ms);
    }

    const cJSON *position = cJSON_GetObjectItemCaseSensitive(root, "position");
    if (position) {
        c->position.enabled = json_get_bool(position, "enabled", false);
        c->position.interval_ms = (uint32_t)json_get_int(position, "interval_ms", (int)c->position.interval_ms);
    }

    const cJSON *battery = cJSON_GetObjectItemCaseSensitive(root, "battery");
    if (battery) {
        c->battery.chemistry = (battery_chemistry_t)json_get_int(battery, "chemistry", c->battery.chemistry);
        c->battery.cell_count = (uint8_t)json_get_int(battery, "cell_count", c->battery.cell_count);
        c->battery.enabled = json_get_bool(battery, "enabled", c->battery.enabled);
        c->battery.interval_ms = (uint32_t)json_get_int(battery, "interval_ms", (int)c->battery.interval_ms);
    }

    const cJSON *sd = cJSON_GetObjectItemCaseSensitive(root, "sd");
    if (sd) {
        c->sd.log_format = (sd_log_format_t)json_get_int(sd, "log_format", c->sd.log_format);
        json_get_str(sd, "station_name", c->sd.station_name, sizeof(c->sd.station_name), c->sd.station_name);
    }

    const cJSON *vars = cJSON_GetObjectItemCaseSensitive(root, "variables");
    c->variable_count = 0;
    if (cJSON_IsArray(vars)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, vars)
        {
            if (c->variable_count >= MAX_VARIABLES) {
                ESP_LOGW(TAG, "stored config has more than MAX_VARIABLES=%d variables, truncating", MAX_VARIABLES);
                break;
            }
            variable_config_t v;
            if (config_store_variable_from_json(item, &v)) {
                c->variables[c->variable_count++] = v;
            }
        }
    }

    c->generation = (uint32_t)json_get_int(root, "generation", 0);
    return true;
}

/* ---------------------------------------------------------------------
 * NVS blob I/O
 * ------------------------------------------------------------------- */

static esp_err_t nvs_load_blob(char **out_str)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = 0;
    err = nvs_get_str(h, NVS_KEY_BLOB, NULL, &len);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    char *buf = malloc(len);
    if (!buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(h, NVS_KEY_BLOB, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    *out_str = buf;
    return ESP_OK;
}

static esp_err_t nvs_save_blob(const char *str)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, NVS_KEY_BLOB, str);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_store_factory_reset(void)
{
    /* Scoped to just this namespace, not a full nvs_flash_erase() -
     * that would also wipe other components' NVS state (e.g. PHY
     * calibration data) unrelated to this app's own settings, for no
     * benefit (WiFi STA credentials aren't stored in the WiFi driver's
     * own NVS blob here - net_manager.c sources them from this same
     * "cfg" namespace via config_store_get_net_settings() and calls
     * esp_wifi_set_config() explicitly every boot). */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Caller must hold s_mutex. */
static esp_err_t save_locked(void)
{
    cJSON *root = config_store_to_json(&s_config);
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_save_blob(str);
    cJSON_free(str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist config: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---------------------------------------------------------------------
 * public API
 * ------------------------------------------------------------------- */

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    psa_status_t psa_status = psa_crypto_init();
    if (psa_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)psa_status);
        return ESP_FAIL;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    bool loaded_ok = false;
    char *blob = NULL;
    if (nvs_load_blob(&blob) == ESP_OK) {
        cJSON *root = cJSON_Parse(blob);
        if (root) {
            loaded_ok = config_store_from_json(root, &s_config);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "stored config blob failed to parse as JSON");
        }
        free(blob);
    }

    if (loaded_ok && s_config.schema_version != CONFIG_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "config schema v%" PRIu32 " != current v%d - no migrations defined yet, using as-is",
                 s_config.schema_version, CONFIG_SCHEMA_VERSION);
        s_config.schema_version = CONFIG_SCHEMA_VERSION;
    }

    if (!loaded_ok) {
        ESP_LOGW(TAG, "no valid stored config found, applying safe defaults");
        config_set_defaults(&s_config);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        save_locked();
        xSemaphoreGive(s_mutex);

        ESP_LOGW(TAG, "*** default portal password is \"%s\" - change it via the web portal before deployment ***",
                 DEFAULT_PORTAL_PASSWORD);
    }

    s_next_var_id = 1;
    for (uint8_t i = 0; i < s_config.variable_count; i++) {
        if (s_config.variables[i].id >= s_next_var_id) {
            s_next_var_id = s_config.variables[i].id + 1;
        }
    }

    return ESP_OK;
}

esp_err_t config_store_save(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

uint32_t config_store_get_generation(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t gen = s_config.generation;
    xSemaphoreGive(s_mutex);
    return gen;
}

void config_store_get_snapshot(device_config_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_mutex);
}

esp_err_t config_store_replace_all(const device_config_t *new_config)
{
    if (!new_config) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config = *new_config;
    s_config.schema_version = CONFIG_SCHEMA_VERSION;
    if (s_config.variable_count > MAX_VARIABLES) {
        s_config.variable_count = MAX_VARIABLES;
    }
    s_config.generation++;

    s_next_var_id = 1;
    for (uint8_t i = 0; i < s_config.variable_count; i++) {
        if (s_config.variables[i].id >= s_next_var_id) {
            s_next_var_id = s_config.variables[i].id + 1;
        }
    }

    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

size_t config_store_get_variables(variable_config_t *out, size_t max_count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = s_config.variable_count < max_count ? s_config.variable_count : max_count;
    memcpy(out, s_config.variables, n * sizeof(variable_config_t));
    xSemaphoreGive(s_mutex);
    return n;
}

bool config_store_get_variable(uint16_t id, variable_config_t *out)
{
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < s_config.variable_count; i++) {
        if (s_config.variables[i].id == id) {
            *out = s_config.variables[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
}

esp_err_t config_store_add_variable(const variable_config_t *var, uint16_t *out_id)
{
    if (!var || !out_id) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err;
    if (s_config.variable_count >= MAX_VARIABLES) {
        err = ESP_ERR_NO_MEM;
    } else {
        variable_config_t v = *var;
        v.id = s_next_var_id++;
        s_config.variables[s_config.variable_count++] = v;
        s_config.generation++;
        err = save_locked();
        if (err == ESP_OK) {
            *out_id = v.id;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t config_store_update_variable(uint16_t id, const variable_config_t *var)
{
    if (!var) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (uint8_t i = 0; i < s_config.variable_count; i++) {
        if (s_config.variables[i].id == id) {
            variable_config_t v = *var;
            v.id = id;
            s_config.variables[i] = v;
            s_config.generation++;
            err = save_locked();
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t config_store_delete_variable(uint16_t id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (uint8_t i = 0; i < s_config.variable_count; i++) {
        if (s_config.variables[i].id == id) {
            for (uint8_t j = i; j < s_config.variable_count - 1; j++) {
                s_config.variables[j] = s_config.variables[j + 1];
            }
            s_config.variable_count--;
            s_config.generation++;
            err = save_locked();
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

void config_store_get_mqtt_settings(mqtt_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_config.mqtt;
    xSemaphoreGive(s_mutex);
}

esp_err_t config_store_set_mqtt_settings(const mqtt_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.mqtt = *settings;
    s_config.generation++;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

void config_store_get_net_settings(net_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_config.net;
    xSemaphoreGive(s_mutex);
}

esp_err_t config_store_set_net_settings(const net_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.net = *settings;
    s_config.generation++;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

void config_store_get_position_settings(position_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_config.position;
    xSemaphoreGive(s_mutex);
}

esp_err_t config_store_set_position_settings(const position_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.position = *settings;
    s_config.generation++;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

void config_store_get_battery_settings(battery_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_config.battery;
    xSemaphoreGive(s_mutex);
}

esp_err_t config_store_set_battery_settings(const battery_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.battery = *settings;
    s_config.generation++;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

void config_store_get_sd_settings(sd_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_config.sd;
    xSemaphoreGive(s_mutex);
}

esp_err_t config_store_set_sd_settings(const sd_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.sd = *settings;
    s_config.generation++;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t config_store_set_portal_password(const char *plaintext_password)
{
    if (!plaintext_password || strlen(plaintext_password) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t salt[8];
    esp_fill_random(salt, sizeof(salt));
    uint8_t hash[32];
    hash_with_salt(plaintext_password, salt, hash);

    char salt_hex[17];
    char hash_hex[65];
    bytes_to_hex(salt, sizeof(salt), salt_hex);
    bytes_to_hex(hash, sizeof(hash), hash_hex);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_config.portal_password_hash, sizeof(s_config.portal_password_hash), "%s%s", salt_hex, hash_hex);
    s_config.generation++;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

bool config_store_verify_portal_password(const char *plaintext_password)
{
    if (!plaintext_password) {
        return false;
    }

    char stored[96];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy_str(stored, sizeof(stored), s_config.portal_password_hash);
    xSemaphoreGive(s_mutex);

    if (strlen(stored) != 80) {
        ESP_LOGE(TAG, "stored password hash has unexpected length, refusing login");
        return false;
    }

    char salt_hex[17];
    memcpy(salt_hex, stored, 16);
    salt_hex[16] = '\0';
    const char *stored_hash_hex = stored + 16;

    uint8_t salt[8];
    if (!hex_to_bytes(salt_hex, salt, sizeof(salt))) {
        return false;
    }

    uint8_t computed_hash[32];
    hash_with_salt(plaintext_password, salt, computed_hash);
    char computed_hash_hex[65];
    bytes_to_hex(computed_hash, sizeof(computed_hash), computed_hash_hex);

    uint8_t diff = 0;
    for (size_t i = 0; i < 64; i++) {
        diff |= (uint8_t)(computed_hash_hex[i] ^ stored_hash_hex[i]);
    }
    return diff == 0;
}
