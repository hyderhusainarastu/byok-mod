# External references

Facts about the original device gathered from public sources — the
maker's own site and firmware API, press coverage, owner reviews, and
vendor datasheets — rather than from this project's own hardware
examination. Where a public source and this project's own findings cover
the same ground, prefer the hardware docs (`docs/hardware.md`,
`docs/display.md`, `docs/pinout.md`, and related pages); this document is
about what could be learned *without* opening the device.

## Evidence grading

Same convention used throughout this project:

- **CONFIRMED** — a maker statement, an official regulatory filing, or a
  fact independently corroborated by multiple sources.
- **STRONGLY INDICATED** — multiple independent sources agree, but it
  isn't an official first-party statement.
- **POSSIBLE** — a single, unofficial source.
- **UNKNOWN** — not found anywhere checked.

## 1. The device and its maker

The device is made and sold by an independent hardware maker (referred to
here simply as "the maker"), crowdfunded and subsequently sold directly.
A contract electronics/design partner is credited with the PCB design and
names the ESP32-S3 as its main microcontroller (CONFIRMED, design
partner's own case-study page).

Product facts, maker-stated (CONFIRMED unless noted): 164 × 79 × 15 mm
enclosure, monochrome FSTN LCD described as fast-refresh with minimal lag,
a microSD card slot, USB-C, Wi-Fi for cloud sync, roughly 20 hours of
battery life, magnetic mount compatible with standard phone/tablet
accessories, 12 keyboard layouts and 4 menu languages. Bluetooth support
is confirmed only by secondary sources (press coverage, the design
partner's page), not the maker's own product page.

Kickstarter campaign figures (funding total, backer count, exact
campaign dates) could not be independently verified from any source
reachable during this research and are **UNVERIFIED** — treat any such
figure found elsewhere with caution.

## 2. Firmware update procedure, as documented by the maker

Two methods, both maker-documented:

1. **Wi-Fi OTA**, from the device's own menu ("Check for Update"); the
   maker explicitly does not push update notifications, so checking is a
   manual, user-initiated action.
2. **Manual install via SD card**, for offline use: download the firmware
   file from the maker's site, place it in an `Updates` folder on the SD
   card (the file **must** be named exactly `BYOK.tar` and be the only
   file in that folder), then power the device on — the install begins
   automatically. The maker states firmware downgrades are untested and
   discouraged.

The status bar shows two version numbers in an `x.x.x/x.x.x` format; the
maker never states which number belongs to which of the device's two
microcontrollers, though the firmware API's own manifest field names
(`firmware_version`, `pico_version`) make the mapping STRONGLY INDICATED.

**The maker's backend.** The site's firmware distribution runs through a
small REST API of its own, one sentence's worth of detail: it serves a
current-version manifest, a version history, and both current and
historical firmware downloads by id, which made it possible to archive
every historical release directly rather than relying on whatever single
build happened to be on hand.

`BYOK.tar` (the single archive both update paths deliver) contains three
members: an ESP32-S3 application image, a second application image for
the device's other microcontroller (identified as an ESP32-PICO-class
part), and a tarball of Lua source files implementing the on-device UI —
confirmed by inspecting a real downloaded archive, not by documentation.
That the application layer is largely interpreted Lua loaded from
storage, rather than compiled entirely into the firmware image, was one
of the most consequential facts this research turned up: it means much of
the UI and application logic can be studied, and potentially modified, by
reading a text file rather than reverse-engineering a binary.

## 3. Hardware facts from external sources

### Display

| Fact | Grade |
|---|---|
| Monochrome FSTN LCD, adjustable warm backlight, fast refresh with minimal lag | CONFIRMED (maker) |
| Not e-paper — a fast-refresh LCD | CONFIRMED (maker + press) |
| Backlight: 5 levels including off | CONFIRMED (maker manual) |
| Shows roughly 2–6 lines of text depending on selected font size | POSSIBLE (single press source) |
| Character grid roughly 26×5 (large font) to 48×11 (small font) | POSSIBLE (single owner report) |
| Pixel resolution | **UNKNOWN** — no public source publishes it (this project's own hardware examination establishes it instead; see `docs/display.md`) |
| Display controller / LCD part number | **UNKNOWN** from public sources |

### Battery and power

| Fact | Grade |
|---|---|
| ~20 hours typical battery life; ~5 hours at maximum backlight | STRONGLY INDICATED (maker + press, consistent) |
| Charges via USB-C | CONFIRMED (maker) |
| A stated ~4700 mAh capacity and wireless charging | POSSIBLE, and contradicted by the maker's own page and by this project's own board observations — treat as aspirational marketing copy from a design partner's case study, not a device fact |
| Battery capacity in mAh, maker-stated | **UNKNOWN** |

### Chips

| Fact | Grade |
|---|---|
| ESP32-S3 main microcontroller, on a custom PCB | CONFIRMED (design partner statement) |
| A second, ESP32-PICO-class microcontroller exists and runs its own application | CONFIRMED (the maker's own firmware archive ships a distinct application image for it, and the API exposes a distinct version field) — no public source names its role, and every press article mentions only "ESP32-S3" |
| The PICO-class part has no native USB peripheral | CONFIRMED by absence in its datasheet's full peripheral list |

### Ports, storage, physical

| Fact | Grade |
|---|---|
| USB-C for charging and data; microSD slot | CONFIRMED (maker) |
| Disk Mode exposes the **SD card's** contents (not internal storage) as a USB mass-storage volume | CONFIRMED (maker manual) |
| Review units shipped with a small (~128 MB), FAT16-formatted microSD card | POSSIBLE (single owner report) |
| No real-time clock; files timestamp to a fixed default date; no NTP over Wi-Fi | POSSIBLE (single detailed owner report) |

### Regulatory

No FCC filing exists for the device itself, confirmed negative by direct
search. The Espressif radio modules it almost certainly uses (identified
by their own separate FCC grants, matched by manufacturer and product
code rather than by an exact part-number string appearing on either
listing) do have their own filings, but their schematic/block-diagram
exhibits are metadata-only entries with no retrievable documents. In
short: there is no FCC teardown of this device to lean on. Every
board-level fact in this project's own documentation came from direct
examination.

## 4. Keyboard input and USB-C behavior

- Both wired (USB) and wireless (Bluetooth/BLE) keyboards are supported;
  the single USB-C port automatically detects whether it's connected to a
  host or a peripheral. Whether host and peripheral roles can be active
  at once is **UNKNOWN**.
- A wired keyboard needs a **data-capable** USB-C cable — a charge-only
  cable produces no input (POSSIBLE, one detailed owner report, but
  consistent with how USB-C charge-only cables are wired generally).
- Wi-Fi is used only on demand (OTA checks and cloud sync), not as a
  background service.

## 5. Prior art

No public reverse-engineering of this device was found prior to this
project: no firmware/RE repository, no teardown, no custom firmware.
Search results consistently returned unrelated projects that use the same
short name to mean something else entirely (an unrelated abbreviation
common in software tooling) — a genuine trap worth knowing about if you
go looking yourself. A handful of independent open-source "writer deck"
projects exist and were reviewed for context; none are related hardware
or software.

The most substantive prior-art find was owner-authored review content
covering the SD-card disk mode, the data-cable requirement, OTA behavior
over a mobile hotspot, and the lack of a real-time clock — useful
corroboration for several of the STRONGLY INDICATED/POSSIBLE facts above.

## 6. Vendor reference material used for the firmware work

Espressif's own published documentation was the primary technical
reference for everything involving the ESP32-S3, independent of anything
specific to this device:

- **esptool basic commands** — the read-only/informational commands
  (`read-mac`, `flash-id`, `read-flash`, `image-info`) versus the
  destructive ones (`write-flash`, `erase-flash`, both refused by
  default when Secure Boot or Flash Encryption is detected).
- **espefuse** — every `burn-*`/`*-protect-efuse` command is a one-time,
  irreversible write; eFuse burning is never performed by this project.
- **Boot mode selection** — the ROM serial bootloader's GPIO0/GPIO46
  entry conditions, a hardware-level mode entirely separate from the
  device's own application-level Disk Mode.
- **USB-OTG console guide** — the chip's two independent USB blocks, the
  build-time console option that determines whether a device enumerates
  as charge-only or as a CDC serial device, and reference D+/D-/GND/+5V
  wiring.
- **TinyUSB MSC example** and **USB Host HID example** — the reference
  implementations Disk Mode and wired-keyboard support are presumed to be
  built on, though nothing external confirms that this device's firmware
  uses these exact drivers.
- **ESP32-PICO-MINI-02 datasheet** — confirms the second microcontroller's
  identity, its lack of USB, and that its UART0 is used for firmware
  download and log output — the basis for treating that UART as a
  candidate observation point for how the two chips communicate.

## 7. Sources

| Source | Type | Used for |
|---|---|---|
| The maker's own product and support pages | maker | Product specs, firmware update procedure, Disk Mode, buttons |
| The maker's firmware distribution API | maker | Version history, firmware downloads by id, manifest |
| The design partner's case-study page | design partner | ESP32-S3 statement, USB role auto-detection; unreliable on battery capacity/water resistance/wireless charging claims |
| Multiple technology press articles | press | Chip/memory figures, dimensions, battery life range, founder/company background |
| Independent owner reviews and blog posts | owner reviews | Disk mode details, cable requirements, OTA-over-hotspot, character grid, no-RTC observation |
| A public forum discussion thread | forum | Consumer reaction and a display-latency critique; no technical/reverse-engineering content |
| FCC's public database | regulatory | Confirmed no filing exists for the device itself; located the likely radio-module grants by manufacturer/product code |
| Espressif's public documentation and datasheets | vendor docs | Everything in §6 above |
| GitHub code search | negative result | No public firmware or reverse-engineering repository found for this device |

### Could not be retrieved at the time

The main crowdfunding campaign page and its sub-pages, one community
discussion platform, and one video-sharing platform's page content could not
be retrieved at the time this research was done — each returned an access
check instead of the page. No fact in this document is sourced to any of them; anything
that would have required them is marked UNVERIFIED or UNKNOWN above
instead of guessed at.
