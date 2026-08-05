/* Analog Devices/Linear Technology LTC4015 battery charger controller
 * with telemetry (Walter Feels onboard battery monitor - lives on the
 * EXTERNAL I2C connector bus per board_pins.h, fixed I2C address
 * 0x68, unlike HDC1080/LPS22HB which are on the separate onboard
 * sensor bus).
 *
 * Telemetry registers are 16-bit, read via SMBus read-word (write the
 * sub-address, then read 2 bytes low-byte-first). Register addresses
 * and scale factors below are sourced from the LTC4015 datasheet
 * (4015fb) "Measurement System" register table; see channel_index==
 * LTC4015_CHANNEL_VBAT for the one genuinely deployment-specific
 * detail (battery chemistry/cell count, configured via config_store's
 * battery_settings_t / the portal's "Battery monitor" section, NOT a
 * compile-time constant, since different lab stations may use
 * different battery types).
 *
 * Flagged uncertainty (see the research this was written against):
 * VBAT/VIN/DIE_TEMP are treated as unsigned 16-bit here - strongly
 * implied by the datasheet (they're physically non-negative
 * quantities) but not found as an explicitly-quoted "unsigned"
 * statement. IBAT *is* explicitly documented as two's-complement
 * signed (negative = discharging). If VBAT/temperature readings look
 * wrong, this is the first thing to re-check against the datasheet's
 * register description table (sub-addresses 0x3A-0x3F). */

#include "config_store.h"
#include "i2c_bus.h"

#define LTC4015_REG_VBAT 0x3A
#define LTC4015_REG_VIN 0x3B
#define LTC4015_REG_IBAT 0x3D
#define LTC4015_REG_DIE_TEMP 0x3F

#define LTC4015_REG_CONFIG_BITS 0x14
#define LTC4015_CONFIG_FORCE_MEAS_SYS_ON (1u << 4)

/* Battery-current sense resistor on the Walter Feels schematic - a
 * fixed board constant (unlike chemistry/cell count, which vary by
 * deployment and are configurable). */
#define LTC4015_RSNSB_OHMS 0.004

/* Base ADC LSB (3.6V / 65535 codes) scaled per the datasheet's
 * per-register multipliers. VBAT's multiplier is chemistry-dependent:
 * x7/2 for lithium-based chemistries (Li-Ion and LiFePO4 use the same
 * scale factor per the datasheet - only lead-acid differs), x7/3 for
 * lead-acid. */
#define LTC4015_ADC_BASE_LSB_V 54.932479e-6
#define LTC4015_VIN_LSB_V (LTC4015_ADC_BASE_LSB_V * 30.0)
#define LTC4015_IBAT_LSB_V 1.46487e-6

static esp_err_t ltc4015_read_reg16(uint8_t i2c_addr, uint8_t reg, uint16_t *out_raw)
{
    uint8_t subaddr = reg;
    uint8_t buf[2];
    esp_err_t err = i2c_bus_write_read(i2c_addr, &subaddr, 1, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        return err;
    }
    *out_raw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8); /* SMBus word: low byte first */
    return ESP_OK;
}

static esp_err_t ltc4015_write_reg16(uint8_t i2c_addr, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value & 0xFF), (uint8_t)(value >> 8) }; /* SMBus word: low byte first */
    return i2c_bus_write(i2c_addr, buf, sizeof(buf), 100);
}

/* By default the LTC4015's A/D measurement system only runs while
 * input power is present (VIN > VBAT) - telemetry registers (VBAT
 * included) sit at their power-on-reset value of 0 the rest of the
 * time, confirmed against the datasheet (4015fb, CONFIG_BITS
 * sub-address 0x14) and matching a real "battery voltage always reads
 * 0 V" report on real hardware with a genuine battery connected but no
 * active charging input. force_meas_sys_on (bit 4) makes the ADC run
 * continuously regardless of input power - the right tradeoff for a
 * monitoring application (a small increase in the LTC4015's own
 * battery-only quiescent draw, not the sensor node's overall power
 * budget). Read-modify-write so as not to disturb other CONFIG_BITS
 * (e.g. suspend_charger, mppt_en_i2c) this driver never otherwise
 * touches. Checked once per boot, not on every read - if the LTC4015
 * itself loses and regains power independently of the ESP32 (VIN and
 * battery both briefly absent), this bit would reset and telemetry
 * would go back to reading 0 until the next reboot; a full LTC4015
 * power-cycle while the ESP32 stays up is enough of an edge case that
 * re-checking on every single read isn't worth doubling I2C traffic
 * for. */
static bool s_meas_sys_checked;
static void ensure_measurement_system_enabled(uint8_t i2c_addr)
{
    if (s_meas_sys_checked) {
        return;
    }
    s_meas_sys_checked = true;

    uint16_t config;
    if (ltc4015_read_reg16(i2c_addr, LTC4015_REG_CONFIG_BITS, &config) != ESP_OK) {
        return; /* LTC4015 not present/reachable - nothing to fix, the actual telemetry read below will fail too */
    }
    if (config & LTC4015_CONFIG_FORCE_MEAS_SYS_ON) {
        return; /* already set */
    }
    ltc4015_write_reg16(i2c_addr, LTC4015_REG_CONFIG_BITS, config | LTC4015_CONFIG_FORCE_MEAS_SYS_ON);
}

esp_err_t ltc4015_read_channel(uint8_t i2c_addr, uint8_t channel, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    ensure_measurement_system_enabled(i2c_addr);

    uint16_t raw;
    esp_err_t err;

    switch (channel) {
        case 0: { /* VBAT - battery voltage */
            err = ltc4015_read_reg16(i2c_addr, LTC4015_REG_VBAT, &raw);
            if (err != ESP_OK) {
                return err;
            }
            battery_settings_t bs;
            config_store_get_battery_settings(&bs);
            double lsb = (bs.chemistry == BATTERY_CHEM_LEAD_ACID) ? (LTC4015_ADC_BASE_LSB_V * 7.0 / 3.0)
                                                                    : (LTC4015_ADC_BASE_LSB_V * 7.0 / 2.0);
            uint8_t cells = bs.cell_count > 0 ? bs.cell_count : 1;
            *out_value = (double)raw * lsb * cells;
            return ESP_OK;
        }
        case 1: { /* VIN - input voltage */
            err = ltc4015_read_reg16(i2c_addr, LTC4015_REG_VIN, &raw);
            if (err != ESP_OK) {
                return err;
            }
            *out_value = (double)raw * LTC4015_VIN_LSB_V;
            return ESP_OK;
        }
        case 2: { /* IBAT - battery current, signed (+ = charging, - = discharging) */
            err = ltc4015_read_reg16(i2c_addr, LTC4015_REG_IBAT, &raw);
            if (err != ESP_OK) {
                return err;
            }
            *out_value = (double)(int16_t)raw * LTC4015_IBAT_LSB_V / LTC4015_RSNSB_OHMS;
            return ESP_OK;
        }
        case 3: { /* DIE_TEMP - charger die temperature */
            err = ltc4015_read_reg16(i2c_addr, LTC4015_REG_DIE_TEMP, &raw);
            if (err != ESP_OK) {
                return err;
            }
            *out_value = ((double)raw - 12010.0) / 45.6;
            return ESP_OK;
        }
        default:
            return ESP_ERR_INVALID_ARG;
    }
}
