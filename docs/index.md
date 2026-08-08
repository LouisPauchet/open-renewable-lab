# Open Renewable Lab

The **Open Renewable Lab** is UNIS's (The University Centre in Svalbard)
student-configurable environmental datalogger/gateway, built on the Walter
ESP32-S3 module and the Walter Feels carrier board. It reads SDI-12 and I2C
sensors (including a built-in PV voltage/current sensing input), logs to a
microSD card, and publishes to an MQTT broker over WiFi or cellular — all
configured through a web page served by the device itself, with **no code
changes needed per deployment**.

Every device has a stable ID derived from its factory MAC address, shown in
the portal's status panel, written into every CSV file's header line, and
included in every MQTT topic, so multiple nodes can share one broker.

## Where to start

**Building the hardware from parts?** Step-by-step assembly of the
enclosure, wiring, and board — see
[Build a datalogger](build_datalogger.md).

**New to the lab?** Wiring up a real 20 W solar panel and getting live data
into ThingsBoard, start to finish — see
[Getting started](getting-started.md).

**Just need firmware on a board?** Flash a pre-built binary straight from
your browser or with `esptool` — no ESP-IDF install required — see
[Firmware downloads](firmware-downloads.md).

**Operating a device day-to-day?** Sensors, MQTT, SD logging, the web
portal — the full reference for anyone deploying or running a station — see
the [User manual](USER_MANUAL.md).

**Reading or modifying the firmware?** Architecture, component reference,
REST API, and how to extend the code — see the
[Developer guide](DEVELOPER_GUIDE.md).

## What it can do

- Read **SDI-12** sensors (soil moisture, weather stations, water level
  probes) and **I2C** sensors (ADCs, onboard battery/temperature/humidity/
  pressure sensors).
- Log everything to a **microSD card** as CSV files.
- Publish data to an **MQTT broker** over WiFi or cellular (4G/LTE-M/NB-IoT),
  including a ThingsBoard-compatible "flat telemetry" mode — see
  [Getting started](getting-started.md).
- Report **GPS position** over cellular.
- Be fully configured **without writing any code** — every sensor, timing
  setting, and network/MQTT credential is set through the device's own
  captive-portal web page.

## Hardware

The lab's physical build — enclosure, connectors, and PV sensing input — is
documented with photos on the [Hardware](hardware.md) page.
