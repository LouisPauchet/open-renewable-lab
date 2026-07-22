# Walter Sensor Node — Developer Guide

This guide explains how the firmware is put together, for anyone reading,
modifying, or extending the code. If you just want to *use* a device, see
[USER_MANUAL.md](USER_MANUAL.md) instead.

It assumes basic C familiarity but not prior ESP-IDF/embedded experience —
unfamiliar terms are explained inline or in the [glossary](#20-glossary)
at the end.

---

## Table of contents

1. [Goals and constraints](#1-goals-and-constraints)
2. [Prerequisites and build](#2-prerequisites-and-build)
3. [Project layout](#3-project-layout)
4. [Architecture overview](#4-architecture-overview)
5. [Boot sequence](#5-boot-sequence)
6. [Concurrency model](#6-concurrency-model)
7. [Configuration system (`config_store`)](#7-configuration-system-config_store)
8. [Board pin mapping (`board_pins`)](#8-board-pin-mapping-board_pins)
9. [Sensor buses](#9-sensor-buses)
10. [Sampling and aggregation (`sampling_engine`)](#10-sampling-and-aggregation-sampling_engine)
11. [SD logging (`sd_logger`)](#11-sd-logging-sd_logger)
12. [Networking (`net_manager`)](#12-networking-net_manager)
13. [MQTT (`mqtt_client`)](#13-mqtt-mqtt_client)
14. [Cellular integration (`cellular_transport`)](#14-cellular-integration-cellular_transport)
15. [Position reporting (`gnss_position`)](#15-position-reporting-gnss_position)
16. [Device identification (`device_id`)](#16-device-identification-device_id)
17. [Web portal (`web_portal`)](#17-web-portal-web_portal)
18. [Known limitations and low-confidence areas](#18-known-limitations-and-low-confidence-areas)
19. [Testing without Walter Feels hardware](#19-testing-without-walter-feels-hardware)
20. [How to extend the firmware](#20-how-to-extend-the-firmware)
21. [Glossary](#21-glossary)

---

## 1. Goals and constraints

This firmware is a **student-configurable environmental sensor node**: the
goal is that a student deploying a new station never needs to touch the
source code or reflash firmware — every sensor, timing setting, and network
credential is set through a captive-portal web UI (see
[USER_MANUAL.md](USER_MANUAL.md)).

A few decisions run through the whole codebase and are worth knowing before
you start reading source:

- **No OTA in v1** — a single factory app partition, no update mechanism.
  Reflashing is done over USB.
- **Config changes apply live where reasonably possible.** Most settings
  (variables, MQTT, position) take effect immediately via a
  generation-counter change-detection pattern (see [section 7](#7-configuration-system-config_store)).
  Only network *transport* (WiFi ↔ Cellular ↔ unconfigured) requires a
  reboot, to avoid the complexity of live-tearing-down the WiFi/modem stack.
- **Graceful degradation over hard failure.** If a board pin isn't
  configured, or a card/sensor isn't present, the affected subsystem logs a
  warning and disables itself rather than blocking boot or crashing — see
  [section 8](#8-board-pin-mapping-board_pins).
- **One FreeRTOS task per hardware resource**, not per feature — e.g. one
  task owns the I2C bus and services requests from other components via
  narrow interfaces, rather than several tasks reaching into the bus driver
  directly. See [section 6](#6-concurrency-model).

---

## 2. Prerequisites and build

- ESP-IDF **v6.0.2**, target `esp32s3`.
- Network access at build time: `dptechnics/walter-modem` is a managed
  dependency fetched from the ESP Component Registry (declared in
  `main/idf_component.yml`).
- **Your ESP-IDF install must have the `mqtt` component's source** —
  check `components/mqtt/esp-mqtt/include` exists under your ESP-IDF path.
  This was missing in the environment this firmware was originally written
  in (a submodule that hadn't been fetched), so parts of `mqtt_client` were
  written against documented API behavior rather than checked headers — see
  [section 18](#18-known-limitations-and-low-confidence-areas).

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Before flashing onto real hardware, read [section 8](#8-board-pin-mapping-board_pins)
— some GPIO assignments are still placeholders.

---

## 3. Project layout

```
open_renewable_lab/
├── CMakeLists.txt, sdkconfig.defaults, partitions.csv
├── main/
│   ├── app_main.c          # init order + task spawn only, no business logic
│   └── idf_component.yml   # declares the walter-modem managed dependency
├── docs/
│   ├── USER_MANUAL.md
│   └── DEVELOPER_GUIDE.md  # this file
└── components/
    ├── board_pins/         # GPIO assignment header, single source of truth
    ├── device_id/          # MAC-derived unique device identifier
    ├── config_store/       # NVS-backed config, JSON (de)serialization
    ├── sampling_engine/    # per-bus scheduling + Welford aggregation
    ├── stub_sensor/        # synthetic sensor source for testing w/o hardware
    ├── sdi12_bus/          # bit-banged SDI-12 physical layer + protocol
    ├── i2c_bus/            # driver/i2c_master.h wrapper (external I2C connector)
    ├── onboard_i2c_bus/    # second, separate driver/i2c_master.h wrapper (Walter Feels'
    │                       # onboard HDC1080/LPS22HB sensor bus - distinct physical bus/pins)
    ├── i2c_sensors/        # per-device-type driver registry (ADS111x, HDC1080, LPS22HB, LTC4015, ...)
    ├── sd_logger/          # SDMMC mount + CSV writer
    ├── net_manager/        # WiFi AP+STA, always-on-AP policy
    ├── time_sync/          # SNTP client, shared "is time synced" definition
    ├── mqtt_client/        # transport-agnostic MQTT bridge, two backends
    ├── cellular_transport/ # WalterModem wrapper: registration, PDP, GNSS
    ├── gnss_position/      # periodic GNSS fix -> SD + MQTT
    └── web_portal/         # captive DNS, HTTP server, REST API, embedded SPA
```

Each component maps to one hardware resource or one cross-cutting concern.
`sampling_engine` never touches bus hardware directly — it calls through
`sdi12_bus`/`i2c_bus` via a registered function pointer — so sensors,
storage, and networking stay independently testable and swappable.

---

## 4. Architecture overview

```
                    ┌──────────────┐
                    │   app_main   │  init order + task spawn only
                    └──────┬───────┘
      ┌──────────┬─────────┼─────────┬───────────┬─────────────┐
      ▼          ▼         ▼         ▼           ▼             ▼
 board_pins  config_store  sdi12_bus  i2c_bus  sampling_    sd_logger
 (headers)   (NVS+JSON)              i2c_sensors  engine
                                                    │
                    ┌───────────────────────────────┼──────────────┐
                    ▼                                ▼              ▼
              net_manager                      mqtt_client    sd_logger
              (AP + STA)                     (dual backend)   (CSV sink)
                    │                                │
                    ▼                                ▼
              web_portal                     cellular_transport
           (REST API + SPA)                  (WalterModem, GNSS)
                                                       │
                                                       ▼
                                                 gnss_position
```

Everything the student configures lives in `config_store` (a single
in-RAM `device_config_t`, mirrored to NVS flash). Every other component
either reads from it directly (e.g. `sampling_engine` re-reads the variable
list each scheduling tick) or is notified of changes via its generation
counter (see [section 7.3](#73-the-generation-counter-pattern)).

---

## 5. Boot sequence

`main/app_main.c` is deliberately just an init-order/task-spawn list, no
business logic. Exact sequence:

1. Log chip info (cores, revision, flash size, free heap) and the device ID.
2. `config_store_init()` — mounts NVS, loads (or defaults) the config.
3. `config_store_get_snapshot()` — a local copy used for the rest of boot
   (safe because `net.transport` can't change without a reboot anyway).
4. `sdi12_bus_init()` — if it fails (pins unset), falls back to
   `stub_sensor_read` for `BUS_TYPE_SDI12` instead of the real driver.
5. `i2c_bus_init()` — same fallback pattern for `BUS_TYPE_I2C`.
6. `sampling_engine_init()` — spawns the bus-scheduler and aggregation
   tasks (drivers were already registered in steps 4-5).
7. A small debug task registers as a result sink and logs every finalized
   aggregate to the serial console — always active, useful during bring-up.
8. `sd_logger_init()` — best-effort; registers as a result sink on success,
   logs a warning and continues on failure (no card, pins unset, etc).
9. `net_manager_init()` — brings up the SoftAP (always) and WiFi STA (only
   if `transport == TRANSPORT_WIFI`).
10. `web_portal_init()` — starts the captive-portal DNS hijack and HTTP
    server.
11. `time_sync_init()` — starts the SNTP client (opportunistic, harmless
    without connectivity).
12. If `transport == TRANSPORT_CELLULAR`: `cellular_transport_init()`, and
    only on its success, `gnss_position_init()`.
13. `mqttc_init()` — always called; registers as a result sink and picks
    a backend (or `NULL` if transport is unconfigured, in which case MQTT
    stays fully inert).

Notice steps 4-8 all use the same pattern: **attempt init, register the
real driver/sink on success, fall back to a safe default (stub driver, or
simply not registering a sink) on failure** — nothing in this list can
abort boot.

---

## 6. Concurrency model

FreeRTOS tasks in this firmware, and what owns what:

| Task | Component | Owns / does | Notes |
|---|---|---|---|
| `sdi12_sched` | sampling_engine | SDI-12 bus scheduling | Sole caller of `sdi12_bus` from the periodic path |
| `i2c_sched` | sampling_engine | I2C bus scheduling | Sole caller of `i2c_bus` from the periodic path |
| `aggregation` | sampling_engine | Per-variable Welford accumulators | Registered with the task watchdog |
| `debug_sink` | main | Logs every aggregate to console | |
| `sd_writer` | sd_logger | All SD card file I/O | Registered with the task watchdog |
| (esp_wifi/lwIP internal tasks) | net_manager | WiFi driver | Not application-owned |
| `sta_reconnect` (esp_timer, not a task) | net_manager | STA reconnect backoff | One-shot timer, not a persistent task |
| `mqtt_pub` | mqtt_client | All MQTT connect/publish/disconnect calls | Registered with the task watchdog |
| `cellular` | cellular_transport | Registration status polling | |
| `gnss_pos` | gnss_position | Periodic GNSS fix acquisition | **Not** watchdog-registered (see below) |
| `httpd` (esp_http_server internal) | web_portal | HTTP request handling | |
| `dns_hijack` | web_portal | UDP:53 captive-portal DNS | |

### 6.1 Bus ownership, not mutexes, for periodic access

Rather than guarding `sdi12_bus`/`i2c_bus` with a mutex that any task could
contend for, each bus's *periodic* scheduling is owned by exactly one task
(`sdi12_sched`/`i2c_sched` inside `sampling_engine`). Both driver
components still carry an **internal mutex** of their own, though — not to
arbitrate between the sched tasks (there's only one caller each in the
periodic path), but because **on-demand callers** (the web portal's
Preview/Scan buttons, called from the HTTP server's own task) need to be
safely serialized against whichever sched task might currently be mid-
transaction. So: single owner for the periodic path, mutex for cross-task
safety against ad hoc callers.

### 6.2 Fan-out via result sinks

`sampling_engine` finalizes an `aggregate_result_t` per variable per log
interval and pushes a **copy** to every registered sink queue (up to
`MAX_RESULT_SINKS = 4`: currently the debug logger, `sd_logger`, and
`mqtt_client`). Sends are non-blocking (`xQueueSend(..., 0)`) — a slow or
stalled sink drops its own copy (logged) rather than ever blocking
`aggregation_task` or the other sinks.

### 6.3 Task watchdog registration

`aggregation_task`, `sd_writer_task`, and `mqtt_publish_task` are
registered with ESP-IDF's task watchdog (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=15`,
`CONFIG_ESP_TASK_WDT_PANIC=y` — a genuine hang panics and reboots, since
this is meant for unattended field operation). They all have per-iteration
blocking bounded well under 15 seconds.

`sdi12_sched`/`i2c_sched` and `gnss_pos` are **deliberately not**
registered: a single sensor read (especially SDI-12's `aM!`-then-wait-then-
`aD0!` sequence, capped at `SDI12_MAX_MEASURE_WAIT_S = 180` seconds) or a
cold GNSS fix (`GNSS_FIX_TIMEOUT_MS = 60000`) can legitimately block far
longer than the watchdog timeout, and that's expected behavior, not a hang.

---

## 7. Configuration system (`config_store`)

### 7.1 Schema

The entire device configuration is one struct, `device_config_t`
(`components/config_store/include/config_schema.h`):

```c
typedef struct {
    uint32_t schema_version;
    char portal_password_hash[96];   // salt(16 hex) + sha256(64 hex)
    net_settings_t net;
    mqtt_settings_t mqtt;
    position_settings_t position;
    variable_config_t variables[MAX_VARIABLES];  // MAX_VARIABLES = 32
    uint8_t variable_count;
    uint32_t generation;
} device_config_t;
```

`variable_config_t` uses a union keyed by `bus_type` for the
bus-specific address fields (`sdi12_addr_t` = address char + parameter
index; `i2c_addr_t` = I2C address + device type + channel index).

### 7.2 Persistence

`config_store` keeps exactly one in-RAM copy (`s_config`), guarded by a
mutex, and mirrors it to NVS as a **single serialized JSON blob** (namespace
`"cfg"`, key `"device_json"`) rather than flat NVS key-value pairs — the
schema is nested/list-shaped (variables array, sub-structs), which flat NVS
keys handle awkwardly. `config_store_to_json()`/`config_store_from_json()`
are exported publicly precisely so `web_portal`'s REST handlers (variables
CRUD, settings export/import) reuse the *exact same* mapping — the wire
format and the persisted format can never drift apart.

On first boot, a missing NVS entry, or a corrupt/unparseable blob,
`config_store` falls back to safe compiled-in defaults
(`config_set_defaults()`): unconfigured transport, MQTT disabled, zero
variables, and the portal password hashed from `DEFAULT_PORTAL_PASSWORD`
(`"walter1234"`).

### 7.3 The generation counter pattern

Every mutating call (`config_store_add_variable`, `config_store_set_mqtt_settings`,
etc.) increments `s_config.generation` and persists to NVS before
returning. Other tasks that need to react to config changes — but don't
want to take a lock on every scheduling tick — instead compare
`config_store_get_generation()` against a locally-cached "last seen" value
each loop iteration, and only re-fetch/re-process the relevant settings
when it differs. `sampling_engine`'s bus-scheduler and aggregation tasks,
and `mqtt_publish_task`, all use this exact pattern.

### 7.4 Accessor API shape

For each settings group, `config_store.h` exposes a `get`/`set` pair that
copies the whole sub-struct in/out under the mutex (never hands out a
pointer into live state):

```c
void config_store_get_mqtt_settings(mqtt_settings_t *out);
esp_err_t config_store_set_mqtt_settings(const mqtt_settings_t *settings);
```

Same shape for `net_settings_t` and `position_settings_t`. Variables get
full CRUD (`get_variables` / `get_variable` / `add_variable` /
`update_variable` / `delete_variable`) since they're a list, not a
singleton.

---

## 8. Board pin mapping (`board_pins`)

`components/board_pins/include/board_pins.h` is the **single source of
truth** for every GPIO number — no other component hardcodes a pin number.

It has two `#if`-branched variants selected via a Kconfig choice
(`components/board_pins/Kconfig.projbuild`, menuconfig → "Walter Sensor
Node Board Selection"): the production Walter Feels mapping (below), and a
minimal I2C-only mapping for testing on a plain ESP32 DevKit V1 without a
Walter Feels board in hand — see
[section 19](#19-testing-without-walter-feels-hardware) for that workflow.

Most pins are now confirmed against the actual Walter Feels schematic
(supplied mid-project):

| Signal | GPIO | Notes |
|---|---|---|
| `BOARD_PIN_SDI12_TXD` | 40 | `SER_TX` — through a tri-state buffer (U5) onto the SDI-12 bus |
| `BOARD_PIN_SDI12_RXD` | 41 | `SER_RX` — from the bus through a second buffer (U6) |
| `BOARD_PIN_SDI12_TX_EN` | 10 | Asserted only while transmitting |
| `BOARD_PIN_SDI12_RX_EN` | 9 | Left asserted permanently (always listening) |
| `BOARD_PIN_SDI12_BUS_POWER` | 43 | `12V_EN` |
| `BOARD_PIN_RS485_TX_EN`/`RX_EN` | 18 / 8 | Held **low** by `sdi12_bus_init()` — shares the same `SER_TX`/`SER_RX` trunk, must stay disabled while SDI-12 is active |
| `BOARD_PIN_RS232_TX_EN`/`RX_EN` | 17 / 16 | Same reasoning as RS485 |
| `BOARD_PIN_I2C_SDA` / `SCL` | 42 / 2 | |
| `BOARD_PIN_I2C_BUS_POWER` | 1 | `I2C_BUSPOW` |
| `BOARD_PIN_SD_CLK`/`CMD`/`D0` | 5 / 6 / 4 | SDMMC **1-bit** mode — no D1-D3 wired |
| `BOARD_PIN_SD_CARD_DETECT` | *unset* | Not visible on the schematic pages available; SD works fine without it |
| `BOARD_PIN_STATUS_LED` | *unset* | Optional; GPIO38/39 (`GPIOA`/`GPIOB`) are spare header pins if you wire one up |
| `BOARD_PIN_FORCE_AP_BUTTON` | *unset* | Optional; extension point for `net_manager_force_ap_on()` |

Two things are **inferred, not confirmed**: the SDI-12 enable pins'
active level (assumed active-HIGH, since the SN74LV1T126 buffer chip only
supports active-high OE — there's no other option for that specific part),
and the SD card's electrical polarity/timing margins (standard SDMMC, not
board-specific). If SDI-12 doesn't work, check enable-pin polarity with a
meter before assuming the driver logic is wrong.

**The safety pattern**: every subsystem's init function calls
`board_pin_is_set(pin)` (`pin != GPIO_NUM_NC`) on the pins it needs and
returns an error — logged, not fatal — rather than ever driving an
unconfirmed pin. This is why you can build and flash today even with some
pins still unset; only the affected subsystem stays disabled.

---

## 9. Sensor buses

### 9.1 SDI-12 (`sdi12_bus`)

SDI-12 is a 1200-baud, 7-data-bit, even-parity, 1-stop-bit serial protocol
with **inverted mark/space polarity** versus standard UART (idle/mark =
logic LOW, break/space = logic HIGH) and a non-standard wake sequence (a
≥12ms break, then a ≥8.33ms marking gap, before the actual command).

**Why bit-banged instead of hardware UART or RMT**: early design
considered both. Hardware UART was ruled out initially under the wrong
assumption that SDI-12 shared one bidirectional GPIO (a real UART peripheral
can't easily do open-drain half-duplex on a single pin without extra
GPIO-matrix trickery) — though the schematic later revealed Walter Feels
actually *does* wire SDI-12 through separate TX/RX pins onto a shared UART
trunk, which in hindsight could support a hardware-UART implementation.
RMT (ESP32's dedicated pulse-train peripheral) was considered but rejected
in favor of something easier to reason about and verify without hardware
access: **manual GPIO bit-banging**, using a GPIO interrupt to detect each
character's start-bit edge (avoiding a busy-wait across the sensor's
up-to-750ms response gap) plus a short `esp_rom_delay_us` busy-wait to
sample that one character's bits at their centers.

Key constants (`sdi12_bus.c`): `SDI12_BIT_PERIOD_US = 833` (1/1200s),
`SDI12_BREAK_US = 15000`, `SDI12_MARKING_US = 9000` (both padded above the
spec minimums).

Concurrency: `sdi12_bus_init()` configures `BOARD_PIN_SDI12_TXD` as a
permanent output and `BOARD_PIN_SDI12_RXD` as a permanent input with a GPIO
interrupt (only enabled during an active receive wait) — no
`gpio_set_direction()` toggling needed at transaction time, since TX and RX
are physically separate pins. An internal mutex (`sdi12_bus.c`) serializes
whichever task calls in (`sdi12_sched`, or the web portal's preview/scan
handlers from the HTTP task).

**API** (`include/sdi12_bus.h`):

```c
esp_err_t sdi12_bus_init(void);
esp_err_t sdi12_send_command(char addr, const char *cmd_body,
                              char *resp_buf, size_t resp_buf_len,
                              uint32_t response_timeout_ms);
esp_err_t sdi12_scan_addresses(char *found, size_t max_found, size_t *out_count);
esp_err_t sdi12_change_address(char old_addr, char new_addr);
esp_err_t sdi12_measure_and_read(char addr, uint8_t parameter_index, double *out_value);
esp_err_t sdi12_variable_read(const variable_config_t *var, double *out_value); // sensor_bus_read_fn_t adapter
```

`sdi12_measure_and_read()` implements the standard `aM!` → wait → `aD0!`
sequence and parses concatenated signed-decimal values (SDI-12 has no
delimiter between values; `strtod()`'s end-pointer naturally lands on the
next value's sign character, so repeated `strtod()` calls split them
cleanly). **Known limitation**: it only issues `aD0!`, not `aD1!..aD9!`, so
sensors returning more values than fit in one `aD0!` response aren't fully
supported yet — see `sdi12_protocol.c`.

**Status**: pin assignments are confirmed from the schematic; the bit
timing and enable-pin polarity are not yet verified against real hardware
(no oscilloscope/sensor was available while writing this).

### 9.2 I2C (`i2c_bus` + `i2c_sensors`)

`i2c_bus` wraps ESP-IDF's `driver/i2c_master.h` (the modern handle-based
API, not the legacy `driver/i2c.h`). Because that API requires an explicit
per-address "device" handle before you can transact, `i2c_bus` keeps a
small cache (`MAX_CACHED_DEVICES = 16`) mapping address → handle,
created lazily on first use. Same ownership/mutex pattern as `sdi12_bus`.

```c
esp_err_t i2c_bus_init(void);
esp_err_t i2c_bus_write(uint8_t addr7, const uint8_t *wbuf, size_t wlen, uint32_t timeout_ms);
esp_err_t i2c_bus_write_read(uint8_t addr7, const uint8_t *wbuf, size_t wlen,
                              uint8_t *rbuf, size_t rlen, uint32_t timeout_ms);
esp_err_t i2c_bus_scan(uint8_t *found, size_t max_found, size_t *out_count); // via i2c_master_probe
```

`i2c_sensors` is a small **device-type registry** (`i2c_sensor_registry.h`)
dispatching on `variable_config_t.addr.i2c.device_type`:

| Constant | Value | Driver |
|---|---|---|
| `I2C_DEVICE_TYPE_ADS111X` | 0 | TI ADS1113/1114/1115, 16-bit ADC |
| `I2C_DEVICE_TYPE_GENERIC_REG16` | 1 | Raw signed 16-bit big-endian register read, no scaling |

`ads111x_read_channel()` uses a fixed configuration — PGA ±4.096V
full-scale, single-shot mode, 128SPS (`ADS111X_CONVERSION_DELAY_MS = 10`
padding above the ~7.8ms conversion time) — and returns a **voltage in
volts** (`raw / 32768.0 * 4.096`). See [section 19.1](#191-adding-a-new-i2c-sensor-driver)
for how to add another device type.

---

## 10. Sampling and aggregation (`sampling_engine`)

Two scheduling tasks (`sdi12_sched`, `i2c_sched`), one per `bus_type_t`,
wake every `SCHEDULER_TICK_MS = 200`, re-read the variable list from
`config_store`, and — for each enabled variable whose `sample_interval_ms`
has elapsed — call the bus type's registered `sensor_bus_read_fn_t` and
push the result onto a shared `sample_queue`.

`aggregation_task` drains that queue, folding each sample into a per-
variable **Welford accumulator** (`aggregator.c`) — numerically stable
running mean/variance, updated incrementally rather than by re-summing a
buffer of raw samples. When a variable's `log_interval_ms` elapses, the
accumulator is finalized into an `aggregate_result_t` (raw/mean/min/max/
stddev, `sample_count`, timestamp + `time_is_synced` flag) and fanned out
to every registered sink (see [6.2](#62-fan-out-via-result-sinks)), then
reset.

Both scheduling tasks and the aggregation task rebuild their internal
schedule/accumulator tables whenever they observe `config_store`'s
generation counter change (see [7.3](#73-the-generation-counter-pattern)),
so adding/editing/deleting a variable via the portal takes effect within
one scheduling tick — no reboot.

`sampling_engine_read_once(variable_id, &value)` is the synchronous,
one-shot path used by the web portal's Preview button — it bypasses
aggregation entirely and calls straight through to the registered driver.

Registering a driver:

```c
typedef esp_err_t (*sensor_bus_read_fn_t)(const variable_config_t *var, double *out_value);
void sampling_engine_register_bus_driver(bus_type_t bus_type, sensor_bus_read_fn_t read_fn);
```

`app_main.c` registers `sdi12_variable_read`/`i2c_variable_read` on
success, or `stub_sensor_read` (a synthetic sine-wave-plus-noise source,
`components/stub_sensor/`) as a fallback — useful for exercising the whole
sample→aggregate→sink pipeline without any real hardware attached.

---

## 11. SD logging (`sd_logger`)

Mounts the microSD card via `esp_vfs_fat_sdmmc_mount()` in **1-bit SDMMC
mode** (`slot_cfg.width = 1`, pins from `board_pins.h`) — not SPI, despite
that being the more common carrier-board wiring; this board's schematic
uses SDMMC signal names (`SD_CMD`/`SD_CLK`/`SD_DATA0`).

Two independent, day-bucketed CSV file series under `/sdcard/data/`:

- `sensors_YYYYMMDD.csv` — one row per finalized `aggregate_result_t`,
  fed by a queue (`RESULT_QUEUE_LEN = 32`) that's what
  `sd_logger_get_sink_queue()` hands to `sampling_engine_add_result_sink()`.
- `position_YYYYMMDD.csv` — one row per GNSS fix, fed by a second, smaller
  queue (`POSITION_QUEUE_LEN = 4`) via `sd_logger_log_position()`.

Both are serviced by the same `sd_writer_task` (all file I/O confined to
one task), which drains the sensor queue with a bounded wait
(`pdMS_TO_TICKS(5000)`, for periodic watchdog resets) and then
non-blockingly drains any pending position rows each iteration.

Filenames use UTC via `gmtime_r`; unsynced timestamps (before SNTP/cellular
time sync) land in `*_19700101.csv` — a deliberate, well-defined bucket
rather than special-cased handling. Every new file starts with
`# device_id=<id>` before the CSV header row (see
[section 16](#16-device-identification-device_id)).

Card-absent/write-failure resilience: `write_result_row()`/
`write_position_row()` just log an error and increment `s_drop_count`
(exposed via `sd_logger_get_drop_count()`, surfaced in `/api/status`)
rather than blocking or crashing.

---

## 12. Networking (`net_manager`)

### 12.1 Always-on SoftAP policy

`ap_policy.c` is a small state machine, independent of the data-plane
transport:

```
        boot
         │
         ▼
  ┌─────────────┐  AP_BOOT_GRACE_MS elapses, 0 clients   ┌────────┐
  │ BOOT_GRACE  │ ───────────────────────────────────────▶│ AP_OFF │
  │ (AP forced  │                                          └───┬────┘
  │  on, 5 min) │  AP_BOOT_GRACE_MS elapses, ≥1 client         │ client
  └──────┬──────┘ ────────────┐                                │ connects
         │                     ▼                                │
         │ client connects  ┌──────────────────┐◀───────────────┘
         │ (no transition,  │ AP_ACTIVE_BY_    │
         │  just counts)    │ CLIENT           │
         └─────────────────▶│ (on while ≥1     │
                             │  client present) │
                             └────────┬─────────┘
                                      │ last client disconnects
                                      ▼
                                  AP_OFF
```

`AP_BOOT_GRACE_MS = 5 * 60 * 1000` (5 minutes). Client count is tracked via
`WIFI_EVENT_AP_STACONNECTED`/`AP_STADISCONNECTED`. `net_manager.c`'s
`apply_wifi_mode_for_ap_state()` callback translates AP-policy state
changes into actual `esp_wifi_set_mode()` calls, combined with whether STA
is also wanted (`compute_desired_mode()` picks `WIFI_MODE_NULL` / `_AP` /
`_STA` / `_APSTA`). `net_manager_force_ap_on()` is an unused-today
extension point for a future physical button
(`BOARD_PIN_FORCE_AP_BUTTON`).

The AP SSID is `WalterSensor-XXYY` (last two MAC bytes, uppercase hex),
**open** (`WIFI_AUTH_OPEN` — no WiFi password; the portal's own login is
the security boundary), channel 1, max 4 simultaneous stations.

### 12.2 WiFi station mode

Only configured/started when `net_settings.transport == TRANSPORT_WIFI`.
Connects on `WIFI_EVENT_STA_START`, retries on `WIFI_EVENT_STA_DISCONNECTED`
with a fixed `STA_RECONNECT_DELAY_US = 5,000,000` (5s) backoff via a
one-shot `esp_timer` (not a busy-loop), and tracks connected state via
`IP_EVENT_STA_GOT_IP` for `net_manager_sta_is_connected()`.

### 12.3 Captive portal DNS

`web_portal`'s `dns_hijack.c` runs a UDP:53 listener that answers every DNS
query with the AP's own IP (192.168.4.1) — the standard trick that makes
phones/laptops auto-open the login page. It's started unconditionally at
`web_portal_init()` and is harmless when the AP is off (no clients means no
queries reach it).

---

## 13. MQTT (`mqtt_client`)

### 13.1 Dual-backend abstraction

`mqtt_backend.h` defines a small vtable so the rest of the app never
touches a transport-specific MQTT implementation directly:

```c
typedef struct {
    esp_err_t (*init)(const mqtt_settings_t *cfg);
    esp_err_t (*connect)(void);
    esp_err_t (*disconnect)(void);
    esp_err_t (*publish)(const char *topic, const char *payload, int qos, bool retain);
    bool      (*is_connected)(void);
    esp_err_t (*deinit)(void);
} mqttc_backend_vtable_t;
```

`mqttc_init()` selects a backend **once**, based on `net_settings.transport`
at boot (matching the "transport requires a reboot" design decision):
`backend_esp_mqtt.c` (wraps ESP-IDF's `esp-mqtt` over WiFi/lwIP/esp-tls) for
`TRANSPORT_WIFI`, `backend_walter_mqtt.cpp` (wraps `WalterModem`'s on-modem
MQTT client) for `TRANSPORT_CELLULAR`, or `NULL` (fully inert) otherwise.

### 13.2 Immediate vs. batch publish

`mqtt_publish_task` (`mqtt_client_bridge.c`) branches on
`mqtt_settings.batch_enabled`:

- **Immediate (default, `batch_enabled = false`)**: the backend connects
  once (in `apply_settings_if_changed()`, whenever settings actually
  change) and stays connected; each result is published as it arrives.
- **Batch (`batch_enabled = true`)**: results accumulate in a bounded
  buffer (`s_batch_buffer[MAX_BATCH_ITEMS]`, `MAX_BATCH_ITEMS = 256`)
  instead of being published. Once `esp_timer_get_time() >=
  s_next_batch_transmit_us`, the task connects, drains the buffer (and any
  pending position fix — see below), disconnects, and schedules
  `s_next_batch_transmit_us = now + batch_interval_ms`. Enabling batching
  or changing the interval resets the schedule to "now" so the first
  transmission isn't delayed a full interval.

Buffer overflow drops the newest sample (logged) — SD logging is
unaffected either way, since it's a separate sink fed independently by
`sampling_engine`.

### 13.3 Topics and payloads

`publish_result()` builds `<topic_prefix>/<device_id>/<sanitized_name>`
(`sanitize_topic_segment()` replaces `/`, `+`, `#` with `_`, since those
are reserved MQTT topic-level characters). Payload is JSON with `ts`,
`time_synced`, `n` (sample count) always present, plus one field per
aggregate the variable's `aggregate_mask` actually selected: `raw`, `mean`,
`min`, `max`, `stddev`.

`mqttc_publish_position()` (called by `gnss_position`) stores only the
*latest* pending fix (overwriting any not-yet-sent previous one, under
`s_position_mutex`) and is drained by `mqtt_publish_task` alongside regular
samples — so a position fix rides along with the next batch window rather
than opening a second connection. Topic:
`<topic_prefix>/<device_id>/position`, payload `{ts, time_synced, lat, lon,
alt}`.

### 13.4 `backend_esp_mqtt.c`

Uses the documented stable esp-mqtt API (`broker.address.uri`,
`credentials.*`, `broker.verification.crt_bundle_attach` for TLS, or
`skip_cert_common_name_check` for the lab-friendly insecure-TLS toggle).
Not verified against local source in this checkout — see
[section 18](#18-known-limitations-and-low-confidence-areas).

---

## 14. Cellular integration (`cellular_transport`)

Wraps DPTechnics' `dptechnics/walter-modem` C++ library. Because there's
exactly one physical modem, a single `WalterModem` instance is shared
between `cellular_transport.cpp` and `mqtt_client`'s
`backend_walter_mqtt.cpp` via a Meyer's-singleton accessor in a **C++-only**
private header (`cellular_transport_cpp.h` — deliberately not included from
any `.c` file, since `WalterModem` is a C++ type):

```cpp
WalterModem &cellular_transport_get_modem();
```

The public, C-callable API (`cellular_transport.h`) covers registration
status, PDP context state, and GNSS:

```c
esp_err_t cellular_transport_init(void);          // begin() + PDP context activation
bool      cellular_transport_is_registered(void);
bool      cellular_transport_is_pdp_active(void);

esp_err_t cellular_transport_acquire_gnss_fix(gnss_fix_t *out_fix, uint32_t timeout_ms);
void      cellular_transport_get_last_fix(gnss_fix_t *out_fix);
```

`cellular_task` polls registration state every 5 seconds.
`cellular_transport_init()` is only ever called when `net_settings.transport
== TRANSPORT_CELLULAR`.

**⚠️ This entire component is the lowest-confidence part of the firmware.**
`dptechnics/walter-modem` is fetched from the ESP Component Registry at
build time and was never present in the checkout this was developed
against, so no method signature here was checked against a real header —
every WalterModem call (`begin()`, `definePDPContext()`,
`getNetworkRegState()`, `gnssPerformFix()`, and MQTT-related calls in
`backend_walter_mqtt.cpp`) is a best-effort guess from a research summary,
marked `VERIFY` inline. Treat this component as an architectural skeleton
to correct against the real `WalterModem.h` once fetched
(`managed_components/dptechnics__walter-modem/src/WalterModem.h`), not as
tested code.

---

## 15. Position reporting (`gnss_position`)

A single task, spawned only when `net_settings.transport ==
TRANSPORT_CELLULAR` (`gnss_position_init()` is a no-op otherwise — no task
spawned at all). Each cycle:

1. Read `position_settings_t` from `config_store` (checked fresh every
   cycle, so toggling it on/off or changing the interval doesn't need a
   reboot, unlike `transport` itself).
2. If disabled, sleep `IDLE_CHECK_MS = 5000` and recheck.
3. If enabled, call `cellular_transport_acquire_gnss_fix()` (up to
   `GNSS_FIX_TIMEOUT_MS = 60000`), then on success call
   `sd_logger_log_position()` and `mqttc_publish_position()`, then sleep
   `position.interval_ms` before the next cycle.

Not registered with the task watchdog — see
[6.3](#63-task-watchdog-registration).

---

## 16. Device identification (`device_id`)

A tiny, dependency-light component: `device_id_get()` reads the ESP32's
factory-programmed MAC (`esp_read_mac(mac, ESP_MAC_WIFI_STA)`) once, caches
it as a 12-character uppercase hex string (`"AABBCCDDEEFF"`), and returns
the cached pointer on every subsequent call. Used by `sd_logger` (CSV file
header line), `mqtt_client` (topic segment), `web_portal` (`/api/status`),
and logged in the boot banner.

---

## 17. Web portal (`web_portal`)

### 17.1 Structure

```
web_portal/
├── web_portal.c            # httpd_start(), route registration order, SPA catch-all
├── web_portal_internal.h   # private helpers shared across this component's .c files
├── wp_common.c             # JSON response/request helpers, URI id parsing
├── auth.c                  # session table, login/logout, wp_auth_require()
├── dns_hijack.c            # UDP:53 captive-portal DNS responder
├── api_variables.c         # /api/variables CRUD + preview
├── api_settings.c          # /api/settings/* (mqtt, network, position, password, export/import), /api/system/reboot
├── api_status.c            # /api/status
├── api_bus.c                # /api/bus/{sdi12,i2c}/scan
└── assets/index.html       # the entire SPA - embedded into the firmware image
```

### 17.2 Route matching order matters

`esp_http_server` matches registered URI handlers **in registration
order**, first match wins (confirmed from `httpd_find_uri_handler()` in
ESP-IDF source — it's a linear scan, not sorted by specificity). All
`/api/*` routes are registered before the wildcard `GET /*` catch-all that
serves the embedded SPA for anything unmatched — including the OS-specific
captive-portal probe paths (`/generate_204`, `/hotspot-detect.html`,
`ncsi.txt`, ...), since serving the SPA body (rather than each OS's
expected "no portal here" response) is what triggers the captive-browser
popup.

### 17.3 Authentication

Login page + server-side session, not HTTP Basic (avoids retransmitting the
password on every request, and the ugly native browser auth dialog).
`POST /api/login` verifies against the salted SHA-256 hash in
`config_store`, issues a random 16-byte hex token as an `HttpOnly` cookie
named `session` (`Max-Age=1800`). Sessions are a fixed array
(`MAX_SESSIONS = 4`), each with a sliding `SESSION_IDLE_TIMEOUT_US = 30 *
60 * 1000000` (30 min) refreshed on every authenticated request; when all
slots are full, the soonest-to-expire is evicted. Every `/api/*` handler
except `/api/login` calls `wp_auth_require(req)` first, which sends the 401
response itself on failure so the handler can just `return ESP_OK`.

### 17.4 Full REST API reference

All routes except `/api/login` require a valid session cookie.

| Method | Path | Body | Notes |
|---|---|---|---|
| POST | `/api/login` | `{password}` | Sets the session cookie |
| POST | `/api/logout` | — | Clears the session |
| GET | `/api/status` | — | See [section 17.5](#175-status-response-fields) |
| GET | `/api/variables` | — | Array of variables |
| POST | `/api/variables` | variable object (no `id`) | Returns `{id}` |
| PUT | `/api/variables/{id}` | variable object | |
| DELETE | `/api/variables/{id}` | — | |
| POST | `/api/variables/{id}/preview` | — | One-shot read via `sampling_engine_read_once()`, returns `{value}` |
| GET | `/api/bus/sdi12/scan` | — | Array of responding address chars; can take several seconds |
| GET | `/api/bus/i2c/scan` | — | Array of responding 7-bit addresses (numbers) |
| GET/PUT | `/api/settings/mqtt` | `mqtt_settings_t`-shaped JSON | `password` is write-only (never echoed; a `password_set` bool is returned instead) |
| GET/PUT | `/api/settings/network` | `net_settings_t`-shaped JSON | Changing `transport` returns `reboot_required: true` |
| GET/PUT | `/api/settings/position` | `{enabled, interval_ms}` | GET also includes an `available` bool (`transport == cellular`) |
| PUT | `/api/settings/password` | `{old, new}` | `new` must be ≥4 chars |
| GET | `/api/settings/export` | — | Full config JSON, secrets redacted (`portal_password_hash` omitted; `mqtt.password`/`wifi_sta_password`/`cellular_pin` blanked) |
| POST | `/api/settings/import` | Full config JSON | Preserves current secrets for any field left blank/omitted, rather than wiping them |
| POST | `/api/system/reboot` | — | Responds first, then `esp_restart()` after a 500ms `esp_timer` delay |

Variable JSON shape (`config_store_variable_to_json`/`_from_json`):

```json
{
  "id": 1, "name": "soil_temp", "bus_type": 0,
  "addr": {"address": "0", "parameter_index": 0},
  "unit": "degC", "sample_interval_ms": 60000, "log_interval_ms": 300000,
  "aggregate_mask": 3, "calibration_a": 1.0, "calibration_b": 0.0, "enabled": true
}
```

`bus_type`: `0` = SDI-12 (`addr` = `{address, parameter_index}`), `1` = I2C
(`addr` = `{i2c_addr, device_type, channel_index, gain}` - `gain` only
meaningful for `device_type` `I2C_DEVICE_TYPE_ADS111X`). `aggregate_mask` is a
bitmask: `RAW=1, MEAN=2, MIN=4, MAX=8, STDDEV=16` — e.g. `3` = raw + mean.
`calibration_a`/`calibration_b` apply `calibrated = a * raw + b` to every
sample before it reaches the aggregator (see `sampling_engine.c`'s
`bus_scheduler_task` and `sampling_engine_read_once`) - defaults `(1.0, 0.0)`
are a no-op.

### 17.5 Status response fields

`GET /api/status` (see `api_status.c`):

`device_id`, `uptime_s`, `free_heap_bytes`, `ap_active`, `ap_client_count`,
`transport` (`"unconfigured"`/`"wifi"`/`"cellular"`), `data_connection_up`,
`time_synced`, `time_unix`, `sd_ready`, `sd_total_bytes`, `sd_free_bytes`,
`sd_drop_count`, `mqtt_enabled`, `mqtt_batch_enabled`, `mqtt_connected`,
`position_enabled`, `position_available` — plus, only when
`transport == "cellular"`: `position_fix_valid` and (if valid)
`position_latitude`/`position_longitude`/`position_altitude_m`/
`position_timestamp_unix` — and finally `variable_count`,
`config_generation`.

### 17.6 The SPA

`assets/index.html` is a single self-contained file (inline `<style>` and
`<script>`, no build step, no framework) embedded into the firmware binary
via CMake's `EMBED_FILES` (symbol name derived from the file's *basename*
only, regardless of subdirectory — `_binary_index_html_start`/`_end`).
This was a deliberate simplicity choice given no OTA/no runtime-asset-
update requirement in v1; revisit with a LittleFS asset partition only if
the SPA outgrows embedding in the firmware image.

---

## 18. Known limitations and low-confidence areas

Ranked roughly by how much you should distrust them before relying on
them in a real deployment:

1. **`cellular_transport` and `mqtt_client/backend_walter_mqtt.cpp`**
   (lowest confidence) — every `WalterModem` call is an unverified guess.
   See [section 14](#14-cellular-integration-cellular_transport).
2. **`sdi12_bus`** — pin numbers are confirmed from the schematic, but bit
   timing and enable-pin polarity are not verified against real hardware.
3. **`mqtt_client/backend_esp_mqtt.c`** — written against documented
   stable esp-mqtt API shape, but this checkout's ESP-IDF was missing the
   `mqtt/esp-mqtt` submodule source, so it couldn't be checked against a
   real header either.
4. **`sdi12_measure_and_read()`** only issues `aD0!`, not `aD1!..aD9!` — a
   known, intentional v1 limitation for sensors returning many values.
5. **`board_pins.h`**: SD card-detect, status LED, and force-AP-button
   pins are still unset placeholders (nothing on the available schematic
   pages showed them).
6. **No OTA** — reflashing requires physical USB access.
7. **MQTT batch buffer** (`MAX_BATCH_ITEMS = 256`) can overflow (drops
   silently, logged) if you combine many variables, short log intervals,
   and a long batch interval — see the sizing note in
   [USER_MANUAL.md §7.4](USER_MANUAL.md#74-batch-transmission-saving-power).

---

## 19. Testing without Walter Feels hardware

If you don't have a Walter/Walter Feels board yet, you can still exercise
the sensor pipeline, the config store, and the web portal end-to-end on a
plain **ESP32 DevKit V1** wired to one or more I2C sensors (e.g. two
ADS1115 ADC boards) — SDI-12, SD card, and cellular are simply left
disabled, exactly like they would be on real hardware with those pins
unset (see [section 8](#8-board-pin-mapping-board_pins)).

### 19.1 Why this needs more than flashing the same firmware

Two things differ, not just the board_pins.h values:

- **Chip target**: the DevKit V1 uses a classic **ESP32** (Xtensa LX6),
  not the **ESP32-S3** the rest of this project targets. Some existing
  `board_pins.h` GPIO numbers (e.g. 40-43) don't even exist on classic
  ESP32, and the S3-specific octal-PSRAM/USB-Serial-JTAG sdkconfig
  settings don't apply either.
- **Pin mapping**: Walter Feels' SDI-12/I2C/SD pins are meaningless on a
  bare DevKit — there's no SN74LV1T126 buffer, no SDMMC wiring, nothing.

Both are handled by two independent switches you set together:

| Switch | How | Affects |
|---|---|---|
| Chip target | `idf.py set-target esp32` | Which `sdkconfig.defaults.<target>` file gets merged (`sdkconfig.defaults.esp32` vs. `sdkconfig.defaults.esp32s3`) — flash size, PSRAM, console. |
| Board variant | `idf.py menuconfig` → "Walter Sensor Node Board Selection" → "Generic ESP32 DevKit V1" (`CONFIG_BOARD_VARIANT_ESP32_DEVKIT_TEST`) | Which `#if` branch of `board_pins.h` is compiled — see `components/board_pins/Kconfig.projbuild`. |

They're independent settings (nothing stops you from selecting the wrong
combination), but only `esp32` + `ESP32_DEVKIT_TEST` and `esp32s3` +
`WALTER_FEELS` are meaningful pairings.

### 19.2 Step by step

```sh
idf.py set-target esp32
idf.py menuconfig   # Walter Sensor Node Board Selection -> Generic ESP32 DevKit V1
idf.py build
idf.py -p <PORT> flash monitor
```

Wiring, for two ADS1115 boards sharing the bus (I2C requires each device
on a bus to have a distinct address):

- Both boards: `SDA` → GPIO21, `SCL` → GPIO22, `VDD`/`GND` → the DevKit's
  3.3V/GND (see `BOARD_PIN_I2C_SDA`/`BOARD_PIN_I2C_SCL` in `board_pins.h`
  for this variant).
- Give each board a distinct address. On the generic ADS1115 chip
  (TI datasheet), this is done by tying the `ADDR` pin to `GND`→`0x48`,
  `VDD`→`0x49`, `SDA`→`0x4A`, or `SCL`→`0x4B`. Some breakout boards
  instead use their own onboard solder jumpers with a different
  mapping - e.g. Soldered's ADS1115 breakout defaults to `0x48` with no
  jumper closed, and remaps to `0x39`/`0x4A`/`0x4B` via jumpers JP3/JP4/JP5
  respectively (**close only one jumper at a time** - the board's own
  docs warn that closing more than one simultaneously can cause a
  malfunction). Either way, confirm what your specific boards actually
  respond at with the portal's **Scan I2C bus** button rather than
  assuming.

Then in the portal, add one variable per ADC input you want to read: bus
type I2C, the I2C address you found via the scan (in decimal — e.g. `72`
for `0x48`), device type `0` (ADS111x), and pick the input from the
channel dropdown — both single-ended (`AIN0`-`AIN3`) and differential
pairs (`AIN0-AIN1`, `AIN0-AIN3`, `AIN1-AIN3`, `AIN2-AIN3`) are available.
E.g. for differential AIN0-AIN1 and AIN2-AIN3 on each of two boards,
that's four variables total (one per pair per board).

Each variable also has its own **gain / full-scale range** setting
(`+/-6.144V` down to `+/-0.256V`) — pick the smallest range that still
comfortably covers that signal's expected peak, for the best resolution.
Two different variables on the same or different ADS1115 boards can use
different gains (e.g. a 0–5V signal at `+/-6.144V` and a 0–1.5V signal at
`+/-2.048V` on the same chip, on different inputs). **This setting does
not change the chip's absolute maximum input voltage**, which is always
its own supply voltage (`VDD`) + 0.3V regardless of gain — feeding a 0–5V
signal into an ADS1115 powered from the DevKit's 3.3V rail will exceed
that limit no matter what gain is selected; that board needs to be
powered at 5V instead (or the signal scaled down with a divider first).

### 19.3 Component-manager target gating (already handled)

`main/idf_component.yml`'s `dptechnics/walter-modem` dependency is gated
with a component-manager `rules: - if: "target == esp32s3"` clause (that
package only publishes versions for `esp32s3`), and `cellular_transport`'s
`CMakeLists.txt` only adds it to `REQUIRES` `if(CONFIG_IDF_TARGET_ESP32S3)`.
The WalterModem-dependent function bodies in `cellular_transport.cpp` and
`backend_walter_mqtt.cpp` are likewise wrapped in
`#if CONFIG_IDF_TARGET_ESP32S3 ... #else ... #endif`, falling back to
`ESP_ERR_NOT_SUPPORTED` stubs on other targets — so `idf.py set-target esp32`
resolves and builds cleanly without ever touching the modem SDK. This was
confirmed by an actual `esp32` build, not just inferred from the manifest.

---

## 20. How to extend the firmware

### 20.1 Adding a new I2C sensor driver

1. Pick an unused `device_type` number and add a
   `#define I2C_DEVICE_TYPE_<NAME> <n>` to
   `components/i2c_sensors/include/i2c_sensor_registry.h`.
2. Add a new source file, e.g. `components/i2c_sensors/<name>.c`, with a
   function `esp_err_t <name>_read(uint8_t i2c_addr, uint8_t channel, double *out_value)`
   that calls `i2c_bus_write()`/`i2c_bus_write_read()` (see `ads111x.c` for
   the reference shape — register write, delay if needed, register read,
   decode).
3. Declare that function in `i2c_sensor_registry.h` and add a `case` for
   your new `device_type` in `i2c_sensor_read()`
   (`components/i2c_sensors/i2c_sensor_registry.c`).
4. Add the new `.c` file to `components/i2c_sensors/CMakeLists.txt`'s
   `SRCS` list.
5. In the portal, students select your new driver via its numeric
   `device_type` in a variable's I2C settings (no UI changes needed unless
   you want a nicer label — see `assets/index.html`'s `variableFormFields()`
   for where device type is currently a bare number input).

### 20.2 Adding a new REST endpoint

1. Add your handler function to the relevant `api_*.c` file (or create a
   new one, following `api_bus.c` as a minimal example).
2. Register it in that file's `..._register_routes(httpd_handle_t server)`
   function — **register specific routes before any wildcard/catch-all**
   (see [17.2](#172-route-matching-order-matters)).
3. If it's a new file, declare the register function in
   `web_portal_internal.h`, call it from `web_portal.c`'s `web_portal_init()`,
   and add the `.c` file to `components/web_portal/CMakeLists.txt`.
4. Use `wp_auth_require(req)` at the top unless the endpoint is
   intentionally public, and the `wp_send_json`/`wp_send_error`/
   `wp_read_json_body` helpers from `wp_common.c` for consistent response
   shapes.

### 20.3 Adding a new config field

1. Add the field to the appropriate struct in
   `components/config_store/include/config_schema.h`.
2. Set its default in `config_set_defaults()` (`config_store.c`).
3. Add it to both `config_store_to_json()` and the corresponding
   `_from_json()` function for that struct, so NVS persistence and the
   REST API pick it up automatically (they share this code — see
   [7.2](#72-persistence)).
4. If it needs its own GET/PUT exposure, extend the relevant
   `api_settings.c` handler.
5. If a component needs to react to it changing at runtime, read it via
   the generation-counter pattern ([7.3](#73-the-generation-counter-pattern))
   rather than polling `config_store` on every operation.

---

## 21. Glossary

| Term | Meaning |
|---|---|
| ESP-IDF | Espressif's official SDK for ESP32 chips — the framework this firmware is built on (as opposed to the Arduino framework). |
| FreeRTOS | The real-time operating system ESP-IDF runs on; provides tasks, queues, mutexes, semaphores. |
| Task | FreeRTOS's unit of concurrent execution — roughly analogous to a thread. |
| Queue | A FreeRTOS inter-task communication primitive — fixed-size, thread-safe, used throughout this firmware to pass data between tasks without shared-memory races. |
| Mutex / semaphore | Synchronization primitives used to guard shared state accessed from more than one task. |
| NVS | Non-Volatile Storage — ESP-IDF's flash-based key-value store, used here to persist the whole config as one JSON blob. |
| Component | ESP-IDF's unit of modular code — roughly a library, with its own `CMakeLists.txt` declaring sources, public headers, and dependencies (`REQUIRES`). |
| `idf_component.yml` | Declares a managed dependency fetched from the ESP Component Registry at build time (used here for `dptechnics/walter-modem`). |
| GPIO matrix | ESP32's ability to route almost any peripheral signal (UART, SPI, I2C, ...) to almost any physical pin, rather than fixed pin assignments. |
| Task watchdog (TWDT) | ESP-IDF's mechanism to detect a hung task (one that hasn't "checked in" within a timeout) and panic/reboot rather than silently lock up. |
| Welford's algorithm | A numerically stable method for computing running mean/variance incrementally, without needing to store or re-sum all prior samples. |
| PDP context | A cellular data-session concept (from 3GPP) — roughly "the data connection the modem has activated," analogous to a WiFi association + DHCP lease. |
| SoftAP | The ESP32 acting as its own WiFi access point (hotspot), as opposed to "station" (STA) mode, joining someone else's network. |
| Captive portal | The auto-popping "sign in to this network" browser flow triggered by a DNS-hijacking/probe-endpoint trick, used here for first-time setup. |
| cJSON | The C JSON library used throughout this firmware for (de)serialization, both for NVS persistence and the REST API. |
| Vtable (virtual table) | A struct of function pointers used here to let `mqtt_client` swap between two unrelated MQTT implementations (`esp-mqtt` vs. `WalterModem`) behind one interface. |
