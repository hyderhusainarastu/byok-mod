# byok-mod

Reverse-engineering notes and replacement firmware for the **BYOK v2.1**
distraction-free writing device — plus a macOS host tool that draws on its
panel over USB, and a couple of optional sample projects built on top of
that link.

> **Unofficial.** This project is not made, endorsed, reviewed, or supported
> by the company that makes the BYOK, or by any of its partners. "BYOK" is
> used here only to identify the hardware this project targets (nominative
> use) — see [LEGAL.md](LEGAL.md) for the trademark position.
>
> **No vendor firmware here.** This repository contains no copy of the
> stock firmware, no stock assets, and no stock update archive. Everything
> you need to build and install our own firmware image is in this repo;
> the one input you must supply yourself is a stock `BYOK.tar` from your
> own device or the maker's own update channel, used only as a packaging
> ingredient (see "Installing a build" below).
>
> **Nothing here is unlocked, jailbroken, or DRM-stripped.** On the one unit
> this project examined, Secure Boot was not enabled and flash encryption
> was off — a fact we document, not a protection we defeated. See
> [LEGAL.md](LEGAL.md) §5 for exactly what that claim does and does not
> cover.
>
> **Use this at your own risk.** This modifies the application partition of
> a physical device you own. We describe a verified path back to the stock
> firmware and we used it ourselves, but we cannot promise your unit, your
> cable, or your SD card will behave identically. There is no warranty of
> any kind — see [LEGAL.md](LEGAL.md) §7.

## What the device is

The BYOK is a small, battery-powered, distraction-free writing device: a
physical keyboard, a monochrome panel, and stock firmware built for
composing and syncing plain text. Under the hood it is **two
microcontrollers on one board**:

| | ESP32-S3-WROOM-1 | ESP32-PICO-MINI-02 |
|---|---|---|
| Role in stock firmware | Display, USB, SD card, Wi-Fi, the SD-card firmware updater | Bluetooth keyboard link |
| Flash / PSRAM | 16 MB / 2 MB — **CONFIRMED** | 8 MB / 2 MB — POSSIBLE (never probed) |
| Native USB | Yes (Full-Speed OTG) | No |
| Bluetooth | BLE only | Classic + BLE |

Only the S3 has native USB; only the PICO has Bluetooth Classic. That one
asymmetry is why this project only ever touches the S3 — see "How the mod
works" below. Full findings, with an evidence label on every claim, are in
[`docs/hardware.md`](docs/hardware.md), [`docs/architecture.md`](docs/architecture.md),
and [`docs/pinout.md`](docs/pinout.md).

The panel is a **240×80, 1-bit-per-pixel monochrome FSTN LCD**, driven over
I²C by a UC1611-class controller at two device addresses (`0x38` command,
`0x39` data), with a PWM backlight and no reset line — bring-up is I²C-only.
Full derivation, including the disassembly that pinned these numbers down:
[`docs/display.md`](docs/display.md).

## What this project does

This project replaces the application on the ESP32-S3 with our own
ESP-IDF firmware, installed into a spare OTA slot so the stock firmware
stays intact in another slot and is never overwritten. Our firmware:

- speaks a small custom protocol ([`docs/protocol.md`](docs/protocol.md))
  over USB CDC-ACM to a macOS command-line tool (`byok`), which renders
  everything — text, images, a live status dashboard — and sends the
  device a packed framebuffer or a dirty-rectangle update;
- shows a **device-local clock** (driven by the device's own PCF8563 RTC)
  when no host is connected, and switches back the moment one attaches;
- leaves the vendor's own boot slot and the vendor's own SD-card update
  path completely alone, so re-flashing stock or reinstalling a stock
  update tar both still work exactly as they did before this project
  touched the device.

The PICO — and the Bluetooth keyboard it drives — is never written, never
erased, and never addressed by anything this project ships.

## How the mod works

Reaching this design took closing four questions first: which MCU owns
USB, which owns the display, what the display actually is, and how the two
MCUs talk to each other. All four turned out to point at the S3 alone —
see [`docs/architecture.md`](docs/architecture.md) for the full
five-option comparison and the evidence behind each answer. In short:

1. **Everything this project needs is already on the S3.** Display, native
   USB, Wi-Fi, the SD card, and the RTC are all confirmed S3-owned in the
   stock firmware.
2. **The PICO can't be backed up.** No download-mode route to it has ever
   been found, so nothing writes to it — a hardware safety rule, not just
   a design preference (see [SAFETY.md](SAFETY.md)).
3. **The S3 has no Secure Boot and no flash encryption enabled on the unit
   examined**, and its bootloader exposes three independent app slots
   (`factory`, `ota_0`, `ota_1`) plus an `otadata` selector — so an
   unsigned image built by us is accepted, and there is a slot to put it
   in without touching the vendor's own image. Full partition map and
   evidence: [`docs/flash-layout-and-updater.md`](docs/flash-layout-and-updater.md).
4. **Our firmware occupies one OTA slot; the vendor's own build stays put
   in another.** Getting our image there re-uses the vendor's *own*
   SD-card update mechanism — see `scripts/make-update-tar.sh` and
   [`docs/firmware.md`](docs/firmware.md) — rather than
   inventing a separate flashing path. `esptool` over USB-Serial-JTAG is
   used only for read-only backup and for the one sanctioned recovery
   write (see [`docs/recovery.md`](docs/recovery.md)).

The partition layout that makes this possible — three app slots plus the
`otadata` selector, all read directly off the device's own flash — is
diagrammed in [`docs/diagrams/flash-partition-map.md`](docs/diagrams/flash-partition-map.md).
More diagrams — whole-board block diagram, bring-up flow, USB PHY hand-over,
frame format, link sequence, GPIO map, panel refresh, power gestures — live
alongside it under [`docs/diagrams/`](docs/diagrams/), each linked from the doc
it illustrates.

## Repo map

```
byok-mod/
  README.md  LEGAL.md  SAFETY.md  CONTRIBUTING.md  CHANGELOG.md  LICENSE  LICENSE-DOCS
  firmware/
    common/          Shared headers (hw_config.h) and the protocol/tar code both sides use
    s3/              The ESP32-S3 application (ESP-IDF project) — this project's own firmware,
                      shipped with sdkconfig.defaults only; sdkconfig itself is generated
    pico/            Empty by design — nothing in this project ever writes to the PICO
  host/
    macos/           The `byok` CLI and dashboard renderer (Python), plus two optional
                      Swift helper packages used by the mirror sample project
    tools/           Small standalone host-side scripts (panel bring-up, test patterns)
  scripts/           Toolchain bootstrap, update packaging, capture, and recovery automation —
                      see each script's own header comment
  extras/doom/       Optional: chocolate-doom mirrored onto the panel (not part of the base build)
  captures/          Two curated device captures (a clean boot log and a USB enumeration trace)
                      cited by the docs
  tests/             Host unit tests, protocol/tar C tests, a static stack-budget guard, and a
                      manual on-device checklist
  docs/              Everything written up along the way — see the table below
    diagrams/        Mermaid sources and rendered SVGs, linked from the docs they illustrate
    img/             Dashboard preview renders
    sample-projects/ The two optional demos built on the host-tool pipeline
```

### Docs

| Doc | Covers |
|---|---|
| [architecture.md](docs/architecture.md) | System-level design and the option comparison that chose it |
| [hardware.md](docs/hardware.md) | Board identity, component inventory, connectors |
| [pinout.md](docs/pinout.md) | GPIO map, test pads, button wiring |
| [display.md](docs/display.md) | Panel electrical interface, controller, framebuffer |
| [usb-and-boot-modes.md](docs/usb-and-boot-modes.md) | Normal / Disk Mode / console mode, `esptool` over USB-Serial-JTAG |
| [flash-layout-and-updater.md](docs/flash-layout-and-updater.md) | Partition table, OTA slots, the SD-card updater, bootloader |
| [bootloader-analysis.md](docs/bootloader-analysis.md) | Disassembly of the second-stage bootloader |
| [recovery.md](docs/recovery.md) | Every verified way back to stock |
| [firmware.md](docs/firmware.md) | Our own firmware's design and install procedure |
| [protocol.md](docs/protocol.md) | The BYOK Link wire protocol (host ↔ our firmware) |
| [host-tools.md](docs/host-tools.md) | The `byok` CLI and dashboard renderer |
| [sample-projects/mirror-and-virtual-display.md](docs/sample-projects/mirror-and-virtual-display.md) | Mirroring a Mac window/display onto the panel |
| [sample-projects/doom.md](docs/sample-projects/doom.md) | Doom, mirrored onto the panel, as a demo of the same pipeline |
| [testing.md](docs/testing.md) | What's automated and what's a manual on-device checklist |
| [troubleshooting.md](docs/troubleshooting.md) | Symptom-indexed writeups of issues hit along the way |
| [design-rationale.md](docs/design-rationale.md) | Why each significant design choice was made, and what was rejected |
| [environment.md](docs/environment.md) | One verified-working build environment, version by version |
| [external-references.md](docs/external-references.md) | What we found out about the device from public sources |
| [diagrams/](docs/diagrams/) | Mermaid sources and SVGs for every diagram the docs embed |

## Quick start

All three pieces are independent — you don't need the firmware toolchain to
use the host tool against an already-flashed device, and you don't need the
Mac helpers to build firmware.

### Firmware (ESP32-S3, ESP-IDF)

```sh
scripts/setup-esp-idf.sh          # one-time: clones and bootstraps ESP-IDF under ~/esp/esp-idf
source scripts/idf-env.sh         # activates it for this shell

cd firmware/s3
idf.py set-target esp32s3
idf.py build                      # compiles firmware/s3/build/byok_mod_s3.bin — does NOT flash anything
```

`set-target` generates `firmware/s3/sdkconfig` from the tracked
`sdkconfig.defaults`; both it and `build/` are generated artefacts and are
git-ignored, so a fresh clone builds from defaults every time.

`idf.py flash`/`idf.py monitor` are printed as suggestions at the end of a
build; do not run them against the physical device casually — see
[SAFETY.md](SAFETY.md) and [`docs/firmware.md`](docs/firmware.md)
for the one supported install path, which goes through the vendor's own
SD-card updater, not a direct flash.

### Host tools (macOS)

```sh
cd host/macos
python3 -m venv .venv && source .venv/bin/activate
pip install --upgrade pip      # macOS's system Python ships a pip too old for
                               # an editable pyproject install
pip install -e '.[dev]'

byok info                          # firmware version, geometry, capability bitmap
byok status                        # battery mV/%, charging state, uptime
byok text "hello from mac"         # draw a line of text and refresh
byok image photo.jpg               # render an image (dithered) to fill the panel
byok dashboard --interval 1        # live status dashboard (clock, battery, host stats)
```

Full command reference and the dashboard's preset/layout system:
[`docs/host-tools.md`](docs/host-tools.md).

The host suite runs with no device attached:

```sh
BYOK_FORCE_MOCK=1 pytest tests/host tests/static
```

`BYOK_FORCE_MOCK=1` forces the mock transport, so nothing opens a serial port.
What is automated and what has to be done by hand on the device is set out in
[`docs/testing.md`](docs/testing.md).

### Sample projects

Two optional demos build on the same host-tool pipeline and are not part of
the base install:

- **Mac window/display mirror** — captures a window or display with
  ScreenCaptureKit, dithers it to 1-bit, and streams it to the panel.
  [`docs/sample-projects/mirror-and-virtual-display.md`](docs/sample-projects/mirror-and-virtual-display.md)
  covers both Swift helpers (`swift build -c release` in
  `host/macos/ScreenMirrorHelper/` and `host/macos/VirtualDisplayHelper/`).
- **Doom** — the same mirror pipeline pointed at a windowed
  `chocolate-doom`. `extras/doom/install.sh` then `extras/doom/byok-doom.sh`;
  see [`docs/sample-projects/doom.md`](docs/sample-projects/doom.md). The
  shareware WAD it fetches is not shipped in this repo — see
  [LEGAL.md](LEGAL.md) §6.

## Installing a build

1. Build the tar: `scripts/make-update-tar.sh --s3 <path to byok_mod_s3.bin> --no-pico --stock-from
   <path to your own vendor release archive> --out <dir>`. You provide the vendor release archive
   yourself (see LEGAL.md §3) — the script's `--help` covers the exact format it expects.
2. Format a **spare** SD card FAT32/MBR and copy only `BYOK.tar` into its
   `/Updates/` folder.
3. Verify the copy's sha256 against the packaging script's own
   `SHA256SUMS` before ejecting.
4. Power off, insert the card, power on, and watch the serial console —
   the stock updater installs the image into whichever OTA slot `otadata`
   is not currently pointing at, exactly as it would for a stock update.

Full step-by-step, pre-flight checklist, and expected timeline:
[`docs/firmware.md`](docs/firmware.md).

## Returning to stock

- Re-apply a stock update tar via the same SD-updater procedure above — it
  restores stock into the OTA slot `otadata` isn't currently pointing at.
- Or erase `otadata` directly with `scripts/s3-revert-to-factory.sh`
  (gated behind an explicit `--yes`) to fall through to the untouched
  `factory` slot — the original build, no tar needed.
- Full detail, including the one destructive step this repo will ever
  run against the device and everything checked before it runs:
  [`docs/recovery.md`](docs/recovery.md).

## Safety

This project modifies a physical device. Read [SAFETY.md](SAFETY.md)
before doing anything on real hardware — it covers the hard constraints
(never write to efuses, never touch the PICO, never overwrite anything
without a verified backup and a tested recovery path first), the evidence
standard every hardware claim in these docs is held to, and how physical,
one-at-a-time experiments are run.

## Licence

The code and documentation in this repository are dual-licensed:

- **Code** — [MIT](LICENSE).
- **Documentation** (everything under `docs/`, this README, and the other
  root-level `.md` files) — [CC BY 4.0](LICENSE-DOCS).

This licence covers this project's own original code and documentation. It
does not cover the vendor's firmware, which is not distributed here — see
[LEGAL.md](LEGAL.md).
