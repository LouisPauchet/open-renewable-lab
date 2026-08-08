# Getting started: a 20 W RS PRO solar panel on the Open Renewable Lab

This walkthrough takes you from an unwired RS PRO 20 W solar panel to live
`PV_Voltage` and `PV_Current` readings in ThingsBoard. It assumes you're
starting from a fresh Open Renewable Lab enclosure (see [Hardware](hardware.md)
for what's inside it) with firmware already flashed — if not, do
[Firmware downloads](firmware-downloads.md) first.

No programming is required for any of this — everything below is either
physical wiring or the device's own web portal, described in full in the
[User Manual](USER_MANUAL.md). This page only covers the parts specific to
a PV panel; it links to the User Manual for general portal reference rather
than repeating it.

## What you'll need

- An RS PRO 20 W solar panel (or similar small panel — the steps are the
  same regardless of exact wattage).
- An Open Renewable Lab enclosure, powered on **with a battery connected
  to BATT**. This isn't optional: the AN1/AN2 analog inputs (and every
  other external I2C device) are powered from a rail derived from the
  battery/PV input, not from USB — running on USB alone, no battery, the
  ADS1115 simply won't be powered and Preview will fail with no obvious
  reason why.
- A multimeter (for the voltage calibration step).
- A phone or laptop with WiFi, to reach the device's setup portal.
- A [ThingsBoard](https://thingsboard.io/) account — either your
  instructor's own instance, or the public
  [demo.thingsboard.io](https://demo.thingsboard.io/) sandbox for testing.

## ⚠ Safety note

Unlike a bench power supply, a solar panel has **no off switch** — any
panel in daylight (even indoor room light will produce a small voltage) is
live the moment it's wired up. Keep the panel covered with an opaque cloth
or bag while making connections, and only uncover it once wiring is
complete and double-checked.

## 1. How the analog input works

⚠ **Not the PV gland.** The enclosure also has a connector labeled **PV**
— that one charges the *logger's own* operating battery from a small
auxiliary panel (straight into the onboard LTC4015 charger), and is
unrelated to measuring your panel. Your instrumented RS PRO panel plugs
into **AN1** or **AN2** instead (see
[Hardware](hardware.md#connector-reference)) — pick whichever is free;
this guide uses AN1.

Each of AN1/AN2 is a generic 4-pin analog input exposing two of the
external ADS1115 ADC's four single-ended channels at I2C address **73**
(`0x49`) — AN1 is `AIN0`/`AIN1`, AN2 is `AIN2`/`AIN3`. The instrumented
panel's own 4-pin plug carries both the voltage-divider output and the
current-shunt output, one per channel:

| Signal | AN1 channel | Circuit | What it measures |
|---|---|---|---|
| PV voltage | `AIN0` | Resistive divider, designed to scale a panel voltage up to roughly the divider's rated ~22 V down to the sensor's ~5 V input range | The panel's terminal voltage |
| PV current | `AIN1` | A 1 Ω 0.5% shunt resistor in the panel's current path | The panel's output current — by design, **1 V measured = 1 A** |

(Using AN2 instead just means `AIN2` for voltage and `AIN3` for current
below — everything else is identical.)

## 2. Connect the panel

1. With the panel still covered, plug its instrumented 4-pin connector into
   the enclosure's **AN1** gland (or **AN2** — just be consistent with
   which channels you configure in [step 3](#3-add-the-two-variables-in-the-portal)).
2. Double-check the plug is fully seated before uncovering the panel — a
   reversed/miswired current path won't damage the sensing circuit at
   20 W scale, but will show up as a negative reading (see
   [step 4](#4-preview-and-calibrate)).
3. Uncover the panel once you're done.

## 3. Add the two variables in the portal

Connect to the device's `WalterSensor-XXXX` hotspot and log in (see
[USER_MANUAL.md §3-4](USER_MANUAL.md#3-quick-start) if this is your first
time). Under **Variables**, add two new I2C variables using the field
reference in [USER_MANUAL.md §6.1](USER_MANUAL.md#61-adding-a-variable):

**PV_Voltage**

| Field | Value |
|---|---|
| Name | `PV_Voltage` |
| Bus type | `I2C` |
| I2C address | `73` |
| Device type | `ADS111x` |
| ADS111x input | `AIN0` |
| Gain / full-scale range | `±6.144V` to start (the divider's designed output tops out around 5 V) |

**PV_Current**

| Field | Value |
|---|---|
| Name | `PV_Current` |
| Bus type | `I2C` |
| I2C address | `73` |
| Device type | `ADS111x` |
| ADS111x input | `AIN1` |
| Gain / full-scale range | `±2.048V` to start (a 20 W-class panel's short-circuit current stays well under 2 A, so under 2 V across a 1 Ω shunt) |

If the address `73` doesn't respond, use the portal's **Scan I2C bus**
button ([USER_MANUAL.md §6.2](USER_MANUAL.md#62-finding-your-sensors-address))
to confirm what's actually on the bus before assuming a wiring problem.

<figure markdown>
![Portal Variables list showing three configured variables](pictures/Portal_Variables.png)
<figcaption>
The Variables list after adding variables — each row shows its bus,
address, sample/log intervals, and aggregates, with Preview/Edit/Delete
per row (this example shows the three onboard environment variables that
ship enabled by default, not PV_Voltage/PV_Current — yours will look the
same shape once you've added those two).
</figcaption>
</figure>

## 4. Preview and calibrate

Leave both variables' calibration at the default (`a=1`, `b=0`) for now,
then click **Preview** on each (see
[USER_MANUAL.md §6.4](USER_MANUAL.md#64-testing-a-sensor-preview)) with the
panel uncovered in reasonable light.

### PV_Current: usually needs no calibration

Because the sensing resistor is a 1 Ω shunt, the raw ADC reading in volts
already **is** the current in amps — `a=1, b=0` is correct as-is, this
isn't a placeholder. If Preview shows a **negative** value, the shunt is
seeing current flow in the direction opposite of what the firmware assumes;
either physically swap the panel's leads, or set `a=-1` in the portal to
flip the sign in software (see
[USER_MANUAL.md §6.6](USER_MANUAL.md#66-calibrating-a-variable-ax--b)).

### PV_Voltage: calibrate against a multimeter

The divider's exact ratio depends on the resistors actually populated on
your board, so rather than trust a fixed number, measure it directly:

1. With the panel connected and in light, measure its voltage directly
   with a multimeter across the PV+/PV− leads. Call this `V_multimeter`.
2. Read the **raw, uncalibrated** value from the portal's Preview
   (`a=1, b=0` still set). Call this `V_raw`.
3. Set the calibration multiplier: `a = V_multimeter / V_raw` (leave
   `b = 0`).

**Worked example**: a multimeter reads `21.7 V` across the panel; the
portal's raw Preview shows `4.93 V`. Then
`a = 21.7 / 4.93 ≈ 4.40`, entered as the PV_Voltage calibration multiplier.
(This lines up with the divider's design target of scaling up to ~22 V down
to a ~5 V sensor range — expect `a` to land somewhere around 4-5 on this
board, but always calibrate against your own multimeter reading rather than
assuming that exact number.)

Re-run Preview after saving the calibration and confirm it now matches your
multimeter reading.

## 5. Set up MQTT → ThingsBoard

1. In ThingsBoard, create a device (**Devices → +  → Add new device**),
   give it a name (e.g. `PV-Panel-01`), and save.
2. Open the device and copy its **access token** (device details →
   **Device credentials** → **Copy access token**, or in older ThingsBoard
   UIs, right-click the device → **Copy access token**).
3. Back in the Open Renewable Lab portal, under **MQTT**
   ([USER_MANUAL.md §7.1](USER_MANUAL.md#71-basic-settings)):

   | Field | Value |
   |---|---|
   | Enabled | ✓ |
   | Host | Your ThingsBoard instance's hostname (e.g. `demo.thingsboard.io`) |
   | Port | `1883` (or `8883` with **Use TLS** checked, if your instance requires it) |
   | Username | The device access token you copied |
   | Password | Leave blank |
   | Topic prefix | `v1/devices/me/telemetry` |
   | Flat telemetry topic | ✓ |

   <figure markdown>
   ![Portal MQTT settings form](pictures/Portal_MQTT.png)
   <figcaption>
   The portal's MQTT settings form — the fields above map directly onto
   this.
   </figcaption>
   </figure>

   Save, then watch **MQTT connected** on the Status panel — see
   [USER_MANUAL.md §7.2](USER_MANUAL.md#72-testing-your-connection) if it
   doesn't come up.
4. While testing, you can temporarily shorten `PV_Voltage`/`PV_Current`'s
   **Log/aggregate interval** (e.g. to `30000` ms) so you don't have to
   wait for the default 5-minute interval to see your first MQTT publish —
   see [USER_MANUAL.md §6.3](USER_MANUAL.md#63-understanding-sampling-vs-logging-vs-aggregation).
   Set it back to a sensible value once you've confirmed everything works.

## 6. Confirm data in ThingsBoard

Open the device in ThingsBoard and check its **Latest telemetry** tab. Once
a log interval has elapsed, you should see keys like `PV_Voltage_mean` and
`PV_Current_mean` (one `<variable>_<aggregate>` key per aggregate you
enabled on each variable — see
[USER_MANUAL.md §7.3](USER_MANUAL.md#73-understanding-topics-and-payloads)
for the full payload shape). From here, you can build a ThingsBoard
dashboard with widgets charting these values in real time — see
ThingsBoard's own documentation for adding widgets to a dashboard.

Your data is also being logged to the SD card the whole time, independent
of MQTT — see [USER_MANUAL.md §10.1](USER_MANUAL.md#101-on-the-sd-card) if
you want a local backup or to analyze it in pandas/Excel later.

## Troubleshooting

See [USER_MANUAL.md §15](USER_MANUAL.md#15-troubleshooting) for general
issues (sensor not responding, MQTT never connecting, etc.). PV-specific
notes:

- **Preview fails / Scan I2C bus finds nothing at all**, on *every*
  address, not just `73`: check there's a **battery connected to BATT**.
  The external I2C bus (everything on AN1/AN2, and BATT's own LTC4015)
  is powered from a battery/PV-derived rail — running the board on USB
  alone with no battery, that whole bus is simply unpowered, which looks
  identical to "nothing responds." This is expected in that situation,
  not a fault.
- **PV_Voltage Preview reads ~0** with the panel in good light (and a
  battery connected): check the panel's plug is actually in **AN1** (not
  the **PV** gland, which won't measure anything — see
  [step 1](#1-how-the-analog-input-works)) and that the ADS111x input
  selected in the variable matches the connector you used (`AIN0`/`AIN1`
  for AN1, `AIN2`/`AIN3` for AN2), and that the I2C address is `73`.
- **Readings look reasonable but don't match your multimeter**: re-check
  the calibration multiplier from [step 4](#4-preview-and-calibrate) — a
  stale `a` from before you calibrated is the most common cause.
