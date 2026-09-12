# Hardware Test Checklist

**Purpose:** Fillable checklist for owner-performed hardware tests, grouped by area rather than
by any particular sequence — run the sections that apply to what you changed.  
**Instructions:** Print or use as a live document. Mark each test with:
- ✅ PASS — test completed, expected result observed
- ❌ FAIL — test completed, result unexpected; note issue
- ⏭️ SKIP — test deferred or not applicable
- ⏸️ PENDING — not yet run

---

## First Install (Checkerboard Smoke Test)

### Pre-flight Checklist

| # | Item | Status | Notes |
|---|---|---|---|
| 1 | Backup dump verified (two-pass SHA256 match) | ⏭️ | See docs/recovery.md §1 (take the backup) and §5 (verify it) |
| 2 | Spare SD card rehearsal with vendor 1.1.0 complete | ⏭️ | Device booted stock, verified working |
| 3 | Escape-hatch button-hold path in place (`firmware/s3/main/app_main.c`) | ⏭️ | Built and linked into firmware |
| 4 | hw_config.h populated (display I2C, GPIO, resolution, init, backlight) | ⏭️ | Minimum constants for this test defined, no #error warnings |
| 5 | Build succeeds: `idf.py build` clean, image ≤ 3 MiB | ⏭️ | esptool.py image_info valid |
| 6 | SD card `/SDCARD/Updates/BYOK.tar` ready | ⏭️ | Contains fresh BYOK.bin from this build |
| 7 | Camera ready for photos | ⏭️ | Mac with Photos app or USB camera ready |
| 8 | Stopwatch ready (phone or `time` command) | ⏭️ | Will measure install latency |
| 9 | USB-Serial-JTAG monitor open | ⏭️ | `tail -f /dev/tty.usbserial-* 2>/dev/null \| grep .` or screen/minicom |

### First Install

| # | Step | Status | Time (ms) | Notes |
|---|---|---|---|---|
| 1 | Power OFF device completely | ⏭️ | — | Remove USB if needed |
| 2 | Insert SD card with BYOK.tar | ⏭️ | — | — |
| 3 | Power ON, menu appears | ⏭️ | 0 | Stopwatch START |
| 4 | Select "Update Firmware" | ⏭️ | ~T1 | Note menu name if different |
| 5 | Progress bar / install begins | ⏭️ | ~T1 | May show "Installing..." or similar |
| 6 | First pixel change observed | ⏭️ | ~T2 | Checkerboard or status screen appears |
| 7 | Checkerboard fully stable | ⏭️ | ~T3 | Stopwatch STOP |
| 8 | Panel resolution printed in log | ⏭️ | — | Screenshot console showing "display W×H" |
| 9 | No USB errors in monitor | ⏭️ | — | Check for "CRC error" or "timeout" messages |

**Timing Summary:**
- Install latency (T3 - 0): ___ ms
- Time to first pixel (T2 - T1): ___ ms

### Post-Install Verification

| # | Test | Status | Pass Criteria | Notes |
|---|---|---|---|---|
| 1 | Checkerboard visible | ⏭️ | 8×8 pattern, full screen, no tearing | Photo: `checkerboard-full.jpg` |
| 2 | Display orientation | ⏭️ | Not rotated/mirrored, 240 px wide visible | Measure with ruler if possible |
| 3 | No dead pixels or artifacts | ⏭️ | All pixels responding, uniform shading | Inspect entire screen |
| 4 | Escape hatch test 1 (button hold at boot) | ⏭️ | Boots to stock v1.1.0 menu | Press EXECUTE at power-on, hold 3 s |
| 5 | Stock firmware works | ⏭️ | Menu responsive, Bluetooth keyboard (if paired) responds | Try selecting a menu item |
| 6 | Escape hatch test 2 (USB unplugged) | ⏭️ | Boots to stock without USB, battery only | Repeat test 1 with USB unplugged |
| 7 | Reboot into our firmware | ⏭️ | Checkerboard reappears after power cycle | Either via menu or SD updater re-install |

---

## Protocol and CDC Transport

### CDC Transport and Text

| # | Test | Status | Expected | Notes |
|---|---|---|---|---|
| 1 | Device enumerates as CDC-ACM | ⏭️ | `/dev/cu.usbmodem*` appears on Mac | `lsusb -v \| grep BYOK` or check System Report |
| 2 | HELLO/HELLO_ACK handshake | ⏭️ | Device replies with version, geometry, caps bitmap | `byok info` command or manual protocol send |
| 3 | byok text "hello" works | ⏭️ | Text appears on panel in < 200 ms | Time with `time byok text "hello"` |
| 4 | byok status returns battery/uptime/heap | ⏭️ | Values are sane, match device display if visible | Compare with panel reading if available |
| 5 | Link survives unplug/replug mid-transaction | ⏭️ | No crash, reconnection succeeds after HELLO | Unplug USB during text rendering, replug, reconnect |
| 6 | 1000 frames with no resync event | ⏭️ | No NACK/E_SEQ_GAP or CRC errors in log | Loop: send PING, record resync count |
| 7 | Button events (GPIO mapping correct) | ⏭️ | EVT_BUTTON reports correct button ID for each GPIO | Press UP (button=1), DOWN (2), EXECUTE (3), BRIGHTNESS (4), WAKE (5) |

---

## Images, Dashboard, and Mirror Mode

### Images and Dithering

| # | Test | Status | Expected | Notes |
|---|---|---|---|---|
| 1 | byok image photo.jpg renders | ⏭️ | Full frame sent via FRAME_*/CRC, displayed < 2 s | Supply a small test JPEG |
| 2 | Ordered (Bayer) dither side-by-side with F-S | ⏭️ | Both paths produce valid 1-bpp frames, visual difference noted | `byok image --dither ordered` vs `--dither floyd` |
| 3 | Frame CRC mismatch → NACK | ⏭️ | Panel keeps previous frame, no half-draw | Corrupt one FRAME_DATA chunk CRC |
| 4 | Full-frame refresh rate measured | ⏭️ | Record time for 240×80×1bpp: ≤ 2000 ms | Measure end-to-end: FRAME_BEGIN → FRAME_END ACK |

### Dashboard

| # | Test | Status | Expected | Notes |
|---|---|---|---|---|
| 1 | byok dash renders status page | ⏭️ | Time, battery %, host CPU/memory on panel | `byok dash` command |
| 2 | Dashboard updates with dirty rects | ⏭️ | Only changed regions sent, byte count < full frame | Monitor bytes-per-update in logs |
| 3 | Full frame sent ≤ once per minute | ⏭️ | Partial updates only, steady-state < 100 B/s | 8-hour idle test: heap unchanged ±2% |
| 4 | Host disconnect → device shows IDLE screen | ⏭️ | Device returns to stock IDLE mode after 30 s no-traffic timeout | Unplug USB, wait 30 s, observe panel |
| 5 | Reconnect resumes dashboard | ⏭️ | Host reconnects, handshake succeeds, updates resume | Replug USB, send HELLO |

### Mirror Mode (ScreenCaptureKit)

| # | Test | Status | Expected | Notes |
|---|---|---|---|---|
| 1 | Mirror captures primary or window | ⏭️ | Desktop / window contents streaming to device at cap rate | `byok mirror --fps 5` |
| 2 | Dirty rects used for updates | ⏭️ | Only changed regions sent, FRAME_BEGIN partial mode | Monitor byte counts |
| 3 | Sustains ≥ 5 fps (or honest measured rate) | ⏭️ | Smooth text-editor motion on device | Measure latency end-to-end |
| 4 | Screen Recording permission handled | ⏭️ | Clear prompt, error message if denied | First run on fresh Mac account |
| 5 | Static screen stops sending frames | ⏭️ | No frame updates while display idle | Open text editor, type nothing, monitor for 60 s |

---

## Original-Mode Switch and Recovery

### Boot-Original and Recovery

| # | Test | Status | Expected | Notes |
|---|---|---|---|---|
| 1 | byok boot-original command works | ⏭️ | Device replies ACK, reboots to stock, menu visible | `byok boot-original` |
| 2 | Boot-original survives 10 round-trips | ⏭️ | All 10 cycles successful, no corruption | Automate: send BOOT_ORIGINAL 10×, verify each time |
| 3 | Button-hold escape hatch works 10× | ⏭️ | EXECUTE held at boot → stock, repeat 10× with USB connected | Manual 10-cycle test |
| 4 | Escape hatch works with USB unplugged 10× | ⏭️ | EXECUTE held at boot → stock, battery only, repeat 10× | Manual 10-cycle test, no USB |
| 5 | SD updater reinstall (A→B→A cycle) | ⏭️ | Our firmware → stock (button/command) → our firmware → working | Full round-trip |
| 6 | docs/recovery.md verified complete | ⏭️ | docs/recovery.md promoted from DRAFT to VERIFIED | Review doc for contingencies |

### PICO Bluetooth Keyboard (Optional)

| # | Test | Status | Expected | Notes |
|---|---|---|---|---|
| 1 | Stock UART protocol passthrough (read-only) | ⏭️ | EVT_KEYP events flow from PICO to S3 | Monitor UART1 logs for >EVT\|KEYP\|< events |
| 2 | Keyboard works under our firmware | ⏭️ | Bluetooth keyboard input functional in our app | Pair keyboard, test typing on device screen text input |

---

## Verification and sign-off

### Overall Verification Summary

| # | Requirement | Status | Evidence Link | Grade |
|---|---|---|---|---|
| A | Original device function preserved | ⏭️ | RS-001, RS-002, RS-005 | CONFIRMED |
| B | Verified backup exists | ⏭️ | docs/recovery.md §1, §5 | CONFIRMED |
| C | Recovery/rollback tested | ⏭️ | RS-004, RS-005, RS-006 | CONFIRMED |
| D | No secure-boot/efuse changes | ⏭️ | docs/firmware.md (disasm review) | CONFIRMED |
| E | USB enumeration matches | ⏭️ | HI-006, docs/usb-and-boot-modes.md §2 | CONFIRMED |
| F | Protocol implemented | ⏭️ | FP-001..010, HT-001..007 | CONFIRMED |
| G | Host renders and connects | ⏭️ | DR-001..010, HT-003..004 | CONFIRMED |
| H | Dashboard data flow | ⏭️ | Dashboard tests above, HI-002 (PING/PONG) | CONFIRMED |
| I | No secrets in git | ⏭️ | .gitignore check, git log scan | CONFIRMED |
| J | Documentation complete | ⏭️ | docs/ review (hw_config.h, protocol.md, recovery.md, display.md, pinout.md) | CONFIRMED |

### Known Issues (If Any)

_Example row (delete before filling in your own): `I2C refresh slow` /
`use ordered dither, reduce fps, batch writes` / `Medium` / `Open`._

| Issue | Workaround | Severity | Status |
|---|---|---|---|
| | | | |
| | | | |

### Sign-Off

| Role | Name | Date | Signature |
|---|---|---|---|
| Device Owner | __________ | ____-__-__ | _______ |
| Hardware Engineer (optional) | __________ | ____-__-__ | _______ |
| Software Engineer (optional) | __________ | ____-__-__ | _______ |

---

## Notes

- Record all timing measurements in **milliseconds**
- All photos should be named systematically: `checkerboard-full.jpg`, `FH-004-battery-multimeter.jpg`, etc.
- Failures: record a photo, the serial log excerpt, and the exact time in your own notes
- For the Verification and sign-off tests, link to docs/testing.md test ID (e.g., "FP-008 failed: see docs/testing.md §FP-008 for details")

