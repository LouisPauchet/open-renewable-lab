#pragma once

/*
 * Board pin map - two variants, selected via `idf.py menuconfig` ->
 * "Walter Sensor Node Board Selection" (components/board_pins/Kconfig.projbuild),
 * independent of but paired with the chip target (`idf.py set-target`):
 *
 *  - BOARD_VARIANT_WALTER_FEELS (default): the production Walter +
 *    Walter Feels carrier board. Requires chip target esp32s3. SDI-12,
 *    I2C, and SD card pins are taken directly from the user-supplied
 *    Walter Feels schematic and are considered confirmed; still
 *    placeholders (BOARD_PIN_NOT_SET) where nothing in the supplied
 *    schematic pages showed a value: SD card-detect, status LED,
 *    force-AP button. Enable-pin ACTIVE LEVELS (TX_EN/RX_EN etc.) are
 *    an inference from the SN74LV1T126 datasheet (active-HIGH OE), not
 *    something visible in a schematic - verify with a meter if SDI-12
 *    doesn't work.
 *
 *  - BOARD_VARIANT_ESP32_DEVKIT_TEST: a plain ESP32 DevKit V1 with no
 *    Walter Feels board at hand, for testing the firmware against real
 *    I2C sensors (e.g. two ADS1115 ADCs at different addresses) before
 *    the production board arrives. Requires chip target esp32 (classic).
 *    Only I2C is wired; SDI-12/SD card/cellular are left unconfigured,
 *    which each subsystem already handles gracefully (see below).
 *
 * Each subsystem's init function calls board_pin_is_set() on the pins
 * it needs and refuses to start (logging an error) rather than driving
 * an unconfirmed pin - so it's safe to build and flash with any pin
 * still unset; only the affected subsystem stays disabled.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"

#define BOARD_PIN_NOT_SET GPIO_NUM_NC

#if CONFIG_BOARD_VARIANT_ESP32_DEVKIT_TEST

/* ---- Generic ESP32 DevKit V1 (I2C-only test rig) ----
 * No SDI-12 transceiver, SD card, or cellular modem wired - all left
 * unconfigured. sdi12_bus_init()/sd_logger_init()/cellular_transport_init()
 * all fail gracefully (logged, non-fatal) without these, and
 * sampling_engine falls back to the synthetic stub_sensor for
 * BUS_TYPE_SDI12 automatically (see app_main.c). */
#define BOARD_PIN_SDI12_TXD BOARD_PIN_NOT_SET
#define BOARD_PIN_SDI12_RXD BOARD_PIN_NOT_SET
#define BOARD_PIN_SDI12_TX_EN BOARD_PIN_NOT_SET
#define BOARD_PIN_SDI12_RX_EN BOARD_PIN_NOT_SET
#define BOARD_PIN_SDI12_BUS_POWER BOARD_PIN_NOT_SET
#define BOARD_PIN_RS485_TX_EN BOARD_PIN_NOT_SET
#define BOARD_PIN_RS485_RX_EN BOARD_PIN_NOT_SET
#define BOARD_PIN_RS232_TX_EN BOARD_PIN_NOT_SET
#define BOARD_PIN_RS232_RX_EN BOARD_PIN_NOT_SET

/* Standard ESP32 DevKit V1 I2C pins (the de facto default used by
 * Arduino's Wire library and most DevKit silkscreens/breakouts). Wire
 * two ADS1115 boards here, each given a distinct address so they
 * coexist on one bus - either via the generic ADS1115 ADDR pin
 * (GND=0x48, VDD=0x49, SDA=0x4A, SCL=0x4B) or a breakout's own
 * jumpers (e.g. Soldered's ADS1115 board: default=0x48, JP3=0x39,
 * JP4=0x4A, JP5=0x4B - close only one at a time). Confirm your actual
 * boards' addresses with the portal's "Scan I2C bus" button rather
 * than assuming - configure one "variable" per ADC input you want in
 * the portal, with device_type 0 (ADS111x) and the channel index set
 * via the portal's dropdown (supports both single-ended and
 * differential pairs - see
 * ads111x.c's header comment for the full mapping). */
#define BOARD_I2C_PORT (-1) /* -1 = let the I2C driver auto-assign a free port */
#define BOARD_PIN_I2C_SDA GPIO_NUM_21
#define BOARD_PIN_I2C_SCL GPIO_NUM_22
#define BOARD_PIN_I2C_BUS_POWER BOARD_PIN_NOT_SET /* no switched rail on a plain DevKit - power sensors from 3V3/5V directly */
#define BOARD_I2C_CLOCK_HZ 100000

#define BOARD_PIN_SD_CLK BOARD_PIN_NOT_SET
#define BOARD_PIN_SD_CMD BOARD_PIN_NOT_SET
#define BOARD_PIN_SD_D0 BOARD_PIN_NOT_SET
#define BOARD_PIN_SD_CARD_DETECT BOARD_PIN_NOT_SET

#define BOARD_PIN_STATUS_LED BOARD_PIN_NOT_SET /* many DevKit V1 clones have an onboard LED on GPIO2, but not universally - verify before wiring logic to it */
#define BOARD_PIN_FORCE_AP_BUTTON BOARD_PIN_NOT_SET

/* No switched 3.3V rail on a plain DevKit - sensors are powered from
 * 3V3/5V directly, so board_pins_enable_3v3_sw() is a no-op here. */
#define BOARD_PIN_3V3_SW_EN BOARD_PIN_NOT_SET

/* No onboard HDC1080/LPS22HB/LTC4015 on a plain DevKit - onboard_i2c_bus_init()
 * fails gracefully (logged, non-fatal) without these pins set, same as
 * every other optional subsystem on this board variant. */
#define BOARD_ONBOARD_I2C_PORT (-1)
#define BOARD_PIN_ONBOARD_I2C_SDA BOARD_PIN_NOT_SET
#define BOARD_PIN_ONBOARD_I2C_SCL BOARD_PIN_NOT_SET
#define BOARD_PIN_ONBOARD_I2C_EN BOARD_PIN_NOT_SET
#define BOARD_ONBOARD_I2C_CLOCK_HZ 100000

#else /* BOARD_VARIANT_WALTER_FEELS (default) */

/* ---- SDI-12 bus ----
 * Unlike a typical single-wire half-duplex SDI-12 breakout, Walter
 * Feels uses two SN74LV1T126 tri-state buffers (one per direction) to
 * put SDI-12 on a shared UART-style bus (SER_TX/SER_RX) that's also
 * muxed with RS485 and RS232 transceivers via their own *_EN pins.
 * SDI-12's TX/RX enable pins are ACTIVE HIGH (SN74LV1T126 has a fixed
 * active-high OE, unlike the active-low '125 variant) - confirmed on
 * real hardware (TX_EN=1 measurably drives the bus; RX_EN's own
 * active level confirmed via DPTechnics support, see below).
 */
#define BOARD_PIN_SDI12_TXD    GPIO_NUM_40 /* SER_TX - MCU output, through U5 to the bus */
#define BOARD_PIN_SDI12_RXD    GPIO_NUM_41 /* SER_RX - MCU input, from the bus through U6 */
#define BOARD_PIN_SDI12_TX_EN  GPIO_NUM_10 /* SDI12_TX_EN - drive HIGH only while transmitting */
#define BOARD_PIN_SDI12_RX_EN  GPIO_NUM_9  /* SDI12_RX_EN - left HIGH permanently (always listening) */
#define BOARD_PIN_SDI12_BUS_POWER GPIO_NUM_43 /* 12V_EN - switched rail many SDI-12 sensors need */

/* SER_TX/SER_RX are shared with RS485 and RS232 transceivers, each
 * gated by their own enable pin - confirmed via DPTechnics support
 * (Daan) that fully disabling each requires *_TX_EN=LOW AND
 * *_RX_EN=HIGH (NOT both LOW, as originally assumed here - RS485/RS232's
 * RX_EN pins are apparently active-low receiver-enables, a common
 * RS-485 transceiver DE/~RE pattern, unlike SDI-12's own simple
 * active-high buffer OE). Leaving RS485_RX_EN/RS232_RX_EN LOW left
 * both transceivers' receivers actively driving the shared SER_RXD
 * line, contending with SDI-12's own U6 output - the real cause of a
 * real "RXD never sees anything" symptom on real hardware that
 * otherwise checked out at every single other link (bus signal
 * present, U6 wired to the bus, U6 powered, RX_EN correctly asserted).
 * sdi12_bus_init() drives *_TX_EN low and *_RX_EN high accordingly. */
#define BOARD_PIN_RS485_TX_EN GPIO_NUM_18
#define BOARD_PIN_RS485_RX_EN GPIO_NUM_8
#define BOARD_PIN_RS232_TX_EN GPIO_NUM_17
#define BOARD_PIN_RS232_RX_EN GPIO_NUM_16

/* ---- I2C bus (GPIO42/2/1) ----
 * The single, shared I2C bus for this board - confirmed directly
 * against the Walter Feels schematic: every I2C device (the external
 * connector's ADC boards, and the onboard HDC1080/LPS22HB/LTC4015) is
 * wired to SDA=42/SCL=2, powered from the one switched rail on GPIO1.
 * Matches the real-hardware scan evidence in i2c_sensors/hdc1080.c's
 * header comment - GPIO1 driven HIGH (its assumed active level, see
 * i2c_bus.c) successfully powered HDC1080/LPS22HB/LTC4015/an LSM6DSM
 * IMU for that scan, which also resolves the polarity as correct rather
 * than merely assumed.
 *
 * CONFIRMED BY THE BOARD OWNER: this whole bus (and its devices'
 * supply) is fed from "Feels 5V", a rail derived from the board's
 * battery/PV input - USB alone cannot power it. Bench-testing over USB
 * only, with no battery connected, is therefore *expected* to show
 * i2c_bus's boot-time "I2C bus stuck (SDA held low) - attempting
 * recovery" / "recovery failed" warnings (nothing is actually wedged;
 * the bus and every device on it are simply unpowered) - this isn't a
 * firmware bug and no amount of bus-recovery logic will fix it. Connect
 * a battery (or otherwise supply Feels 5V) before trusting an I2C
 * failure on this bus as a real fault. */
#define BOARD_I2C_PORT          (-1) /* -1 = let the I2C driver auto-assign a free port */
#define BOARD_PIN_I2C_SDA       GPIO_NUM_42
#define BOARD_PIN_I2C_SCL       GPIO_NUM_2
#define BOARD_PIN_I2C_BUS_POWER GPIO_NUM_1 /* I2C_BUSPOW - shared switched rail for the whole I2C bus above, not just external-connector devices */
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

/* ---- Switched 3.3V sensor-power rail (3V3_SW) ----
 * Confirmed via the official Walter datasheet, DPTechnics' hardware
 * FAQ, and the Walter Feels schematic (v2.6): module pin 26 ("3V3
 * OUT") is a software-switched 3.3V rail, OFF by default, enabled by
 * driving module pin 4 (ESP GPIO0) LOW. It feeds the onboard
 * HDC1080/LPS22HB/IMU sensors and the CO2 sensor's supply per the
 * schematic - real hardware testing also found it powers the external
 * I2C connector and (very likely) the SD card, since both failed
 * without it. GPIO0 is also the boot-mode strap pin (LOW at reset =
 * download mode) - only drive it from board_pins_enable_3v3_sw(),
 * called once well after boot completes (see app_main.c), never near
 * a reset. */
#define BOARD_PIN_3V3_SW_EN GPIO_NUM_0

/* ---- Onboard peripherals ----
 * LTC4015 battery charger/monitor, HDC1080 (temp/humidity, fixed addr
 * 0x40), and LPS22HB (pressure/temp, addr 0x5C or 0x5D depending on
 * SA0 strapping) all live on BOARD_I2C_PORT above (the external-
 * connector bus), fixed I2C addrs - no dedicated GPIOs needed, see
 * i2c_sensors/ltc4015.c, hdc1080.c, lps22hb.c.
 *
 * CORRECTED from an earlier assumption that HDC1080/LPS22HB sat on
 * this separate onboard bus instead: real-hardware I2C scan evidence
 * (a factory-fresh board, nothing plugged into the external connector)
 * found 0x40/0x5C/0x68/0x6A all by scanning the EXTERNAL bus, while
 * this bus's own devices cleanly NACKed every read (not timed out).
 * Confirmed directly by the board owner: this pin pair only reaches a
 * real, unpopulated CO2 sensor header on the PCB (pads present, no
 * chip soldered) - not a second bus for the fixed sensors. */
#define BOARD_ONBOARD_I2C_PORT      (-1) /* -1 = let the I2C driver auto-assign a free port */
#define BOARD_PIN_ONBOARD_I2C_SDA   GPIO_NUM_12 /* CO2_SDA - unpopulated CO2 (SCD30) sensor header (also documented to carry an LSM6DSM IMU); neither wired into firmware, out of scope for v1 */
#define BOARD_PIN_ONBOARD_I2C_SCL   GPIO_NUM_11 /* CO2_SCL */
#define BOARD_PIN_ONBOARD_I2C_EN    GPIO_NUM_13 /* CO2_EN - active level not confirmed from the schematic; onboard_i2c_bus_init() drives it HIGH, verify with a meter once a CO2/IMU chip is actually populated and a driver is added */
#define BOARD_ONBOARD_I2C_CLOCK_HZ  100000

#endif /* BOARD_VARIANT */

static inline bool board_pin_is_set(int gpio_num)
{
    return gpio_num != BOARD_PIN_NOT_SET;
}

/* `1ULL << BOARD_PIN_xxx` written directly at a call site is a compile-time
 * constant shift as far as GCC's front end is concerned, so on board
 * variants where that pin is BOARD_PIN_NOT_SET (-1) it trips
 * -Werror=shift-count-negative even though the call is always guarded by
 * board_pin_is_set() at runtime. Routing the shift through a function
 * parameter (as here) keeps it from being evaluated as a constant
 * expression at parse time. */
static inline uint64_t board_pin_bit_mask(int gpio_num)
{
    return (uint64_t)1 << gpio_num;
}

/* Enables the switched 3.3V sensor-power rail (see BOARD_PIN_3V3_SW_EN
 * above) on variants that have one; a no-op otherwise. Call exactly
 * once, early in app_main() - well after boot completes, since this
 * pin doubles as the GPIO0 download-mode boot strap. Several
 * subsystems' init (external I2C connector, onboard I2C sensor bus, SD
 * card) depend on this rail being up first. */
static inline void board_pins_enable_3v3_sw(void)
{
    if (!board_pin_is_set(BOARD_PIN_3V3_SW_EN)) {
        return;
    }
    gpio_config_t conf = {
        .pin_bit_mask = board_pin_bit_mask(BOARD_PIN_3V3_SW_EN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&conf);
    gpio_set_level(BOARD_PIN_3V3_SW_EN, 0); /* active LOW - see BOARD_PIN_3V3_SW_EN comment */
}
