# Walter Sensor Node

ESP-IDF firmware for a student-configurable environmental datalogger/gateway
built on the Walter ESP32-S3 module + Walter Feels carrier board: SDI-12 and
I2C sensors, SD card logging, MQTT (WiFi or cellular transport), and a
captive-portal web UI for setup - no code changes needed per deployment.

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

## Before first flash: verify `board_pins.h`

`components/board_pins/include/board_pins.h` holds every Walter Feels GPIO
assignment (SDI-12 data/direction/power, I2C SDA/SCL, SD card SPI pins,
sensor power switch) as an explicit "not configured" placeholder - none of
them were verified against real hardware (the schematic PDF couldn't be
parsed while writing this firmware). Each subsystem safely no-ops (logs a
warning, falls back to a synthetic stub sensor where relevant) if its pins
are left unset, so the firmware boots and the portal works even before this
is done - but no real sensor/SD/etc. will function until you:

1. Open the Walter Feels schematic (or check the board silkscreen) and fill
   in every `BOARD_PIN_*` value in `board_pins.h`.
2. If a pin's direction-enable/power-switch polarity is marked "TODO verify
   active level" in `board_pins.h` or `sdi12_bus.c`, confirm it before
   trusting sensor readings - a wrong guess there just won't work, but it's
   worth checking with a meter first.

## Known low-confidence areas (verify before relying on them)

Written without access to real hardware, a full ESP-IDF Python environment,
or the `walter-modem`/`esp-mqtt` library sources - most of the firmware was
checked against the actual ESP-IDF v6.0.2 headers on disk, but these
specific pieces could not be and should be reviewed first:

- **`components/sdi12_bus/`** - bit-banged SDI-12 physical layer (mark/space
  polarity, break/marking timing). Verify with a logic analyzer against one
  known-good sensor.
- **`components/cellular_transport/` and
  `components/mqtt_client/backend_walter_mqtt.cpp`** - every `WalterModem`
  method call is a best-effort guess from a research summary, not the real
  header (marked `VERIFY` inline throughout both files). Only relevant if
  you set network transport to "Cellular" in the portal.
- **`components/mqtt_client/backend_esp_mqtt.c`** - written against the
  documented stable esp-mqtt API, but unverified locally since the
  submodule source was missing (see Prerequisites above).

## First boot

1. Flash and power on. A SoftAP named `WalterSensor-XXXX` appears (open
   network) for 5 minutes, and stays up as long as a client is connected.
2. Connect to it; a captive-portal login page should open automatically
   (or browse to `http://192.168.4.1/`).
3. Log in with the default password `walter1234` and **change it
   immediately** under Portal password.
4. Add variables (SDI-12/I2C sensors), configure MQTT and/or network
   transport, then Save & Reboot if you changed the transport.

## Layout

See `main/app_main.c` for init order and `components/*/` for one component
per hardware resource or concern (config storage, sensor buses, sampling/
aggregation, SD logging, networking, MQTT, web portal). Each component's
header comment explains its role; most are independently testable per the
incremental build order the project was developed in (scaffolding -> config
store -> sampling engine with a stub sensor -> SD logging -> web portal ->
WiFi -> MQTT -> real SDI-12/I2C drivers -> cellular).
