#pragma once

/*
 * Walter Feels carrier-board pin map.
 *
 * SDI-12, I2C, and SD card pins below are taken directly from the
 * user-supplied Walter Feels schematic (SDI-12 transceiver sheet +
 * Walter module header pinout) and are considered confirmed. Still
 * placeholders (BOARD_PIN_NOT_SET) where nothing in the supplied
 * schematic pages showed a value: SD card-detect, status LED,
 * force-AP button. Enable-pin ACTIVE LEVELS (TX_EN/RX_EN etc.) are an
 * inference from the SN74LV1T126 datasheet (active-HIGH OE), not
 * something visible in a schematic - verify with a meter if SDI-12
 * doesn't work.
 *
 * Each subsystem's init function calls board_pin_is_set() on the pins
 * it needs and refuses to start (logging an error) rather than
 * driving an unconfirmed pin - so it's safe to build and flash with
 * remaining placeholders still in place; only the affected subsystem
 * stays disabled.
 */

#include <stdbool.h>

#include "driver/gpio.h"

#define BOARD_PIN_NOT_SET GPIO_NUM_NC

/* ---- SDI-12 bus ----
 * Unlike a typical single-wire half-duplex SDI-12 breakout, Walter
 * Feels uses two SN74LV1T126 tri-state buffers (one per direction) to
 * put SDI-12 on a shared UART-style bus (SER_TX/SER_RX) that's also
 * muxed with RS485 and RS232 transceivers via their own *_EN pins.
 * SDI-12's TX/RX enable pins are ACTIVE HIGH (SN74LV1T126 has a fixed
 * active-high OE, unlike the active-low '125 variant) - not
 * independently confirmed from the schematic image, but it's the only
 * OE polarity this specific part supports.
 */
#define BOARD_PIN_SDI12_TXD    GPIO_NUM_40 /* SER_TX - MCU output, through U5 to the bus */
#define BOARD_PIN_SDI12_RXD    GPIO_NUM_41 /* SER_RX - MCU input, from the bus through U6 */
#define BOARD_PIN_SDI12_TX_EN  GPIO_NUM_10 /* SDI12_TX_EN - drive HIGH only while transmitting */
#define BOARD_PIN_SDI12_RX_EN  GPIO_NUM_9  /* SDI12_RX_EN - left HIGH permanently (always listening) */
#define BOARD_PIN_SDI12_BUS_POWER GPIO_NUM_43 /* 12V_EN - switched rail many SDI-12 sensors need */

/* SER_TX/SER_RX are shared with RS485 and RS232 transceivers, each
 * gated by their own enable pin - these must be held LOW (disabled)
 * whenever SDI-12 is in use to avoid multiple transceivers driving the
 * same UART lines at once. sdi12_bus_init() drives these low. */
#define BOARD_PIN_RS485_TX_EN GPIO_NUM_18
#define BOARD_PIN_RS485_RX_EN GPIO_NUM_8
#define BOARD_PIN_RS232_TX_EN GPIO_NUM_17
#define BOARD_PIN_RS232_RX_EN GPIO_NUM_16

/* ---- I2C connector (external sensors, e.g. ADC boards) ---- */
#define BOARD_I2C_PORT          (-1) /* -1 = let the I2C driver auto-assign a free port */
#define BOARD_PIN_I2C_SDA       GPIO_NUM_42
#define BOARD_PIN_I2C_SCL       GPIO_NUM_2
#define BOARD_PIN_I2C_BUS_POWER GPIO_NUM_1 /* I2C_BUSPOW - switched rail for external I2C sensors */
#define BOARD_I2C_CLOCK_HZ      100000

/* ---- microSD slot ----
 * SDMMC 1-bit mode (CMD/CLK/DATA0 only - no D1-D3 on the header
 * pinout, so 4-bit mode isn't wired).
 */
#define BOARD_PIN_SD_CLK         GPIO_NUM_5
#define BOARD_PIN_SD_CMD         GPIO_NUM_6
#define BOARD_PIN_SD_D0          GPIO_NUM_4
#define BOARD_PIN_SD_CARD_DETECT BOARD_PIN_NOT_SET /* TODO: not visible on the supplied schematic pages; optional */

/* ---- Status / UX ---- */
#define BOARD_PIN_STATUS_LED      BOARD_PIN_NOT_SET /* TODO: optional; GPIOA (IO39) / GPIOB (IO38) are spare header pins if wiring one up */
#define BOARD_PIN_FORCE_AP_BUTTON BOARD_PIN_NOT_SET /* TODO: optional; extension point for net_manager_force_ap_on() */

/*
 * Onboard peripherals documented for reference, not GPIO-mapped here
 * since they live on BOARD_I2C_PORT rather than needing dedicated
 * GPIOs: LTC4015 battery charger/monitor (typ. I2C addr 0x68), and a
 * separate onboard I2C bus (CO2_SDA=IO12/CO2_SCL=IO11/CO2_EN=IO13) for
 * the optional SCD30 CO2 sensor + onboard temp/humidity/pressure/IMU -
 * none wired into firmware, out of scope for v1.
 */

static inline bool board_pin_is_set(int gpio_num)
{
    return gpio_num != BOARD_PIN_NOT_SET;
}
