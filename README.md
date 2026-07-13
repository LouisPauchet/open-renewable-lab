# Walter Sensor Node

ESP-IDF firmware for a student-configurable environmental datalogger/gateway
built on the Walter ESP32-S3 module + Walter Feels carrier board: SDI-12 and
I2C sensors, SD card logging, MQTT (WiFi or cellular transport), GNSS
position logging (cellular only), scheduled batch MQTT transmission for
power saving, and a captive-portal web UI for setup - no code changes needed
per deployment.

Every device has a stable ID derived from its factory MAC address, shown in
the portal's status panel, written into every CSV file's header line, and
included in every MQTT topic (`<topic_prefix>/<device_id>/<variable name>`)
so multiple nodes can share one broker/topic_prefix.

## Documentation

- **[docs/USER_MANUAL.md](docs/USER_MANUAL.md)** - deploying and operating a
  device via the web portal. No code or ESP-IDF knowledge needed; start here
  if you're a student setting up a sensor station.
- **[docs/DEVELOPER_GUIDE.md](docs/DEVELOPER_GUIDE.md)** - firmware
  architecture, component reference, REST API, concurrency model, and how to
  extend the code (e.g. add a new sensor driver). Start here if you're
  reading or modifying the source.

## Prerequisites

- ESP-IDF **v6.0.2**, target `esp32s3`.
- Network access at build time: `dptechnics/walter-modem` is fetched from the
  ESP Component Registry via `main/idf_component.yml`.
- **Check your ESP-IDF install has the `mqtt` component's source** before
  building: `components/mqtt/esp-mqtt/include` should exist. If it doesn't
  (missing submodule), re-run the ESP-IDF installer / `git submodule update`
  first - this was missing in the environment this firmware was written in,
  so it was never build-tested end-to-end.

## Build

```
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

## Before first flash: check `board_pins.h`

`components/board_pins/include/board_pins.h` holds every Walter Feels GPIO
assignment. SDI-12, I2C, and SD card pin *numbers* come straight from the
schematic and are confirmed; enable-pin polarities and a few optional pins
(card-detect, status LED, force-AP button) are not - see
[DEVELOPER_GUIDE.md §8](docs/DEVELOPER_GUIDE.md#8-board-pin-mapping-board_pins)
for the full pin table and what's still open. Each subsystem safely no-ops
(logs a warning, falls back to a synthetic stub sensor where relevant) if
its required pins are left unset, so the firmware boots and the portal
works even before any of this is resolved.

For a full list of known low-confidence areas (cellular/GNSS integration,
the SDI-12 physical layer, etc.) that should be reviewed before a real
deployment, see
[DEVELOPER_GUIDE.md §18](docs/DEVELOPER_GUIDE.md#18-known-limitations-and-low-confidence-areas).

## First boot

See [USER_MANUAL.md](docs/USER_MANUAL.md) for the full walkthrough. Short
version: connect to the `WalterSensor-XXXX` WiFi hotspot, browse to
`http://192.168.4.1/` if the login page doesn't pop up automatically, log
in with the default password `walter1234`, and **change it immediately**.
