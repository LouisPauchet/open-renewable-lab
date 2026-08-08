# Firmware downloads

Every [GitHub Release](https://github.com/<OWNER>/<REPO>/releases) publishes
ready-to-flash firmware binaries automatically — you do **not** need to
install ESP-IDF or build anything to get firmware onto a board. Pick one of
the first two options below; the third is only for people modifying the
firmware source.

## Option 1: Flash from your browser (easiest)

Works in **Chrome or Edge** (desktop) via WebSerial — nothing to install.

1. Plug the board into your computer over USB.
2. Click the button below and follow the prompts (pick the right serial
   port, then "Install").

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js"></script>

<esp-web-install-button manifest="../manifest.json">
  <button slot="activate">Connect and flash latest firmware</button>
  <span slot="unsupported">
    Your browser doesn't support WebSerial — use Chrome or Edge, or see
    Option 2 below.
  </span>
  <span slot="not-allowed">
    This page needs to be served over HTTPS to flash (the GitHub Pages
    site already is) — if you're seeing this locally, use Option 2.
  </span>
</esp-web-install-button>

This always flashes the **latest published release**. It erases the whole
flash (including any saved portal configuration) and writes the merged
firmware image in one shot, starting fresh — see
[USER_MANUAL.md](USER_MANUAL.md) afterwards for first-boot setup.

## Option 2: Flash with `esptool` (no ESP-IDF)

1. Install [`esptool`](https://docs.espressif.com/projects/esptool/en/latest/esp32/):
   ```sh
   pip install esptool
   ```
2. Download `walter_sensor_node.merged.bin` from the
   [latest release](https://github.com/<OWNER>/<REPO>/releases/latest).
3. Flash it to offset `0x0` (it already contains the bootloader, partition
   table, and application — no other files or offsets needed):
   ```sh
   esptool.py --chip esp32s3 --port <PORT> write_flash 0x0 walter_sensor_node.merged.bin
   ```
   Replace `<PORT>` with your board's serial port (e.g. `COM5` on Windows,
   `/dev/ttyUSB0` on Linux, `/dev/cu.usbserial-*` on macOS).

Each release also publishes the three components separately
(`bootloader.bin`, `partition-table.bin`, `walter_sensor_node.bin`) and a
`SHA256SUMS` file, in case you need to flash/verify them individually.

## Option 3: Build from source

Only needed if you're modifying the firmware itself. Requires installing
ESP-IDF v6.0.2 — see
[DEVELOPER_GUIDE.md §2](DEVELOPER_GUIDE.md#2-prerequisites-and-build).

## Which release should I use?

Use the [latest release](https://github.com/<OWNER>/<REPO>/releases/latest)
unless your instructor tells you otherwise (e.g. to match a specific lab
session or reproduce a known-good setup).
