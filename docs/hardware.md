# Hardware

This page documents the physical Open Renewable Lab enclosure — what's
inside it, what each external connector is for, and where to find the
CAD/schematic source if you're building or repairing one.

## The enclosure

<figure markdown>
![Open Renewable Lab enclosure lid, branded with the UNIS logo](pictures/ORL_Hardware%20%283%29.jpeg)
<figcaption>
The enclosure lid: UNIS (The University Centre in Svalbard) branding and
the "Open Renewable LAB" label.
</figcaption>
</figure>

<figure markdown>
![Open Renewable Lab enclosure, external connectors](pictures/ORL_Hardware%20%281%29.jpeg)
<figcaption>
The enclosure's external cable glands, each labeled with what it connects
to. "12V LEAD ACID ONLY!" is a hardware constraint on the battery input —
the onboard LTC4015 charger circuit is built for a 12&nbsp;V lead-acid
battery specifically; check with your instructor before connecting a
different battery chemistry.
</figcaption>
</figure>

### Connector reference

| Label | Connects to | Notes |
|---|---|---|
| **PV** | A small auxiliary solar panel, wired straight to the onboard LTC4015 charger | Keeps the logger's *own* operating battery topped up. **Not a measurement input** — this is not where an instrumented/monitored panel goes; see **AN 0&1**/**AN 2&3** below for that. |
| **AN 0&1** (0x49) | A 4-pin instrumented sensor (e.g. the RS PRO panel in [Getting started](getting-started.md)), via the external ADS1115 ADC at I2C address `73` (`0x49`) | Exposes single-ended channels `AIN0`/`AIN1`. For the PV add-on: `AIN0` = voltage (divider output), `AIN1` = current (shunt voltage). |
| **AN 2&3** (0x49) | Same ADS1115, a second 4-pin instrumented sensor input | Exposes single-ended channels `AIN2`/`AIN3` — same wiring pattern as AN 0&1, for a second instrumented sensor (e.g. a second panel, a pyranometer). |
| **BATT** | The 12&nbsp;V lead-acid battery, read by the onboard LTC4015 battery monitor (I2C address `104`/`0x68`) | Set battery chemistry/cell count under the portal's **Battery monitor** settings — see [USER_MANUAL.md §6.5](USER_MANUAL.md#65-onboard-walter-feels-sensors-battery-temperature-humidity-pressure). |
| **SDI12** (×2) | External SDI-12 sensors (soil moisture, weather stations, etc.) | Multiple SDI-12 sensors can share one bus, each with its own address — see [USER_MANUAL.md §6.2](USER_MANUAL.md#62-finding-your-sensors-address). |

`AN 0&1`/`AN 2&3` are generic analog inputs — this enclosure is built on
the same base as the plain [datalogger](build_datalogger.md), with the PV
sensing add-on (ADS1115 + shunt + divider) wired to those two ports rather
than being a fixed, PV-only circuit.

⚠ **AN 0&1, AN 2&3, and BATT (the LTC4015) all need a battery connected
to work at all.** They share one I2C bus, powered from a "Feels 5V" rail
derived from the battery/PV input — USB alone doesn't power it. Bench
testing over USB only, with nothing on BATT, will make that whole bus
look dead/unresponsive; that's expected, not a fault.

## Inside the enclosure

<figure markdown>
![Open enclosure showing the Walter ESP32-S3 board and internal wiring](pictures/ORL_Hardware%20%282%29.jpeg)
<figcaption>
The Walter ESP32-S3 module + Walter Feels carrier board, with the GNSS
antenna, battery connector, microSD slot, and the screw-terminal block used
for SDI-12 wiring.
</figcaption>
</figure>

## CAD and schematic source

The full KiCad project (schematic + PCB) for the sensing/interconnect board
lives in the repository under
[`hardware/open_renewable_lab/`](https://github.com/<OWNER>/<REPO>/tree/main/hardware/open_renewable_lab),
open it with [KiCad](https://www.kicad.org/) 8 or newer.

The 3D-printable/CNC enclosure mount is under
[`hardware/drawing/`](https://github.com/<OWNER>/<REPO>/tree/main/hardware/drawing):
`OpenRenewableLabMount.stl` (print-ready), `OpenRenewableLabMount.CATPart`
(CATIA source), and `OpenRenewableLabMount.3mf`.

For the firmware-side GPIO pin mapping that corresponds to this hardware
(SDI-12, I2C, SD card pins), see
[DEVELOPER_GUIDE.md §8](DEVELOPER_GUIDE.md#8-board-pin-mapping-board_pins).
