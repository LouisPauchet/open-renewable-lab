#pragma once

/*
 * Walter Feels carrier-board pin map.
 *
 * *** EVERY VALUE BELOW IS AN UNVERIFIED PLACEHOLDER (BOARD_PIN_NOT_SET). ***
 *
 * The Walter Feels schematic (QuickSpot/walter-hardware, walter-feels/
 * schematic/schematic-v2.6.pdf) could not be parsed automatically while
 * this firmware was written. Before first flash, check every pin below
 * against the schematic (or board silkscreen) and replace the placeholder
 * with the real GPIO_NUM_xx. Guessing is not safe here: an incorrect
 * value could drive a GPIO that is actually a power rail, the modem UART,
 * or a PSRAM/flash strapping pin.
 *
 * Each subsystem's init function calls board_pin_is_set() on the pins it
 * needs and refuses to start (logging an error) rather than driving an
 * unconfirmed pin - so it's safe to build and flash with placeholders
 * still in place; only the affected subsystem stays disabled.
 */

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#define BOARD_PIN_NOT_SET GPIO_NUM_NC

/* ---- SDI-12 bus ---- */
#define BOARD_PIN_SDI12_DATA        BOARD_PIN_NOT_SET /* TODO: single half-duplex data line */
#define BOARD_PIN_SDI12_DIR_ENABLE  BOARD_PIN_NOT_SET /* TODO: transceiver TX/RX direction-enable, if the carrier's SDI-12 transceiver needs one; leave NOT_SET if it auto-directs */
#define BOARD_PIN_SDI12_BUS_POWER   BOARD_PIN_NOT_SET /* TODO: switched rail powering SDI-12 sensors, if gated separately from BOARD_PIN_SENSOR_PWR_EN */

/* ---- I2C connector (external sensors, e.g. ADC boards) ---- */
#define BOARD_I2C_PORT              (-1) /* -1 = let the I2C driver auto-assign a free port */
#define BOARD_PIN_I2C_SDA           BOARD_PIN_NOT_SET /* TODO */
#define BOARD_PIN_I2C_SCL           BOARD_PIN_NOT_SET /* TODO */
#define BOARD_I2C_CLOCK_HZ          100000

/* ---- microSD slot ----
 * TODO: confirm whether the Walter Feels microSD slot is wired for SDMMC
 * or SPI mode. Pins below assume SPI (the more common wiring on carrier
 * boards); if it's actually SDMMC, update sd_logger's mount call and this
 * pin set accordingly.
 */
#define BOARD_PIN_SD_SPI_MISO       BOARD_PIN_NOT_SET /* TODO */
#define BOARD_PIN_SD_SPI_MOSI       BOARD_PIN_NOT_SET /* TODO */
#define BOARD_PIN_SD_SPI_SCLK       BOARD_PIN_NOT_SET /* TODO */
#define BOARD_PIN_SD_SPI_CS         BOARD_PIN_NOT_SET /* TODO */
#define BOARD_PIN_SD_CARD_DETECT    BOARD_PIN_NOT_SET /* TODO: optional, leave NOT_SET if unavailable */
#define BOARD_SD_SPI_HOST           SPI2_HOST

/* ---- Switched sensor power rail ---- */
#define BOARD_PIN_SENSOR_PWR_EN     BOARD_PIN_NOT_SET /* TODO: enable pin for external sensor power (I2C + SDI-12), if the carrier gates it */

/* ---- Status / UX ---- */
#define BOARD_PIN_STATUS_LED        BOARD_PIN_NOT_SET /* TODO: optional */
#define BOARD_PIN_FORCE_AP_BUTTON   BOARD_PIN_NOT_SET /* TODO: optional; extension point for net_manager_force_ap_on() */

/*
 * Onboard peripherals documented for reference, not GPIO-mapped here since
 * they live on BOARD_I2C_PORT rather than needing dedicated GPIOs:
 *  - LTC4015 battery charger/monitor (typ. I2C addr 0x68 per
 *    walter_feels.md) - not wired into firmware, out of scope for v1.
 *  - Onboard temp/humidity/pressure + IMU + optional SCD30 CO2 sensor -
 *    also I2C devices; can be added as extra i2c_sensors drivers later.
 */

static inline bool board_pin_is_set(int gpio_num)
{
    return gpio_num != BOARD_PIN_NOT_SET;
}
