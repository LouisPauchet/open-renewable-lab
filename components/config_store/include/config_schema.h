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
    uint8_t gain;            /* I2C_DEVICE_TYPE_ADS111X only: raw 3-bit PGA select (0-7, see
                              * ads111x.c's gain table) - ignored by other device types */
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

    /* Linear calibration applied to every raw sample before it reaches
     * the aggregator (so raw/mean/min/max/stddev all reflect the
     * calibrated value): calibrated = calibration_a * raw + calibration_b.
     * Defaults (1.0, 0.0) are a no-op. Typical use: correcting a voltage
     * divider's ratio, or a two-point sensor calibration. */
    double calibration_a;
    double calibration_b;

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
    char topic_prefix[64]; /* published topics = "<topic_prefix>/<device_id>/<variable name>" (unless flat_telemetry) */

    /* When true, publishes go to the literal topic_prefix (no
     * /<device_id>/<name> suffix) with flat "<name>_<aggregate>" JSON
     * keys instead of {"mean":...,"stddev":...} - the shape platforms
     * like ThingsBoard/AWS IoT expect from their single fixed device
     * telemetry topic. When false (default), the original per-variable
     * topic + generic JSON shape is used. */
    bool flat_telemetry;

    /* When true, publishes are buffered instead of sent immediately;
     * the connection is opened, the buffer drained, and the connection
     * closed again once every batch_interval_ms - trading latency for
     * radio-on time (mainly useful with the cellular backend). When
     * false (default), the existing always-connected/immediate-publish
     * behavior is used. */
    bool batch_enabled;
    uint32_t batch_interval_ms;
} mqtt_settings_t;

typedef struct {
    /* GNSS position reporting - only meaningful (and only ever
     * attempted) when net.transport == TRANSPORT_CELLULAR, since
     * Walter's GNSS is provided by the cellular modem chip itself.
     * Dynamically toggleable without a reboot, unlike transport.
     *
     * Same sample/log-interval + aggregate-mask model as a regular
     * variable_config_t below: latitude, longitude, elevation, and
     * horizontal precision are each a "variable" of their own, fed by
     * one fix per sample_interval_ms into a Welford accumulator
     * (aggregator_t, see aggregator.h), finalized into
     * raw/mean/min/max/stddev (per aggregate_mask) and logged/published
     * every log_interval_ms - see gnss_position.c. */
    bool enabled;
    uint32_t sample_interval_ms; /* how often to attempt a GPS fix */
    uint32_t log_interval_ms;    /* how often the fixes since the last log are aggregated and logged/published; must be >= sample_interval_ms */
    uint8_t aggregate_mask;      /* bitwise OR of aggregate_mask_t, applied independently to each of the four fields above */
} position_settings_t;

typedef enum {
    BATTERY_CHEM_LI_ION = 0,   /* also covers LiFePO4 - the LTC4015's VBAT ADC
                                * front-end uses the same "lithium-based
                                * chemistries" scale factor for both per its
                                * datasheet; only lead-acid differs. */
    BATTERY_CHEM_LIFEPO4 = 1,
    BATTERY_CHEM_LEAD_ACID = 2,
} battery_chemistry_t;

typedef struct {
    /* Onboard LTC4015 battery charger/monitor (Walter Feels only).
     * chemistry/cell_count determine the VBAT register's voltage scale
     * factor - set these to match your actual battery, they're a
     * per-deployment choice, not a fixed board constant. See
     * ltc4015.c for the scale-factor math and its sourcing. */
    battery_chemistry_t chemistry;
    uint8_t cell_count;

    /* Independent of the above: periodically publish battery voltage
     * over MQTT, the same enabled/interval_ms shape as
     * position_settings_t above - a dedicated toggle rather than
     * requiring a manually-added Variable (I2C/LTC4015/VBAT) for
     * something this common. See battery_monitor.c. */
    bool enabled;
    uint32_t interval_ms;

    /* If true, ignore interval_ms and instead report the latest
     * reading alongside every regular MQTT publish (each non-batch
     * publish, or each batch transmit window) rather than on its own
     * independent timer - useful so battery voltage always accompanies
     * the rest of the telemetry instead of drifting in and out of sync
     * with it. The sensor is still polled on a short fixed cadence
     * internally so the reading handed to each publish is fresh - see
     * battery_monitor.c. */
    bool sync_with_mqtt;
} battery_settings_t;

typedef enum {
    /* One row per (variable, aggregate-finalize-event) - the original
     * format. Simple, never needs to wait/align across variables, but
     * one column ("value") shared by every variable/aggregate, so
     * every row repeats the variable name/aggregate type as text. */
    SD_LOG_FORMAT_LONG = 0,

    /* One row per "scan" (see sd_logger.c) with one column per
     * variable+aggregate, grouped into a separate file per distinct
     * log_interval_ms - variables sharing an interval land in the same
     * file/row, like a Campbell Scientific datalogger's output tables.
     * Column names bake in the unit (e.g. "PV_Voltage_mean_V"); a
     * single "# station_name=...,device_id=...,interval_ms=..." comment
     * line stands in for TOA5's file-info line. */
    SD_LOG_FORMAT_WIDE_SIMPLE = 1,

    /* Same row/column layout and file grouping as WIDE_SIMPLE, but the
     * header instead matches Campbell Scientific's TOA5 format: a
     * quoted "TOA5",station,model,... file-info line, quoted field
     * names (unit NOT baked into the name), a units row, and a
     * process/aggregate-type row (Avg/Max/Min/Std/Smp) - importable by
     * TOA5-aware tools (Loggernet, common R/Python TOA5 readers).
     * TIMESTAMP is a quoted "YYYY-MM-DD HH:MM:SS" string per TOA5
     * convention rather than a raw unix timestamp. */
    SD_LOG_FORMAT_WIDE_TOA5 = 2,
} sd_log_format_t;

typedef struct {
    sd_log_format_t log_format;

    /* Free-form station identity, used in WIDE_SIMPLE's comment line
     * and as TOA5's file-info "StationName" field. Falls back to the
     * device_id if left blank. Purely a data-file label - has no
     * effect on device behavior. */
    char station_name[32];
} sd_settings_t;

typedef struct {
    uint32_t schema_version;

    /* combined "<16-char salt hex><64-char sha256 hex>" + NUL */
    char portal_password_hash[96];

    net_settings_t net;
    mqtt_settings_t mqtt;
    position_settings_t position;
    battery_settings_t battery;
    sd_settings_t sd;

    variable_config_t variables[MAX_VARIABLES];
    uint8_t variable_count;

    uint32_t generation; /* bumped on every mutation; other tasks poll to detect config changes */
} device_config_t;

#ifdef __cplusplus
}
#endif
