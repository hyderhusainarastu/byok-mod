# Firmware

This is the custom ESP32-S3 firmware this project builds: how it is structured, how to build it
with ESP-IDF, how to package it into the vendor's own update-archive format, and how to get it onto
the device through the stock SD-card updater — the only supported install path (see [LEGAL.md](../LEGAL.md) and
[SAFETY.md](../SAFETY.md); this project never installs firmware over `esptool`, whose role here is
read-only identification and backup plus the one reserved recovery write of
[recovery.md](recovery.md) §4).

The co-processor ("Pico") side is not touched by anything in this document or by this project's own
build: every release ships without a co-processor image, which keeps that chip, and its Bluetooth
stack, completely untouched by any install this project performs.

---

## 1. Scope: which chip, and why

The custom firmware in this repository targets the **ESP32-S3 only**. The device's Bluetooth-HID
co-processor ("Pico" — itself a second ESP32, not an RP2040, despite the product name) owns
everything Bluetooth-related and nothing else; the S3 owns the display, USB, SD card, and every
protocol this firmware implements. See [architecture.md](architecture.md) for the full chip-split evidence.

---

## 2. Directory layout

```
firmware/
  common/
    hw_config.h        shared interoperability constants (see docs/display.md, docs/pinout.md)
    byok_proto/          wire-frame encoder/decoder for the host link (docs/protocol.md)
    byok_tar/             minimal POSIX-tar reader used by the on-device SD updater
  s3/
    CMakeLists.txt        top-level project file
    sdkconfig.defaults    the only sdkconfig this repo ships or builds from (see §3)
    partitions.csv         reproduces this project's own unit's verified partition table;
                           never flashed — see docs/flash-layout-and-updater.md
    main/
      app_main.c            boot sequence + protocol dispatcher (see §5)
      Kconfig.projbuild      project-level build options (see §3)
    components/
      byok_display/          panel driver (docs/display.md) + boot self-test
      byok_hw_shim/           surfaces every unresolved hardware constant as a Kconfig-visible,
                              loud-boot-warning placeholder rather than a silent guess
      byok_usb_cdc/           TinyUSB CDC-ACM transport for the host link
      byok_sd_updater/        the on-device half of the install path in §7
      byok_battery/           battery/charger ADC and status-line reporting
      byok_power/             WAKE-hold power-off task, boot-time power latch
      byok_rtc/                PCF8563 real-time-clock driver
      byok_clock/               clock idle-screen renderer
      byok_note/                static-note idle-screen storage/renderer
      byok_idle/                idle-submode state machine + button-hold handling
      byok_menu/                on-device preset menu
      byok_modes/                application-mode state machine
      byok_nvs/                  this firmware's own NVS namespace (separate from the vendor's)
      byok_docstats/              read-only background SD-card writing-stats scan
      byok_sysbus/                 shared inter-component signalling
  pico/                    reserved for a future co-processor build; empty in this release
```

Every `byok_*` component name, and the `byok` CLI/config-symbol prefix (`CONFIG_BYOK_*`) throughout
this tree, is this project's own naming and is unrelated to, and not affiliated with, the vendor's
own product or firmware — see [LEGAL.md](../LEGAL.md) §2 for the trademark position.

---

## 3. Build-time gates

This firmware is deliberately conservative about which of its capabilities can touch flash the
device's bootloader treats as sensitive. Three gates control that, and they are **independently
switchable** on purpose — arming one does not arm the others:

| Gate | Kconfig symbol | Default | What it actually allows |
|---|---|---|---|
| Boot-time revert-to-stock, and marking the running app "valid" | `CONFIG_BYOK_ALLOW_OTADATA_WRITE` | `n` | With this off, a 3-second boot-time hold of the EXECUTE button, and the protocol's own revert-to-stock command, both run their full lookup and validation and then stop short of the actual write — logged, not silently ignored. |
| The SD-card updater's own OTA write | `CONFIG_BYOK_SD_UPDATER_ARMED` | `n` (component default); **`y`** in this repo's `sdkconfig.defaults` | With this on, a validated update archive found on the SD card at boot is actually installed. This is deliberately a *separate* gate from the one above — see the components' own `Kconfig` help text for the full rationale. |
| NVS persistence for this firmware's own settings (idle submode, backlight level, presets, static-note text) | `CONFIG_BYOK_NVS_PERSIST_ARMED` | `n` (component default); **`y`** in this repo's `sdkconfig.defaults` | With this on, those settings survive a reboot in this firmware's own NVS namespace, distinct from — and never reading or writing — any of the vendor's own NVS namespaces sharing the same physical partition. |

A fourth, non-gating setting worth knowing about: `CONFIG_BYOK_USB_CDC_MIN_UPTIME_MS` (default 5000)
holds the native USB-OTG PHY hand-over (which this firmware's own host link needs) behind a minimum
boot-time delay, specifically so the ESP32-S3's separate USB-Serial-JTAG interface — the only route
to `esptool`'s ROM-download recovery mode on this design, since this project never burns Secure Boot
or otherwise changes the boot chain — stays reachable for a known, fixed window on every boot,
independent of how long the display self-test happens to run. See §9.

`sdkconfig.defaults` is the only sdkconfig this repository tracks or builds from; a generated
`sdkconfig` is a build artifact and is never committed — always rebuild from `sdkconfig.defaults`
rather than hand-editing a stale generated file.

---

## 4. Building

This firmware is built with **ESP-IDF v5.5.3** — the same major/minor/patch line the vendor's own
shipped firmware uses (see [environment.md](environment.md) for one fully worked-through, verified-working
toolchain setup on macOS/Apple Silicon).

```sh
git clone --depth 1 --branch v5.5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
source ~/esp/esp-idf/export.sh

cd firmware/s3
idf.py set-target esp32s3
idf.py build
```

The first build (or `idf.py reconfigure`) pulls one managed dependency (`espressif/esp_tinyusb`,
pinned to an exact version in `main/idf_component.yml`) via the ESP-IDF Component Manager, which
needs network access once; it is cached under `managed_components/` after that.

**`idf.py flash`, `idf.py monitor` with auto-reset, or any bare `esptool.py write_flash` /
`erase_flash` / `erase_region` call must never be run against a real device with this project.**
`idf.py build` alone (no `flash` target) never touches a device and is always safe. The partition
table in `partitions.csv` exists so `idf.py build`'s own size checks agree with the table already on
a real device's flash — it is a `CONFIG_PARTITION_TABLE_CUSTOM` build input only, and this project's
own hard rule ([SAFETY.md](../SAFETY.md)) forbids ever flashing it. See §6/§7 for the only supported way this
firmware reaches a device.

---

## 5. Boot sequence, briefly

The full sequence — every step in source order, with the reasoning for why each sits where it
does — is diagrammed at
[`diagrams/mod-firmware-boot-flow.md`](diagrams/mod-firmware-boot-flow.md). In outline:

1. Drive the power-latch GPIO low (the very first action of all — this is what keeps the device
   powered once the front button is released).
2. Reproduce the vendor's own early boot-time GPIO output block, for stock parity.
3. Hand the USB PHY back to the USB-Serial-JTAG controller, before anything else, so the
   `esptool` recovery window (§9) is open for the *whole* boot, not just part of it.
4. Sample the boot-time revert-to-stock button gesture (§3's `CONFIG_BYOK_ALLOW_OTADATA_WRITE` gate).
5. Start the WAKE-hold power-off task — early, so it remains available even if a later boot step
   stalls.
6. Start the battery/charger monitor.
7. Log an identity banner (version, IDF version, build date, running partition, reset reason, and
   the state of all three gates in §3) and any unresolved-hardware warnings (`byok_hw_shim`).
8. Run the SD-card updater check (§7) — installs and reboots immediately if an update archive is
   present and armed; otherwise falls through with nothing drawn yet.
9. Bring up the display and run the boot self-test pattern ([display.md](display.md)), left showing the
   resting screen.
10. Initialize NVS, the note/menu/idle subsystems, the real-time clock, and the writing-stats
    scanner, in a load-bearing order (state must be loaded before the tasks that read it start).
11. Wait out the minimum-uptime gate (§3), then hand the USB-OTG PHY to this firmware's own
    CDC-ACM host link and start the protocol dispatch task.

---

## 6. Packaging an update archive

`scripts/make-update-tar.sh` builds a vendor-format update archive (`BYOK.tar`) on the host — it
never touches a serial device, never calls `esptool` against a `--port`, and never writes to an SD
card; it only reads local files and writes to the directory passed to `--out`.

**The vendor's stock firmware release is never shipped in this repository** ([LEGAL.md](../LEGAL.md) §3) — you
supply your own, obtained directly from the maker. The script enforces this: its `--stock-from`
option names the vendor release archive to source the co-processor image and the Lua asset bundle
from (both left untouched by this project). Without `--stock-from`, the script looks for exactly
one file matching a version-pinned pattern under a local, git-ignored staging directory, and **fails
with a clear, listed error** — the exact directory it searched, and what it found there — rather
than silently doing nothing or picking an unrelated file. There is no working default that requires
no input from you: you always point the script at your own copy of the vendor's release.

```sh
scripts/make-update-tar.sh \
    --s3 firmware/s3/build/byok_mod_s3.bin \
    --no-pico \
    --stock-from <path to your own vendor release archive> \
    --out <output directory>
```

`--no-pico` builds a **two-member** archive — the custom S3 image plus the vendor's own Lua asset
bundle, with the co-processor image omitted entirely. Omitting it means the on-device updater never
contacts that chip at all: no GPIO toggle, no UART transfer, no write to its flash. This project
treats that chip as out of scope for any install it performs, and two-member archives are what every
release in [CHANGELOG.md](../CHANGELOG.md) ships.

The script refuses to package an S3 image that is not a valid, correctly-chipped, valid-checksum
application image under the OTA slot's size limit, verifies the vendor archive you pass it against
a checksum manifest sitting alongside it (skippable only with an explicit flag, for a deliberately
unlisted archive), and writes the resulting `BYOK.tar` alongside its own hash manifest and a
byte-for-byte comparison against the stock archive's members, so you can see exactly what changed
and what did not before it ever reaches an SD card.

---

## 7. Installing via the stock SD-card updater

This project never installs firmware over `esptool`. Instead it uses the vendor's own boot-time SD
updater — the same mechanism the vendor's own official updates use — which the device already
trusts and which needs no bootloader, partition-table, or eFuse change of any kind.

The stock updater's full trigger-to-reboot behaviour, decoded from the vendor's own image, is
diagrammed at [`diagrams/sd-updater-flow.md`](diagrams/sd-updater-flow.md). The essential facts:

- The updater is triggered purely by the **existence** of `/SDCARD/Updates/BYOK.tar` on the card at
  boot — not its contents, size, or any signature. There is no vendor cryptographic signature
  anywhere in this path; the only checks are a valid tar structure, an image magic byte, and the
  application image's own checksum/hash.
- Holding the DOWN button at power-on, before the install begins, aborts before anything is
  installed — it is checked exactly once, before installation starts, and does nothing once
  installation is underway. There is no in-flight abort.
- The firmware slot is committed before the Lua asset bundle is applied, which is why this
  project's own packaging (§6) always ships both members together.
- Once installed, the device falls back to the stock `factory` partition on its own only if the
  boot-selector partition is blank or invalid — not as a general safety net after a completed
  install. See §9 for what a real, on-device problem after that point actually requires.

Practical steps:

1. Format a **spare** microSD card FAT32/MBR (not exFAT — this firmware's FAT driver does not
   support it). Prefer a card ≤32 GB, which formats FAT32 natively.
2. Copy the archive built in §6 to `/Updates/BYOK.tar` on that card — it should be the only file in
   that folder — and verify its checksum before ejecting the card.
3. Power the device off, swap in the prepared card, and power it back on with a normal short press
   of the front power button. Do not hold any other button together with it.
4. The device installs the archive and reboots on its own; no further input is needed unless you
   are deliberately using the DOWN-at-power-on abort above.
5. Confirm success: the device should return to its normal UI, and a quick press of the front power
   button toggles a status line showing the running firmware version.

---

## 8. First boot

On the very first boot of this firmware on a given unit, expect, in order: the identity banner and
unresolved-hardware warnings on the console (§5 step 7 — read these once, since they name exactly
which hardware constants this build has not yet had confirmed on real silicon), the SD-updater
check (a no-op on this boot, since the archive that installed this build has already been
consumed and deleted by the updater itself), the display boot self-test sequence, and then the
resting screen. The device's host link only becomes reachable after the minimum-uptime gate in §3
elapses — a host-side tool that connects immediately on power-up should retry rather than assume
failure.

Because `CONFIG_BYOK_NVS_PERSIST_ARMED` ships on, any idle-screen selection, preset list, or
static-note text from a *previous* installation of this project's firmware on the same unit will
already be present — this project's own NVS namespace is independent of, and persists across,
reinstalling this firmware, and it never reads or writes any of the vendor's own settings sharing
the same physical NVS partition.

---

## 9. Recovery pointers

- **No on-device rollback exists on this bootloader.** A build that boots but then crashes or hangs
  will boot-loop indefinitely rather than reverting itself — see [bootloader-analysis.md](bootloader-analysis.md).
  There is no cost to this beyond the boot-loop itself: nothing about it damages the device.
- **The real recovery net is `esptool` over the ESP32-S3's own USB-Serial-JTAG interface**, which
  stays reachable for a fixed window on every boot regardless of what this firmware does (§3, §5
  step 3) — see [usb-and-boot-modes.md](usb-and-boot-modes.md) and [recovery.md](recovery.md) for the exact window and how to
  use it. This project's own hard rule ([SAFETY.md](../SAFETY.md)) still applies: any actual `esptool` invocation
  against a real device is a deliberate, physically-performed, one-at-a-time action, never an
  automated one.
- **Reverting to the vendor's own stock firmware** uses the same SD-updater path as §7, with a
  packaged archive built from the vendor's own release instead of this project's image — see
  [flash-layout-and-updater.md](flash-layout-and-updater.md) for the exact partition slots involved and
  [recovery.md](recovery.md) for the full, verified partition map this project's own `partitions.csv`
  reproduces.
- **Symptom-indexed troubleshooting** for specific failure modes encountered while developing this
  firmware — a blank panel, a shifted image, a display-init panic, and others — lives in
  [troubleshooting.md](troubleshooting.md).

---

See also: [architecture.md](architecture.md) (why the S3 alone is the right target), [protocol.md](protocol.md) (the
wire protocol this firmware's dispatcher implements), [display.md](display.md) (the panel driver in
detail), [pinout.md](pinout.md) (every GPIO this firmware touches), and [CHANGELOG.md](../CHANGELOG.md) (what changed
in each release, and each release's own build/test evidence).
