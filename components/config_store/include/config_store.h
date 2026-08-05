#pragma once

#include "cJSON.h"
#include "config_schema.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes NVS (erasing and reinitializing if a new NVS layout version
 * is detected) and loads the device config from the "cfg" namespace. Falls
 * back to safe compiled-in defaults (unconfigured transport, MQTT
 * disabled, zero variables, default portal password) if no config is
 * stored yet or the stored blob fails to parse. Must be called once from
 * app_main before any other component touches config_store. */
esp_err_t config_store_init(void);

/* Persists the current in-RAM config to NVS immediately. All mutator
 * functions below already call this internally, so this is only needed
 * if you construct a device_config_t via config_store_replace_all(). */
esp_err_t config_store_save(void);

/* Erases all persisted settings (variables, network/cellular/MQTT/
 * position/battery/SD config, portal password) - the device falls back
 * to the same safe compiled-in defaults config_store_init() uses when
 * no config is stored at all. Does NOT touch the in-RAM config or
 * reboot; the caller must reboot for config_store_init() to actually
 * pick up the cleared state on the next boot. */
esp_err_t config_store_factory_reset(void);

/* Bumped on every successful mutation. Other tasks poll this to detect
 * that they need to reload their view of the config. */
uint32_t config_store_get_generation(void);

/* ---- Whole-config access (settings export/import) ---- */
void config_store_get_snapshot(device_config_t *out);
esp_err_t config_store_replace_all(const device_config_t *new_config);

/* ---- Variables CRUD ---- */
size_t config_store_get_variables(variable_config_t *out, size_t max_count);
bool config_store_get_variable(uint16_t id, variable_config_t *out);
/* On success, *out_id receives the newly assigned id. */
esp_err_t config_store_add_variable(const variable_config_t *var, uint16_t *out_id);
esp_err_t config_store_update_variable(uint16_t id, const variable_config_t *var);
esp_err_t config_store_delete_variable(uint16_t id);

/* ---- MQTT settings ---- */
void config_store_get_mqtt_settings(mqtt_settings_t *out);
esp_err_t config_store_set_mqtt_settings(const mqtt_settings_t *settings);

/* ---- Network settings ---- */
void config_store_get_net_settings(net_settings_t *out);
esp_err_t config_store_set_net_settings(const net_settings_t *settings);

/* ---- Position (GNSS) reporting settings ---- */
void config_store_get_position_settings(position_settings_t *out);
esp_err_t config_store_set_position_settings(const position_settings_t *settings);

/* ---- Battery monitor (onboard LTC4015, Walter Feels only) settings ---- */
void config_store_get_battery_settings(battery_settings_t *out);
esp_err_t config_store_set_battery_settings(const battery_settings_t *settings);

/* ---- SD card logging format settings ---- */
void config_store_get_sd_settings(sd_settings_t *out);
esp_err_t config_store_set_sd_settings(const sd_settings_t *settings);

/* ---- Portal password ---- */
esp_err_t config_store_set_portal_password(const char *plaintext_password);
bool config_store_verify_portal_password(const char *plaintext_password);

/* ---- JSON (de)serialization, shared with web_portal's REST API so the
 * wire format and the NVS-persisted format never drift apart ---- */
cJSON *config_store_variable_to_json(const variable_config_t *v);
bool config_store_variable_from_json(const cJSON *o, variable_config_t *out);
cJSON *config_store_to_json(const device_config_t *c);
bool config_store_from_json(const cJSON *root, device_config_t *out);

#ifdef __cplusplus
}
#endif
