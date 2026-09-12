# Testing

This document is the test reference: acceptance criteria, test procedures, and what has actually
been verified on the physical device versus what is still specification-only. It merges the
project's test plan with its running verification record.

**Test framework:** manual owner-performed hardware tests for anything that touches the physical
device, plus automated pytest for host code and firmware-side unit tests that don't need hardware.
Nothing in this project's own test suite has ever opened the real serial port outside a deliberate,
one-at-a-time owner-performed step — see host-tools.md §2a for the `BYOK_FORCE_MOCK` guard that
enforces this in every other shell.

---

## 1. Test categories

1. **Firmware Protocol (FP)** — packet parser, error handling, state machine
2. **Firmware Hardware (FH)** — display driver, USB CDC, button/ADC integration
3. **Host (HT)** — discovery, encoding, rendering, transport
4. **Hardware Integration (HI)** — device bring-up, USB enumeration, power state
5. **Display Rendering (DR)** — visual patterns, refresh behavior, performance
6. **Recovery & Safety (RS)** — rollback, boot-original path, escape hatch

FP and HT are automated pytest, runnable with no device attached (protocol.md §12's test vectors
are exactly what FP-001 through FP-010 exercise). FH, HI, DR and RS are manual, owner-performed,
one device-state-changing step at a time, per this project's hardware safety rules (SAFETY.md).

## 2. Firmware Protocol tests (FP) — automated, no device needed

| ID | Requirement | Expected result |
|---|---|---|
| FP-001 | Valid `HELLO` frame decodes and gets `HELLO_ACK` | CRC matches, SEQ echoed correctly (protocol.md §12 vector #2) |
| FP-002 | Bad CRC is rejected | `NACK/E_BAD_CRC`, original SEQ echoed best-effort |
| FP-003 | Oversized payload (`LEN` > 4096) is rejected | `NACK/E_BAD_LENGTH`, no attempt to read past the limit |
| FP-004 | Firmware resyncs after a mid-frame stall/disconnect | Times out the incomplete frame (protocol.md §7.6's 200 ms/3000 ms rules), resync succeeds, the next valid frame is processed |
| FP-005 | A `SEQ` gap is detected and reported, then processing continues | `NACK/E_SEQ_GAP` with the correct `detail`, followed by the real reply to the frame that triggered it (protocol.md §7.5) |
| FP-006 | A duplicate `SEQ` on an idempotent command is dropped, not re-executed | The cached reply is re-sent; the command's side effect does not happen twice |
| FP-007 | An open `FRAME_*` transaction times out after 3 s with no data | `EVT_LOG` warning, transaction closes, a late `FRAME_END` gets `NACK/E_STATE` |
| FP-008 | `FRAME_END` with a mismatched CRC is rejected | `NACK/E_FRAME_CRC`, panel keeps its previous content |
| FP-009 | An incomplete frame is reported and discarded | `NACK/E_INCOMPLETE` with `detail` ≥ 1 |
| FP-010 | `CLEAR` mutates the back buffer only; `PARTIAL_REFRESH` pushes it | Panel unchanged after `CLEAR` alone, changed only once a refresh command follows (protocol.md §7.1) |

## 3. Firmware Hardware tests (FH) — manual, device required

| ID | Requirement | Expected result |
|---|---|---|
| FH-001 | Display initializes on cold boot | Debug console reports success, panel shows the expected startup state, no I²C errors |
| FH-002 | Panel renders a full-screen self-test pattern correctly | Pattern covers the whole display, correct size, no dead areas |
| FH-003 | All five buttons report the correct GPIO mapping to the host | Each button reports the correct id and press/release/long-press transitions (protocol.md §6.5) |
| FH-004 | Battery ADC reading is within tolerance | Reported millivolts within ±100 mV of an independent measurement |
| FH-005 | Backlight responds correctly across its range, including fade | Off at level 0, fully on at 255, fade timing within ±50 ms of the requested `fade_ms` |
| FH-006 | USB enumerates as our own CDC-ACM device after flashing | Our own VID/PID and product string, `/dev/cu.usbmodem*` node appears |

## 4. Host tests (HT) — automated pytest, integration cases need a device

| ID | Requirement |
|---|---|
| HT-001 | Host encodes a `HELLO` frame matching protocol.md §12 vector #2 exactly (CRC `0x60E06847`) |
| HT-002 | Host parses `HELLO_ACK` and extracts geometry, version, and capability bitmap correctly |
| HT-003 | Host discovers the device on `/dev/cu.usbmodem*` (needs real hardware) |
| HT-004 | Host reconnects cleanly after a USB unplug/replug, without crashing |
| HT-005 | Host loads an image, dithers to 1 bpp, and packs it into a protocol frame (2400 bytes for 240×80) |
| HT-006 | Both bundled dither algorithms (ordered/Bayer and Floyd–Steinberg) produce different, valid 1-bpp output |
| HT-007 | The dirty-rectangle module correctly identifies a changed region between two frames, with no false positives |

## 5. Hardware Integration tests (HI) — manual, device required

| ID | Requirement | Notes |
|---|---|---|
| HI-001 | Cold boot completes to a display-ready state within a reasonable time | Target < 5 s |
| HI-002 | `REBOOT` command produces a clean reboot and reconnect | CDC drops, device restarts, `HELLO`/`HELLO_ACK` succeeds again |
| HI-003 | Device survives a USB hot-unplug/replug with no data corruption | Display unchanged during the unplug; reconnect and handshake succeed |
| HI-004 | Device stays responsive across a host sleep/wake cycle | Dashboard updates pause during sleep and resume after wake; the device answers `PING` throughout |
| HI-005 | `charging` field in `STATUS`/`BATTERY` correctly reports idle/charging/full | Needs a charger available |
| HI-006 | `EVT_STATUS` fires when the battery crosses a reporting threshold | Skippable if battery monitoring isn't exercised in this round |

## 6. Display Rendering tests (DR) — manual, device + host pipeline required

| ID | Requirement |
|---|---|
| DR-001 / DR-002 | Solid black / solid white full-frame fills render with no stray pixels |
| DR-003 | A self-test checkerboard pattern renders correctly, no tearing or partial updates |
| DR-004 / DR-005 | 1px-pitch horizontal and vertical line patterns render sharp, with no missing rows/columns |
| DR-006 / DR-007 | Small and large fonts render legibly via `DRAW_TEXT`, no corruption |
| DR-008 | A full 240×80×1bpp refresh completes within a reasonable time budget | Directly informs whether dirty-rect diffing is worth the complexity — see display.md for the measured figure |
| DR-009 | `PARTIAL_REFRESH` updates only the named rectangle, not the whole screen |
| DR-010 | Rapid successive partial refreshes show no visible tearing | Relevant to the Mode 2 mirror path — see sample-projects/mirror-and-virtual-display.md |

## 7. Recovery and Safety tests (RS) — manual, device required, gated

| ID | Requirement |
|---|---|
| RS-001 | `BOOT_ORIGINAL` finds and boots the stock partition correctly |
| RS-002 | Holding EXECUTE for 3 s at power-on boots to stock, USB connected |
| RS-003 | Same escape hatch works with USB unplugged (battery-only) |
| RS-004 | **Invalid as written — do not run.** This test assumed the ESP-IDF app-rollback mechanism (`esp_ota_mark_app_valid_cancel_rollback`/`esp_ota_check_rollback_is_possible`) would revert to the stock slot after a watchdog timeout on a bad image. Both functions are confirmed absent from the vendor's own 1.1.0 image (zero string hits on disassembly), and independently confirmed absent from the bootloader's own anti-rollback configuration — see flash-layout-and-updater.md. **There is no automatic rollback on this device.** Deliberately installing a corrupt image commits the boot partition to firmware designed to hang, with no automatic recovery — a real, unrecoverable-without-manual-intervention risk to the unit, not a safe negative test. If this is ever run in a modified, valid form, it needs its own explicit owner sign-off separate from this document's general one, and must document up front that recovery is manual-only (an `esptool` erase of the boot-selector region to fall back to the factory slot — itself a separately gated, unrehearsed operation forbidden by SAFETY.md without explicit fresh approval). |
| RS-005 | Recovery via a spare SD card and a known-good stock firmware tar | Requires the stock tar's SHA-256 be verified before use |
| RS-006 | Full A→B→A cycle: our firmware → boot-original → reinstall our firmware → working |

**Gate on any test that powers the device with an SD-card update tar present** (the Phase-12
first-boot checklist, and RS-004/005/006 above): the stock updater begins the install automatically
at boot the instant an update tar is present at the expected path — there is no menu step, no
confirmation. This requires the owner's explicit sign-off before proceeding past pre-flight, every
time, not a one-time approval.

## 8. Acceptance criteria — what "done" means

| Requirement | Evidence | Test IDs |
|---|---|---|
| **A** — Original device function fully preserved | Bluetooth keyboard still works, USB disk mode, stock menu all reachable | RS-001, RS-002, RS-003, RS-005 |
| **B** — Verified backup exists before any write | SHA-256 matches on a two-pass dump | prior phase, outside this test plan |
| **C** — Recovery path tested and working | Device recovers via the SD updater and the escape hatch | RS-004, RS-005, RS-006 |
| **D** — No secure-boot/efuse changes | Disassembly inspection finds no eFuse API calls | code review |
| **E** — USB enumeration matches stock | Our own VID/PID, our own product string, device node exists | HI-006 / FH-006 |
| **F** — Protocol implemented and functional | Handshake, drawing, and framebuffer streaming all work end to end | FP-001 through FP-010, HT-001, HT-003 |
| **G** — Host application connects and renders | Host discovers the device, sends frames, display updates | HT-003, HT-004, DR-001 through DR-010 |
| **H** — Dashboard data sources update live | Battery, status, and button events flow correctly | FH-004, HI-002 |
| **I** — No secrets in git history | No dumps, credentials, or Wi-Fi configuration committed | file review |
| **J** — Documentation complete and matches implementation | docs/ tracks hardware constants, display specs, pin assignments as they're confirmed | doc review |

## 9. Current verification status (on-glass results)

This section is the running record of what has actually been confirmed against the physical
device, as opposed to what is only specified or only tested against a fake/mock transport.

| Requirement | Status | Evidence |
|---|---|---|
| **A** — Original device function fully preserved | **PENDING** | Escape-hatch tests (RS-001 through RS-003) scheduled but not yet all run |
| **B** — Verified backup exists | **CONFIRMED** | Two-pass SHA-256 match on the original dump |
| **C** — Tested recovery/rollback path | **PARTIAL** | SD-updater path (stock tar) **PASS** — unattended install, device returned to the normal menu, correct status-bar version string (see recovery.md and firmware.md). The `esptool`-based full/per-partition restore path remains draft and untested. |
| **D** — No secure-boot or efuse changes | **CONFIRMED** | No eFuse or secure-boot API calls found anywhere in the shipped image on disassembly |
| **E** — USB enumeration matches stock behavior | **CONFIRMED** | Disconnect/reconnect re-enumerates correctly, no device reboot, host resumes polling after replug |
| **F** — Link protocol implemented and functional | **PARTIAL** | Protocol logic is CONFIRMED on host-side and firmware-side unit tests (cross-checked byte-for-byte against each other, including protocol.md §12's vectors and a large randomized fuzz pass); on-device wire behavior against the physical panel is exercised for the message types covered by the on-glass results below, but FP-001 through FP-010 as a formal on-device acceptance pass remain the target |
| **G** — Host application connects and renders | **PARTIAL** | Host logic is CONFIRMED against a fake serial port (discovery, handshake, reconnect/backoff, framing, rendering); the on-device HELLO/HELLO_ACK handshake, drawing and frame streaming below have all been exercised live |
| **H** — Dashboard data sources update on device | **PARTIAL** | The link-level messages this depends on (`GET_STATUS`/`STATUS`, `GET_BATTERY`/`BATTERY`, `EVT_BUTTON`) are specified and implemented; §10's on-glass table below covers dashboard rendering specifically |
| **I** — No secrets/credentials/dumps in git history | **CONFIRMED** | Raw dumps and captures are excluded from version control; the one historical instance of a network name landing in an early commit was found, redacted, and documented |
| **J** — Documentation complete and matches implementation | **ONGOING** | Tracked per doc as each subsystem is confirmed |

## 10. On-glass verification — has our firmware actually driven the panel from the host?

A separate, narrower set of letters (A–J again, but a different table — do not conflate the two)
tracks specifically "does the Mac successfully drive the physical panel," independent of the
broader acceptance criteria in §8/§9:

| # | What was checked | Result |
|---|---|---|
| **A** | Device receives data from the host at all (USB CDC, protocol round-trip) | **CONFIRMED** — a clear command reached the device and cleared the panel; `GET_INFO`/`GET_STATUS`/`ACK` all verified on hardware |
| **B** | Known text renders correctly on the glass | **CONFIRMED** — text sent via `DRAW_TEXT` appeared correctly at the expected row after a scroll-line correction was applied (display.md has the full analysis) |
| **C** | Known image renders correctly on the glass | **CONFIRMED** — a self-test checkerboard and a text-demo pattern both displayed correctly; the full framebuffer-streaming path (`FRAME_*` + `FULL_REFRESH`) and the image-to-1bpp pipeline were both verified end to end |
| **D** | The dashboard renders and updates live on the device | **CONFIRMED** — a live dashboard loop ran against the device with a one-second interval; layout, alignment, and live clock ticking were all confirmed correct after fixing an early rendering-alignment bug (troubleshooting.md has the incident writeup) |
| **E** | USB enumeration matches stock behavior post-modification | **CONFIRMED** — same evidence as §9's row E |
| **F** | Device recovers cleanly after a restart (soft reset, power cycle) | **CONFIRMED** — both a soft reset and full power cycles return cleanly to the menu; a shutdown-path crash found during this testing was fixed and re-verified (troubleshooting.md) — stock-parity shutdown behavior (USB-power refusal message, backlight fade, release-wait, GPIO state) confirmed |
| **G** | Original (stock) mode is fully restorable via the recovery path | **CONFIRMED for the SD-updater path** — stock firmware installed unattended via SD card, device returned to its normal menu with the correct version string; the `esptool`-based partition-level restore path remains draft/untested |
| **H** | A Mac-driven virtual/mirrored display (Mode 2) is documented and tested in situ | **DEMONSTRATED, out of the current install scope** — both halves of Mode 2 (window/display capture, and a virtual display the Mac genuinely owns) were shown working live against the physical panel; the owner then chose to park Mode 2 out of the day-to-day install scope while keeping the code, tests and docs as a working reference — see sample-projects/mirror-and-virtual-display.md |
| **I** | Recovery is documented (escape hatch, rollback risk, all stages) | **CONFIRMED** — recovery.md and troubleshooting.md between them cover every failure mode hit during development and the escape route for each |
| **J** | No hidden destructive-risk issue; secure boot/efuse/flash-encryption untouched | **PARTIAL** — no secure-boot or eFuse changes confirmed by disassembly; no secrets in git history beyond the one documented, redacted instance; the OTA-partition write gate defaults off and requires an explicit rebuild to arm; open items are the exact charger-IC pin identification (STRONGLY INDICATED, not schematic-confirmed — see hardware.md) and the measured-vs-assumed battery divider ratio (see hardware.md) |

## 11. What remains untested

- **`esptool`-based full/per-partition flash restore** (as opposed to the SD-updater path, which
  is confirmed working) — remains a draft procedure, not rehearsed against this unit.
- **Writing to, or restoring, the shared NVS partition** — the OTA app-partition write path has a
  rehearsed recovery; the NVS partition does not, which is why the persistence gate described in
  protocol.md §10 defaults off and is called out explicitly wherever it's overridden.
- **Formal on-device execution of FP-001 through FP-010** as a single pass against real hardware,
  as opposed to the cross-checked host/firmware unit-test coverage that stands in for it today.
- **Mode 2 (mirror/virtual display)** beyond the window-capture and virtual-display demonstrations
  already run — longer-duration sessions, reconnect behavior under load, and additional capture
  sources are all unexercised; see sample-projects/mirror-and-virtual-display.md for exactly what
  was and wasn't run.
- **Exact charger IC pin labeling** and the **measured battery-divider ratio** (both hardware.md
  items, both currently inferred from firmware rather than confirmed against a schematic or a
  direct measurement).
- **TCC/Screen-Recording permission behavior for an unbundled command-line capture helper** — noted
  in sample-projects/mirror-and-virtual-display.md as resolved in practice on the one machine this
  was built on, but not verified as a general property of an unsigned, unbundled binary.

## 12. Test patterns and fixtures

Test patterns (1-bpp, 240×80, generated by a small helper script under `host/tools/`) live in
`tests/patterns/`: solid black, solid white, 1px and 8px checkerboards, 1px and 2px horizontal and
vertical line grids, and a multi-line text-rendering demo. protocol.md §12's CRC test vectors are
the normative reference for anything that needs a known-good CRC over a specific payload; the host
and firmware test suites both check their CRC implementation against those vectors directly rather
than against each other's output, so a bug shared between them can't hide.

Run the automated suites:

```sh
cd host/macos
source .venv/bin/activate
BYOK_FORCE_MOCK=1 python3 -m pytest ../../tests/host -q      # host suite, no device
```

Firmware-side protocol unit tests build and run under ASan/UBSan on the host toolchain (no device
needed) and include a large randomized fuzz pass in addition to the fixed vectors — see the test
harness under `tests/proto/` for the exact invocation.
