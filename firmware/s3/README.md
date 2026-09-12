# BYOK Mod — ESP32-S3 firmware

This is the replacement application for the ESP32-S3, built with ESP-IDF v5.5.3. It builds
cleanly from a fresh checkout and has been installed on real hardware via the stock SD-card
updater; see `CHANGELOG.md` for the version history and `docs/firmware.md` for the design,
packaging and install procedure. See `docs/protocol.md` for the wire protocol this firmware
implements and `firmware/common/hw_config.h` for the reverse-engineered hardware constants it's
built from.

## Build

```sh
source scripts/idf-env.sh          # or: source ~/esp/esp-idf/export.sh
cd firmware/s3
idf.py set-target esp32s3
idf.py build
```

The first `idf.py build` (or `idf.py reconfigure`) pulls `espressif/esp_tinyusb` (pinned to
`2.2.1` in `main/idf_component.yml`) into `managed_components/` via the ESP-IDF Component Manager —
this needs network access once; after that it's cached like any other dependency.

**`idf.py flash` (or anything that calls `esptool.py write_flash`) must NEVER be run against the
real device from this project.** This tree's `sdkconfig.defaults` sets `CONFIG_PARTITION_TABLE_CUSTOM`
so `partitions.csv` matches the device's own table, but *matching* is not *never touching*: the
project's hard rules (see the repo's `SAFETY.md`) forbid writing the bootloader, partition table,
`nvs`, `phy_init`, `factory`, `ota_0`, or `otadata` regions under any circumstances, and restrict
our own images to the `ota_1` slot or the stock SD-card updater path — never `esptool` directly.
`idf.py build` alone (no `flash` target) never touches the device and is always safe to run.

## How the image actually gets onto the device

Not via `esptool`. `docs/flash-layout-and-updater.md` and `docs/recovery.md` document the
stock boot-time updater: at boot, the vendor firmware looks for
`/SDCARD/Updates/BYOK.tar` (containing `BYOK.bin`, `BYOK-Pico.bin`, `assets.tar`) and installs it
via `esp_ota_begin`/`esp_ota_write`/`esp_ota_end`/`esp_ota_set_boot_partition` into the next OTA
slot, accepting any image with a valid magic byte and SHA-256 — no vendor signature check. That
same path is both the project's recovery mechanism and the intended install path for **our**
image: rename `build/byok_mod_s3.bin` to `BYOK.bin`, package it into a `BYOK.tar` alongside the
stock `BYOK-Pico.bin` and `assets.tar` (so the PICO side and Lua assets are left exactly as they
were), and drop that `BYOK.tar` onto the SD card's `Updates/` folder. `scripts/make-update-tar.sh`
does exactly that packaging, including the tar-format requirements the vendor's own extractor
enforces; you supply the stock archive it takes `BYOK-Pico.bin`/`assets.tar` from. Our firmware
lands wherever the stock updater's own OTA-slot selection puts it — the slot `otadata` is *not*
currently pointing at — rather than somewhere we choose; see `docs/flash-layout-and-updater.md`
for that logic in full.

## Two USB personalities — and why they conflict

The ESP32-S3 has two *separate* USB peripherals that both come out on the same physical D+/D− pads
(GPIO19/20, wired to the case's USB-C port): the built-in **USB-Serial-JTAG** controller (used for
the ROM bootloader, `esptool`, and — per `sdkconfig.defaults`' `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`
— the app's secondary log console, matching stock) and the **native USB-OTG** controller (which
TinyUSB drives, `components/byok_usb_cdc`). Only one can own the pads at a time.

Stock firmware's own boot log shows USB-Serial-JTAG visible for ~4 s at boot before the app's own
USB personality takes over (`hw_config.h` Sec.7, `BYOK_BOOTMODE_*`) — this firmware does the same
thing implicitly: `ESP_LOG*` lines are visible over USB-Serial-JTAG (and over UART0, the primary
console) from power-on until `byok_usb_cdc_init()` runs in `app_main()`, at which point TinyUSB
claims the OTG PHY and the USB-Serial-JTAG console link, if a host had one open, will stop
responding. After that point, `ESP_LOG*` output is only reliably visible over UART0 — which this
board does not obviously expose outside the case (not confirmed either way; out of scope for this
firmware). Anyone debugging boot problems after that handover needs a UART0 connection, not a
`/dev/cu.usbmodem*` log tail.

## USB PID choice

VID stays Espressif's own, `0x303A` — appropriate for hobbyist/development use of their silicon
(their own Kconfig help text calls this "helpful at product develop stage"), not a claim to be an
Espressif product.

PID **`0x83B8`**, set via `CONFIG_TINYUSB_DESC_CUSTOM_PID` in `sdkconfig.defaults`. This must not
collide with `0x1001` (the S3's own USB-Serial-JTAG interface — a different peripheral, always
enumerable, see above) or `0x4002` (the **stock firmware's own** Disk-Mode PID — confirmed in
`docs/usb-and-boot-modes.md` from a live capture: `idProduct = 16386 = 0x4002`, itself just TinyUSB's
generic example default, not vendor-unique). Both of those values are *default example PIDs*
baked into Espressif's own SDK examples (from `espressif/esp-usb`'s `usb_descriptors.c`:
`USB_TUSB_PID = 0x4000 | (class bitmap)`), not part of Espressif's actual PID *registry* —
confirmed by fetching `github.com/espressif/usb-pids` directly on 2026-09-03: its
`allocated-pids-espressif-devboards.txt` covers a completely different range (`0x7000`+, UF2/
CircuitPython bootloaders for Espressif's own devkits), and `0x1001`/`0x4002` appear in neither of
that repo's two registry files at all — they're SDK example defaults, not registrations.

`0x83B8` is the **next sequential value** after that repo's `allocated-pids.txt` (the real
customer-PID registry, `0x8000`–`0x83B7` as of the same date) — i.e. it follows that registry's own
stated process ("take the next available one and add it at the bottom of the list") without
actually having gone through it: **nobody has opened a pull request against `espressif/usb-pids`
to register this value.** It is a good-faith, collision-avoiding choice for this project's own
development and testing, not a real registration — if this device is ever meant to be distributed
beyond this project, that PR is the actual next step, and by then some other project may already
have claimed `0x83B8` in the meantime (re-check the registry before relying on this value long-term).

## Directory layout

```
firmware/s3/
  CMakeLists.txt          top-level project file; EXTRA_COMPONENT_DIRS -> firmware/common
  sdkconfig.defaults
  partitions.csv           reproduces the device's VERIFIED partition table; never flashed
  main/
    app_main.c              boot sequence + BYOK Link dispatcher (see its own header comment)
    Kconfig.projbuild        CONFIG_BYOK_ALLOW_OTADATA_WRITE
    idf_component.yml        espressif/esp_tinyusb dependency
  components/
    byok_battery/             ADC divider read + charge-state reporting
    byok_clock/               device-local clock face rendered from the PCF8563 RTC
    byok_display/             UC1611-class panel driver + LEDC backlight + 8x8 font
    byok_docstats/            read-only manuscript statistics reported back to the host
    byok_hw_shim/             resolves hw_config.h Sec.13 build-blockers (see its own header)
    byok_idle/                back-button poll task; cycles CLOCK -> NOTE -> BLANK with no host
    byok_menu/                device-driven preset menu overlay
    byok_modes/               application-mode state machine (see its own header)
    byok_note/                word-wrapped stored-note rendering
    byok_nvs/                 our own NVS namespace; never touches the vendor's
    byok_power/               power/shutdown gestures and their guards
    byok_rtc/                 PCF8563 driver (read, and SET_TIME writes)
    byok_sd_updater/          our own re-implementation of the stock SD update path
    byok_sysbus/              shared I2C bus ownership
    byok_usb_cdc/             TinyUSB CDC-ACM transport
firmware/common/
  hw_config.h                 the reverse-engineered hardware constants both sides build from
  byok_proto/                 BYOK Link frame encoder/decoder (own component, own CMakeLists.txt)
  byok_tar/                   tar reader used by the SD update path
```

## Guesses and things to verify before this matters on real hardware

Numbered so they're easy to reference. None of these block a *build*; several matter a great deal
before this is ever pointed at the physical device.

1. **Flash mode QIO vs the RE-confirmed DIO.** `hw_config.h` Sec.0 records the vendor's own image
   as `[CONFIRMED]` DIO/80 MHz (from `esptool image_info`). This firmware's `sdkconfig.defaults`
   sets QIO. See the `NOTE` in that file for why this is very likely inert (the bootloader
   configures the flash bus once, from its own header, and we never touch the bootloader) — but it
   is a real disagreement with the evidence, left visible rather than silently reconciled.
2. **PSRAM speed (40 MHz).** Not in `hw_config.h` at all (RE pass never covered it) — a conservative
   default for Quad PSRAM, not evidence.
3. **`byok_hw_shim`'s remaining unresolved unknowns** (`hw_config.h` Sec.13): LCD framebuffer bit
   order, GPIO39/48 function, and three LCD init opcodes' meaning. Every one is a Kconfig-visible
   placeholder with a loud boot-time `ESP_LOGW` — see `components/byok_hw_shim/include/
   byok_hw_shim.h` and its `Kconfig`. The framebuffer bit order in particular is a "best guess,
   unverified on real pixels" per `hw_config.h`'s own words — if the first real image on the panel
   looks vertically mirrored within 8-row bands, flip `CONFIG_BYOK_HW_LCD_FB_BIT_ORDER_LSB_TOP`.
   (0.1.11: the battery ADC divider ratio and charger STAT pin polarity, formerly two more items on
   this list, are now CONFIRMED from the stock image's own battery code — see `docs/hardware.md` —
   and baked directly into `hw_config.h` Sec.9 as real constants, not a Kconfig guess; see
   `components/byok_battery`.)
4. **USB PID `0x83B8` is self-assigned, not registered** — see "USB PID choice" above.
5. **The boot/DRAW_TEXT font (`components/byok_display/include/byok_font8x8.h`) is original and
   minimal** — hand-authored for this firmware, not reverse-engineered from the vendor's `gclcd1611`
   font system and not copied from any third-party font table. It covers only space, `A`-`Z`,
   `0`-`9`, and a handful of punctuation (47 glyphs); anything else (including all lowercase)
   renders as a filled fallback box, per `docs/protocol.md` Sec.6.2's own escape hatch for
   unmappable glyphs.
6. **`DRAW_TEXT` treats its payload as raw ASCII bytes, not decoded UTF-8.** A multi-byte UTF-8
   sequence draws one fallback box per raw byte instead of one box per codepoint. Fine for ASCII
   text (which is all the font covers anyway); a real UTF-8 decoder is future work.
7. **RLE (`FLAGS.RLE`) is not implemented.** Any frame with that bit set gets
   `NACK/E_UNSUPPORTED` — spec-legal (capability bit 2 is correctly left unset in `HELLO_ACK`/`INFO`).
8. **The v1 "partial frame" `FRAME_BEGIN` path is not implemented** (`flags` bit0) —
   `docs/protocol.md` Sec.6.3 calls this path "ugly" in its own text and earmarks a cleaner
   replacement for v2; this firmware NACKs it rather than implementing the awkward v1 form.
9. **`FRAME_END`'s three non-zero `refresh` values (partial/full/device-chooses) all do a full
   refresh.** No dirty-sub-rectangle tracking exists across a `FRAME_*` transaction yet — correct
   but not optimally fast.
10. **Duplicate-SEQ replay (`docs/protocol.md` Sec.7.5) resends a bare `ACK`, not the original
    typed reply.** There's no last-reply cache in this firmware, so a retransmitted `HELLO` or
    `GET_STATUS` (say, because the host's own ACK timed out) gets a plain ACK back on the second
    delivery rather than a second `HELLO_ACK`/`STATUS`. A host that always waits for the *specific*
    reply type rather than treating ACK as sufficient will need this fixed before it's reliable.
11. **Outbound events shipped in 0.1.14** — `EVT_BUTTON` is emitted for short and long presses
    (`app_main.c`'s `send_event()`), and `EVT_STATUS` carries unsolicited status changes. `EVT_LOG`
    is defined in `byok_proto.h` but is still never sent.
12. **Two "mode" concepts coexist** — `main/app_main.c`'s `s_link_mode` (protocol `SET_MODE`'s
    `IDLE`/`HOST`/`MIRROR`/`SLEEP`, still purely bookkeeping — `SLEEP` does not turn off the panel)
    and `components/byok_modes` (`byok_app_mode_t`'s `DASHBOARD_USB`/`CLOCK`/`STATIC_NOTE`/
    `ORIGINAL`) — but as of 0.1.12 `SET_MODE` actually drives the second one too: `IDLE` switches
    the device into `CLOCK` (a real, rendering mode now — `components/byok_clock`), `HOST`/`MIRROR`
    switch back to `DASHBOARD_USB`. `STATIC_NOTE` remains a skeleton with no rendering code
    anywhere in this tree. See `byok_modes.h`'s own header comment for the full correspondence.
13. **`GET_BATTERY` always `NACK`s `E_UNSUPPORTED`, by design** — not because the reading is
    unreliable any more (0.1.11: `components/byok_battery` gives `GET_STATUS`/`STATUS` real
    `battery_mv`/`battery_pct`/`charging`, and `HELLO_ACK`/`INFO` caps bit6 is set to say so), but
    because the *dedicated* `GET_BATTERY` command's one extra field over `STATUS` (`current_ma`)
    has no sensor behind it in this design, vendor or ours — see `main/app_main.c`'s own comment at
    the `BYOK_TYPE_GET_BATTERY` case.
14. **CPU frequency is IDF's own default**, not pinned to the ~160 MHz `hw_config.h` records for
    stock — out of scope for this project's `sdkconfig.defaults`; revisit if matching stock's
    exact power/clock behaviour turns out to matter.
15. **NVS persistence is behind a build-time gate.** `docs/protocol.md` Sec.10 reserves our own
    NVS namespace for our settings (mode, backlight, contrast) and is explicit that the vendor's
    own namespaces are read-never/write-never. `components/byok_nvs` implements it, but it only
    writes when `CONFIG_BYOK_NVS_PERSIST_ARMED` is set at build time; with the gate off,
    `SET_MODE`'s persist-across-reboot flag bit is parsed and deliberately ignored.
16. **`SET_TIME` (`0x27`, v1.1) is implemented on both sides.** `components/byok_rtc` writes the
    PCF8563 and the host sends it as `byok settime` (`host/macos/byok/cli.py`). The device still
    keeps correct time on its own between hosts, so this is a convenience, not a dependency.

## Never do this against the real device

- `idf.py flash`, `idf.py monitor` (if it auto-resets), or any bare `esptool.py write_flash` /
  `erase_flash` / `erase_region` call against `/dev/cu.usbmodem*` or `/dev/cu.usbserial*` for this
  board.
- Enabling `CONFIG_BYOK_ALLOW_OTADATA_WRITE` as a build-time convenience rather than a deliberate,
  verified-recovery-path-exists decision (see the Kconfig help text).
