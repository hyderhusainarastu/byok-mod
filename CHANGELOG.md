# Changelog

All notable changes to the byok-mod ESP32-S3 firmware (`byok_mod_s3`), by
version. `HELLO_ACK.fw_patch` (returned by the `INFO`/`HELLO` protocol
message) matches each release's patch number. Every sha256 below is of
**our own** `byok_mod_s3.bin` application image — never of the vendor's
`assets.tar`, which is repackaged unchanged alongside it, or of the
combined `BYOK.tar`, which also bundles that vendor member; see
[LEGAL.md](LEGAL.md) §3.

**About those hashes — read this before trying to reproduce one.** Each
sha256 identifies the exact binary that was packaged and installed on this
project's own device for that release. **It is not reproducible from this
source tree, and it is not meant to be.** `firmware/s3/sdkconfig.defaults`
does not set `CONFIG_APP_REPRODUCIBLE_BUILD`, so ESP-IDF embeds a build
timestamp (and build-machine paths) in every image it produces: two clean
builds of identical sources, minutes apart, differ. What a reader's own
build *does* reproduce is the **image size** listed alongside each hash —
that is the figure to check a build against. The hashes are here for a
different purpose: to pin down which byte-exact artifact a given release
note, photograph, or serial log refers to, and to let anyone holding one of
these binaries confirm which release it is. If you want a hash you can
reproduce yourself, add `CONFIG_APP_REPRODUCIBLE_BUILD=y` to your own
`sdkconfig.defaults` and hash your own output — that is a different binary
from the ones listed here.

Build/test detail beyond what's summarized here lives in
[`docs/firmware.md`](docs/firmware.md) and [`docs/testing.md`](docs/testing.md).

## 0.1.15 — 2026-09-04

**Fix:** preset-menu UP/DOWN direction. `byok_menu_move()` was adding its
`+1`/`-1` input directly to the highlight index, which was inverted
relative to the menu's own top-to-bottom row order; it now subtracts the
delta instead. No GPIO or button-wiring change — the GPIO map was already
confirmed correct; the bug was purely in the highlight/row reconciliation.

- Image: 470,752 B (85% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `e0c62490d3c8eb7dfca78f331ca995243dd7a3365b30bf9cbac2b0c070653199`

## 0.1.14 — 2026-09-03

**Added:**
- **Preset menu** — up to 8 named presets (`SET_PRESETS`, ≤20 bytes each),
  persisted in NVS when persistence is armed, shown at boot and via a
  ≥1 s EXECUTE hold or `SET_MODE 7`; UP/DOWN move the highlight, EXECUTE
  confirms and fires `EVT_PRESET_CHANGED`.
- **Host `EVT_BUTTON`** — UP/DOWN/BRIGHTNESS/short-EXECUTE now emit a
  button event to the host whenever the device is host-active and the
  menu is closed.
- **Writing stats** — a read-only background scan of `/SDCARD/Projects`
  for `*.txt` files, answered via new `GET_DOCSTATS`/`DOCSTATS`.
- **Display bulk-write runtime toggle** — the I²C bulk-vs-per-byte write
  path is now switched at runtime (`DISPLAY_CFG`) instead of at build
  time; the shipped default stays per-byte.
- NVS persistence build-armed (`CONFIG_BYOK_NVS_PERSIST_ARMED=y`) for
  presets and the selected preset index.

Protocol gains a v1.2 row in [`docs/protocol.md`](docs/protocol.md) §13 —
additive only, no version-byte bump.

- Image: 470,752 B (85% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `6bdde1e8df8b59c64680a47545261586c65fa106837da39ea35de3a4da543729`

**Known issue (fixed in 0.1.15):** preset-menu UP and DOWN moved the
highlight in the opposite direction from the button pressed.

## 0.1.13 — 2026-09-03

**Added:**
- **STATIC NOTE idle screen** — an in-RAM, word-wrapped note (≤200 bytes,
  set via new `SET_NOTE`) rendered against this firmware's own real
  30-column × 10-row text geometry.
- **Idle-mode and backlight cycling** — the four "back" buttons, polled
  only while no host is connected, cycle CLOCK → STATIC NOTE → BLANK and
  step the backlight through the vendor's own five stock levels
  (0/4/20/50/100%).
- NVS persistence (namespace `byokmod`), gated off by default pending a
  tested NVS-partition recovery path (see SAFETY.md §1).

**Fixed:** an NVS note read-back was one byte short of the buffer
`nvs_get_str()` needs for a 200-byte note at exactly its maximum length
(reload failure only, no corruption; only reachable once persistence is
armed, which it isn't by default).

- Image: 446,000 B (86% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `94564c0c744f30df4395f6797555051f86c8b482792147402cbd4f13a6aa2cac`

## 0.1.12 — 2026-09-03

**Added:**
- **Device-local CLOCK mode** — a PCF8563 RTC driver (I²C, address
  `0x51`) plus a rendered `HH:MM` / date / battery clock screen, entered
  automatically after a configurable idle timeout (default 30 s) and
  exited on the next host frame.
- **`SET_TIME`** (device-side only this release) to set the RTC from the
  host.
- **USB-power-refusal screen restore** — the framebuffer is now saved
  before the "ON USB POWER / CAN'T POWER OFF" message is drawn and
  restored afterward, instead of permanently replacing whatever was on
  the glass.

- Image: 442,272 B (86% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `0c51f0fc88ace26b118480043fc6503e3ae23e8f3b63b77aaf6e3b60930d9b79`

## 0.1.11 — 2026-09-03

**Added:** real battery voltage and charger-status readout — a 200-sample
trimmed-mean ADC read (S3's curve-fitting calibration scheme), a 5-entry
ring filter plus an EMA smoothing pass, and a 21-entry mV→percent table,
all values taken from the vendor's own disassembled constants. `STATUS`'s
`battery_mv`/`battery_pct`/`charging` fields now report the real reading
instead of a placeholder. `GET_BATTERY` (current draw) stays unsupported —
there is no current sensor in this design.

- Image: 436,096 B (86% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `be06ff5d6217c74a92a2b9ea4ac49e285fabb8d276949a6ba93b183034bddfb4`

## 0.1.10 — 2026-09-03

**Fixed:** a WAKE-hold shutdown crash — a scheduler-suspended, nonzero-
timeout I²C call inside the old suspend-all shutdown path could assert in
FreeRTOS, panic, and reboot instead of powering off. The display driver
now has its own internal mutex instead of relying on the caller to
suspend the whole scheduler.

**Added (stock-parity):** the on-glass "ON USB POWER / CAN'T POWER OFF"
refusal screen when a WAKE-hold is attempted while USB power is present,
a 500 ms backlight fade-out before power-off, and a 100 ms settle delay
before the final GPIO42 power-latch drive — all reproducing the vendor's
own pre-cutoff sequence.

- Image: 421,792 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `376b0cb7a6acdae8683d3bc55612e5a1a1aa179edfaf31fc4ba94d05ce124e34`

## 0.1.9 — 2026-09-03

**Fixed:** the 15-pixel-row display offset found on 0.1.8's first on-device
render. Root cause: the UC1611 "Set Scroll Line" register holds 15 at
power-on and neither firmware ever programmed it. Fix forces it to 0 at
bring-up, immediately after the vendor's own unchanged 21-entry
initialization table — strictly additive, revertible by one Kconfig
option.

- Image: 420,976 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `de5306fc614f05f87b65cd19427c901f481c8f1dc87837d9b3d62f846aa118a3`

## 0.1.8 — 2026-09-03

**First on-device render.** The panel lit up for the first time: `240X80
1BPP`, firmware version, reset reason, and a "waiting for host" line all
legible, with one defect (the 15-row offset fixed in 0.1.9).

**Investigated and refuted:** a hypothesis that the panel sat behind a
pair of I²C GPIO-expander latches. Full disassembly of the vendor's
`sim_send()`/`sim_reset()` showed each is a single, direct
`i2c_master_transmit()` — no strobe, no second transaction, no expander.
This project's transport was already byte-for-byte identical to the
vendor's; no transport code changed this release.

**Added:** a missing display-enable step (`CMD 0xC9` / `DATA 0xAD`), sent
once after the first frame reaches display RAM, matching a step the
vendor's own driver sends that this firmware had omitted through 0.1.7.

- Image: 420,976 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `2918ea08f8a32e904fef2cc08ca8c4fdbc491034d3f90504f94da71542225fb7`

## 0.1.7 — 2026-09-03

**Investigated and refuted:** a hypothesis that the I²C command/data
address polarity (`0x38`/`0x39`) was inverted. Re-derivation against the
UC1611 datasheet confirmed the existing mapping is correct — 16 of 19
init-table opcodes match the datasheet's command table exactly, including
the command/data bit.

**Added:** an on-device A/B/C polarity experiment (current mapping vs.
swapped vs. confirmed-correct) as the boot self-test, for direct
comparison on real hardware, plus a UC1611 STATUS READ probe on both bus
addresses.

- Image: 420,880 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `ccf0610f9c819b431f8a90d2f7e3af2e351c78f053b63d14a35fc80c29c023c0`

## 0.1.6 — 2026-09-03

**Added:**
- **Stock GPIO parity** — GPIOs 35, 39, and 47 are now driven LOW at boot,
  matching the vendor's own first GPIO action (previously only GPIO 42
  was reproduced; 35 and 47 are since understood to be part of an
  inter-MCU handshake, leaving 39 as the leading "panel enable" candidate).
- **Panel ACK probe** — a diagnostic-only I²C probe with ACK checking
  enabled (the normal display transactions run with ACK checking off, for
  vendor parity, so they can't tell presence from absence on their own).

- Image: 420,432 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `e4088083c13720eb16f8f9e2f2fe4b1f611dbdf7c543e1c6ae9b2236ed801f67`

## 0.1.5 — 2026-09-03

**Fixed:** the default display write path switched from an untested bulk
I²C burst mode to the per-byte fallback, after a byte-for-byte,
timing-for-timing audit against the vendor's own disassembly found the
per-byte path already identical to stock. Along the way, fixed a latent
build bug the switch exposed (a Kconfig boolean referenced directly as a
C ternary, which only happened to compile while it defaulted to enabled).

- Image: 419,296 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `d107ef5d0a1b3424904805de8765d79f18bea894d62f28640818c3b15ef9a110`

## 0.1.4 — 2026-09-03

**Fixed (from live device observation):**
- Power-off no longer reboots the device — the shutdown latch now waits
  for WAKE to be released before driving the power-off GPIO, instead of
  reading the still-held button as a fresh power-on press.
- USB now re-enumerates after an unplug/replug on battery power — a new
  component polls the USB-role chip's status register and forces a
  disconnect/reconnect cycle on a state transition, since this port
  doesn't otherwise notice a VBUS cycle on a self-powered device.
- The resting boot screen now says what it's waiting for and stays up
  until a host actually draws something, rather than being clearable by a
  bare connection attempt.

- Image: 419,040 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `0e850d88f9ff3866194c610cec7b8874c35628949c346b11cf442d40601b3975`

## 0.1.3 — 2026-09-03

**Fixed:** a FreeRTOS stack overflow on the main task the instant an
SD-card update actually started installing — present, unfixed, in every
build through 0.1.2. The SD-updater's buffers moved off the task stack
and onto the heap, the updater now runs on its own dedicated task with a
sized stack, and the main task's own stack budget was raised as
defense-in-depth. Added a host-side static regression guard
(`tests/static/check_stack_budgets.py`) that fails against the pre-fix
tree and passes against this one.

- Image: 416,832 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `db9a65f988970d1221f98856376985cd074a5e527d053ff7c6ed3aee9ec2509c`

## 0.1.2 — 2026-09-03

**Fixed:**
- USB-Serial-JTAG recovery access is now restored before every software
  restart (an `esp_restart()` after TinyUSB has installed otherwise
  permanently strands the `esptool` recovery window, since the PHY mux
  that controls it lives in a domain that survives a CPU-level reset).
- The display now issues a genuine UC1611 System Reset at bring-up
  (behind a Kconfig option, default on) — the vendor's own reset
  preamble sends a byte to an address the datasheet says is decoded as a
  harmless RAM write, not a real reset.
- Reset reason is now logged and shown on the boot self-test screen.

- Image: 416,096 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `4e5215da4e0d71e2e701c1b14f8aac361b8855cb683a76c95a350875493ca9b9`

## 0.1.1 — 2026-09-03

**Fixed:**
- **Power-off handler added** — a 3+ second WAKE hold now shuts the
  device down (drives the power-latch GPIO high and halts), where 0.1.0
  had no power-off path at all on battery.
- **USB serial no longer leaks the device MAC** — the CDC serial number
  changed from 12 hex characters of the raw station MAC to a
  non-reversible hash-derived string.

- Image: 414,432 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `b6bd052589b85ce803a7a158e467a8a75b5c70f0baabf78f68649910b55a7ba4`

## 0.1.0 — 2026-09-03

First real install: our firmware, packaged as a two-member update archive
(our `BYOK.bin` plus the stock, unmodified `assets.tar`) and installed
through the vendor's own SD-card updater, booted successfully on real
hardware. Display self-test ran with zero I²C errors; CDC-ACM enumerated.
Two defects found on first boot (no power-off path, and the USB serial
number leaking the device's MAC) — both fixed in 0.1.1, above.

- Image: 413,504 B (87% of the 3 MiB OTA slot free)
- `byok_mod_s3.bin` sha256: `5037db2228fddd748b0554463ca7527ceaa34a6c002989af59128c96ebb7b15b`
