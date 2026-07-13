#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_SCHEMA_VERSION 1
#define MAX_VARIABLES 32

/* Default portal password shown to the student on first boot. Must be
 * changed via /api/settings/password before any real deployment. */
#define DEFAULT_PORTAL_PASSWORD "walter1234"

typedef enum {
    BUS_TYPE_SDI12 = 0,
    BUS_TYPE_I2C = 1,
} bus_type_t;

typedef enum {
    AGG_RAW    = 1 << 0, /* passthrough of the most recent raw sample */
    AGG_MEAN   = 1 << 1,
    AGG_MIN    = 1 << 2,
    AGG_MAX    = 1 << 3,
    AGG_STDDEV = 1 << 4,
} aggregate_mask_t;

#define AGG_ALL_VALID_BITS (AGG_RAW | AGG_MEAN | AGG_MIN | AGG_MAX | AGG_STDDEV)

typedef struct {
    char address;            /* SDI-12 address: '0'-'9', 'A'-'Z', 'a'-'z' */
    uint8_t parameter_index; /* index into the values returned by aD0!..aD9! */
} sdi12_addr_t;

typedef struct {
    uint8_t i2c_addr;        /* 7-bit I2C address */
    uint8_t device_type;     /* looked up in the i2c_sensor_registry */
    uint8_t channel_index;   /* e.g. ADC channel, or sub-value index for multi-value devices */
} i2c_addr_t;

typedef struct {
    uint16_t id;              /* stable id, independent of array index - used by REST CRUD */
    char name[32];            /* MQTT JSON key / CSV column header, must be unique */
    bus_type_t bus_type;
    union {
        sdi12_addr_t sdi12;
        i2c_addr_t i2c;
    } addr;
    char unit[16];             /* free-form unit label, e.g. "degC", "%RH" - not interpreted by firmware */
    uint32_t sample_interval_ms; /* how often a raw sample is taken */
    uint32_t log_interval_ms;    /* how often aggregates are flushed to SD/MQTT; must be >= sample_interval_ms */
    uint8_t aggregate_mask;      /* bitwise OR of aggregate_mask_t */
    bool enabled;
} variable_config_t;

typedef enum {
    TRANSPORT_UNCONFIGURED = 0,
    TRANSPORT_WIFI = 1,
    TRANSPORT_CELLULAR = 2,
} net_transport_t;

typedef struct {
    net_transport_t transport;
    char wifi_sta_ssid[33];
    char wifi_sta_password[64];
    char cellular_apn[32];
    char cellular_pin[8]; /* optional SIM PIN, empty if not needed */
} net_settings_t;

typedef struct {
    bool enabled;          /* MQTT stays fully inert until true */
    char host[64];
    uint16_t port;
    bool use_tls;
    bool tls_allow_insecure; /* skip server-cert validation - lab-friendly default for self-signed brokers */
    char client_id[32];
    char username[32];
    char password[64];
    char topic_prefix[64]; /* published topics = "<topic_prefix>/<variable name>" */
} mqtt_settings_t;

typedef struct {
    uint32_t schema_version;

    /* combined "<16-char salt hex><64-char sha256 hex>" + NUL */
    char portal_password_hash[96];

    net_settings_t net;
    mqtt_settings_t mqtt;

    variable_config_t variables[MAX_VARIABLES];
    uint8_t variable_count;

    uint32_t generation; /* bumped on every mutation; other tasks poll to detect config changes */
} device_config_t;

#ifdef __cplusplus
}
#endif
