# Display

The device's LCD panel is driven by the ESP32-S3 over I²C, at 240×80 pixels, 1 bit per pixel, by a
UC1611/UC1611s-class monochrome LCD controller. Every number in this document was recovered by
disassembling the stock vendor firmware and cross-checking it against the published UltraChip
UC1611 command table; the machine-usable constants live in `firmware/common/hw_config.h` §§1–3, and
this document is the narrative account of how each one was established, plus everything that is
still unresolved.

**Evidence labels** — CONFIRMED / STRONGLY INDICATED / POSSIBLE / UNKNOWN — follow [SAFETY.md](../SAFETY.md) §2.
Diagram: [`diagrams/panel-framebuffer-and-refresh.svg`](diagrams/panel-framebuffer-and-refresh.svg)
(geometry, framebuffer packing, and the full bring-up sequence in one picture) — source and
facts table: [`diagrams/panel-framebuffer-and-refresh.md`](diagrams/panel-framebuffer-and-refresh.md).

---

## 1. Headline

| Question | Answer | Grade |
|---|---|---|
| **Interface** | I²C, not SPI — port 0, SDA = GPIO 18, SCL = GPIO 10, 1 MHz, internal pull-ups enabled in this driver, XTAL clock source, glitch filter 7 | CONFIRMED |
| **Addresses** | Two 7-bit addresses on one physical device: `0x38` = command, `0x39` = data (they differ only in bit 0, the controller's CD bit folded into the slave address) | CONFIRMED |
| **Controller** | UC1611 / UC1611s class | STRONGLY INDICATED |
| **Resolution** | 240 × 80 pixels | CONFIRMED |
| **Colour depth** | 1 bit per pixel (monochrome; the UC1611's 2-bit grey mode is not used) | CONFIRMED |
| **Framebuffer** | 2400 bytes, page-major: `index = (y >> 3) × 240 + x`, 10 pages of 240 bytes | CONFIRMED |
| **Polarity** | Driven inverted (`0xA7` = Set Inverse Display ON); `0xFF` is the blank byte | CONFIRMED |
| **Which chip drives it** | The ESP32-S3 — the entire display driver, its I²C bus and its framebuffer live in the S3's own firmware image; none of it is present in the co-processor's image | CONFIRMED |
| **Reset line** | None — the panel has no reset GPIO; bring-up is I²C-only | CONFIRMED negative |
| **Backlight** | GPIO 2, LEDC low-speed timer 0 / channel 0, 5 kHz, 13-bit (max duty 8191), fade-on-change; levels {0, 4, 20, 50, 100} % | CONFIRMED |

This closes the interface question that a purely mechanical inspection of the flex connector could
not settle on its own (§2 below): a 14-way flex carrying SDA, SCL, a reset-less power rail,
backlight, and a generous ground/VLCD allocation is exactly what a UC1611-class chip-on-glass panel
needs, and no SPI peripheral in the firmware touches a display at all.

---

## 2. Physical interconnect

The panel connects through a single FPC/FFC connector on the board's left edge: a bottom-contact
ZIF with a flip/slide actuator, mounted vertically, contacts facing inboard, with an amber/tan
polyimide flex entering from the left and folding around the board edge to run underneath the PCB —
consistent with the panel sitting behind/below the main board inside the case. The connector carries
**14 contacts at roughly 1.0 mm pitch**, spanning about 13 mm — a count and pitch resolved by direct
inspection under magnification and cross-checked against two independent reference dimensions on
the board.

Fourteen conductors is too few for a classic 24-pin e-paper FPC interface (the industry-standard
EPD pinout needs BUSY/RST/DC/CS/SCK/MOSI plus panel-power/pump pins) and rules out a parallel RGB
bus outright, but on its own it does not name the panel technology — a 14-way interface is a
constraint on the connector, not an identification of what is on the other end of it. That
identification came from the firmware disassembly in §1, not from the connector: I²C needs far
fewer conductors than a parallel bus, and 14 pins comfortably covers SDA, SCL, an unreset supply
rail, a PWM backlight feed, and ground/VLCD — matching §1's findings exactly.

No separate display controller or driver daughterboard is visible near the connector, no
component near it carries a panel part number or FPC part number, and — before firmware disassembly
resolved the electrical picture — no dedicated e-paper power stage (the boost/pump ladder that class
of panel typically needs close to its connector) was found anywhere on the board either, which was
one of several early indicators pointing away from e-paper and toward the FSTN-LCD reading in §1.

**What is still UNKNOWN about the physical panel:** its exact size and dot pitch as manufactured,
any printed part number or model string (these are normally printed on the flex itself, which has
never been read), whether 240×80 is the panel's true native resolution or a window programmed into
larger glass (the controller's own "last COM = 79" setting only tells the *driver* there are 80
active rows; a UC1611s can address up to 160), a front-light or touch layer, and which of the 14
contacts carries which signal (no continuity trace has been made from the connector to either
processor). None of these affect the interoperability specification in §§3–4 below, which comes from
the firmware side of the interface, not the connector side.

---

## 3. I²C transaction model

The vendor driver — and this project's own driver, which reproduces it exactly — talks to the panel
with the plainest possible I²C usage: one real hardware `i2c_master_transmit()` call per logical
write, no reads, no strobe line, no control byte, ACK checking disabled on both device handles.
There is no expander, no parallel bus hiding behind the two addresses, and no bit-banging — "the
vendor's I²C helper function is internally named as if it were simulating a bus" turned out to be
an artifact of its own naming, not evidence of one; it calls the real ESP-IDF I²C-master driver like
any other caller.

| Property | Value | Grade |
|---|---:|---|
| Bus | I²C port 0, SDA GPIO 18, SCL GPIO 10 | CONFIRMED |
| Bus speed | 1 MHz | CONFIRMED |
| Clock source | XTAL | CONFIRMED |
| Glitch filter | 7 | CONFIRMED |
| Command address | `0x38`, 7-bit | CONFIRMED |
| Data address | `0x39`, 7-bit | CONFIRMED |
| Per-transaction timeout | 1000 ms | CONFIRMED |
| ACK checking | Disabled on both device handles | CONFIRMED |
| Bytes per transaction (per-byte path) | 1 | CONFIRMED |

### 3.1 Per-byte path — the vendor's own, and this project's default

Every command byte, every parameter byte, and every pixel byte is its own one-byte
`i2c_master_transmit()` call — command bytes to `0x38`, data bytes to `0x39` — with no bulk transfer
and no DMA anywhere in the vendor's own firmware. A full-screen refresh is 2400 pixel bytes plus a
12-transaction window-program preamble, i.e. **2412 transactions**. On real hardware this measured
**220,459 µs for one full refresh, ≈91 µs/transaction** — call/scheduling overhead on the ESP-IDF
driver stack, not I²C bus time (a single byte at 1 MHz takes on the order of 10 µs of actual bus
time). This means the update rate of a per-byte implementation is bound by transaction *count*, not
by bus bandwidth — a driver that batches the payload into longer writes has a lot of headroom to be
faster, at no cost to correctness, because it uses the exact same controller feature described next.

| Operation | Transactions |
|---|---:|
| One command byte | 1 |
| One double-byte command (opcode + parameter) | 2 |
| One pixel byte | 1 |
| Full window-program preamble | 12 |
| Full-screen refresh (2400 px bytes) | 2412 |
| Power-on init (reset preamble + 21-entry table) | 23 |

This is the path this project ships by default, specifically *because* it is verified: every I²C
bus parameter, every reset-preamble byte, the full 21-entry init table, the window-program opcode
sequence, page-alignment, framebuffer addressing, and the blank/inverse-video convention were
audited byte-for-byte and timing-for-timing against the disassembled vendor sequence and found
identical, with no wire-protocol defect anywhere in it.

### 3.2 Bulk path — a protocol-legal optimisation, not yet exercised at scale

The controller auto-increments its own column address after each data byte written inside a
programmed window (§5 below). That means a full page row — or, when the window spans the panel's
full width, the entire window — can be sent as **one** `i2c_master_transmit()` call to the data
address instead of one call per byte. This is a real, controller-documented capability, not a guess
about undocumented behaviour, and this project's driver implements it as a runtime-selectable
alternate path: a full-width refresh becomes one transaction per 8-row page (at most 10 transactions
for a whole screen) instead of 2400.

Measured on real hardware, a full-panel bulk-mode refresh completed in **≈29–37 ms**, against the
per-byte path's ≈220 ms for the same amount of pixel data — consistent with the per-byte path being
call-count-bound rather than bus-bound, as §3.1 predicts. Command traffic (the reset preamble, the
init table, window-program addressing, contrast) is always sent one byte per transaction regardless
of which pixel-data path is selected, since it is a negligible fraction of a refresh's total byte
count and preserving it byte-for-byte costs nothing. The bulk path is available at runtime (a host
can request either path) but per-byte remains the shipped default, since it is the path with full
byte-for-byte vendor-parity evidence behind it.

---

## 4. Panel initialisation sequence — interoperability specification

Bring-up has three parts, replayed in this exact order: a reset preamble, a 21-entry register
table, and (after the very first full frame is written) a one-time display-enable step. This is
presented as a specification because it is one: any implementation driving this controller family
over this bus needs exactly this byte sequence, in exactly this order, regardless of what firmware
issues it — this project's own driver replays it verbatim for that reason, including the opcodes
whose meaning is not fully known (see §4.3).

### 4.1 Reset preamble

1. Create the I²C bus and the two device handles. No GPIO is touched — the panel has no reset line.
2. Delay 150 ms.
3. Send **command `0xE1`**.
4. Send **data `0xE2`**.
5. Delay 150 ms.
6. Replay the 21-entry table below.

Grade: CONFIRMED for the byte sequence and timing. The meaning of `0xE1` is UNKNOWN (§4.3). `0xE2`
is the UC1611's documented "Set System Reset" opcode — but the datasheet requires it to be sent to
the *command* address (CD=0) to take effect, and this preamble sends it to the *data* address
(CD=1). Per the datasheet's own words, that write is decoded as a byte written into display RAM,
not as a command, and does **not** invoke System Reset — CONFIRMED from the datasheet text, not
inference. This is not a defect: a real reset is not needed here, because the panel's own power-on
reset already puts it in a known state before this preamble runs, and the very next step in the
21-entry table below (`0x88`, Set RAM Address Control) overwrites whatever column-pointer side
effect that data byte might otherwise have had.

### 4.2 The 21-entry register table

Replayed byte-for-byte, in order, as a single-byte-per-transaction sequence. Every opcode below was
checked against the published UltraChip UC1611 (Rev 0.81) command table; **12 of the 15
command-address bytes, plus both double-byte-command parameter shapes, match a real UC1611 opcode
exactly, including its documented command/data (C/D) addressing bit** — strong evidence that the
`0x38`=command / `0x39`=data assignment is the correct polarity (§6 has the full derivation). Three
opcodes have no match in the published table and are marked UNKNOWN; see §4.3 for why that is
expected rather than alarming.

| # | Address | Byte | Decoded meaning | Grade |
|---|---|---|---|---|
| 0 | cmd | `0xC9` | UNKNOWN — double-byte opcode, parameter follows | UNKNOWN |
| 1 | data | `0xAC` | …its parameter | UNKNOWN |
| 2 | cmd | `0x2D` | Set Pump Control (`0x2C \| PC=1`) | CONFIRMED (opcode match) |
| 3 | cmd | `0x24` | Set Temperature Compensation, TC = 0 | CONFIRMED (opcode match) |
| 4 | cmd | `0xE9` | Set LCD Bias Ratio (`0xE8 \| BR=1`) | CONFIRMED (opcode match) |
| 5 | cmd | `0xA3` | Set Line Rate (`0xA0 \| LC=3`) | CONFIRMED (opcode match) |
| 6 | cmd | `0xC4` | Set LCD Mapping Control: MY=1, MX=0, MSF=0 | CONFIRMED (opcode match) |
| 7 | cmd | `0x88` | Set RAM Address Control, AC=0 | CONFIRMED (opcode match) |
| 8 | cmd | `0x81` | Set VBIAS Potentiometer (double-byte) | CONFIRMED (opcode match) |
| 9 | data | `0x50` | …contrast register = 80 | CONFIRMED (parameter shape) |
| 10 | cmd | `0xC8` | UNKNOWN — double-byte opcode, parameter follows | UNKNOWN |
| 11 | data | `0x39` | …its parameter | UNKNOWN |
| 12 | cmd | `0xF1` | Set COM End (double-byte) | CONFIRMED (opcode match) |
| 13 | data | `0x4F` | …last COM = 79 → 80 rows | CONFIRMED (parameter shape) |
| 14 | cmd | `0xF2` | Set Partial Display Start (double-byte) | CONFIRMED (opcode match) |
| 15 | data | `0x00` | …= 0 | CONFIRMED (parameter shape) |
| 16 | cmd | `0xF3` | Set Partial Display End (double-byte) | CONFIRMED (opcode match) |
| 17 | data | `0x4F` | …= 79 | CONFIRMED (parameter shape) |
| 18 | cmd | `0x85` | Set Partial Display Control (`0x84`–`0x87` family) | CONFIRMED (opcode match) |
| 19 | cmd | `0x95` | UNKNOWN — no match in the published command table | UNKNOWN |
| 20 | cmd | `0xA7` | Set Inverse Display ON | CONFIRMED (opcode match) |

There is no `0xD0`–`0xD7` (Set Color Pattern / Set Color Mode) anywhere in the table, so the panel
stays in its reset colour mode — consistent with the 1 bpp framebuffer addressing in §5.

### 4.3 The three unplaced opcodes, and why that is expected

`0xC9`, `0xC8`, and `0x95` do not match any row in the public UC1611 Rev-0.81 command table. This is
expected, not contradictory: that datasheet revision (2003) documents only the base UC1611's
parallel and SPI interfaces — it has no I²C mode at all. This board is STRONGLY INDICATED to use an
I²C-capable sibling part ("UC1611s") that the Rev-0.81 sheet simply does not cover; an I²C variant
revision extending or renumbering a handful of commands while preserving the base part's addressing
grammar is exactly the kind of change that would leave 12 of 15 opcodes matching exactly and only
the variant-specific extensions failing to match. Critically, all three unplaced bytes are on the
*command* side of the table — none of them would change the command/data-polarity conclusion in §6
even if their specific meaning were known.

**Policy: replay every byte in this table verbatim, regardless of whether its meaning is known.**
Do not omit or "translate" an unresolved opcode — see `firmware/common/hw_config.h` §3.2 for the
machine-readable form of this same table and its build-time UNKNOWN guards.

---

## 5. Display-enable step — the one step early releases of this project omitted

After the reset preamble and the 21-entry table, the vendor firmware performs one further step
**exactly once**, immediately after the very first full frame has been written to display RAM:
it sends **command `0xC9`** (the same double-byte opcode as init-table entry 0 above) with
**data `0xAD`** — differing from that entry's own parameter (`0xAC`) in bit 0 only.

| Claim | Grade |
|---|---|
| `0xC9` is a double-byte command with an 8-bit parameter | CONFIRMED |
| Its parameter's bit 0 is a boolean, toggled exactly once, immediately after the first frame | CONFIRMED |
| That boolean means "display enable" | STRONGLY INDICATED — position in the bring-up sequence and no other candidate meaning fit; `0xC9` has no row in the published UC1611 command table, so this cannot be checked against a datasheet |
| Omitting it leaves a correctly initialised, correctly fed panel dark | STRONGLY INDICATED from the reasoning above; **CONFIRMED on real hardware** — see below |

This step was missing from this project's driver through its early releases: every write ACKed,
error counts stayed at zero, and the byte stream reaching the controller was already byte-for-byte
identical to the vendor's own sequence — and the panel stayed dark regardless. Adding this one step,
in the same position the vendor uses it (right after the first full-frame write, both at
initialisation and on a full reinit), was the fix: the panel rendered on the very next boot after it
was added, confirming the "display enable" reading directly rather than by inference alone.

---

## 6. Address polarity — command/data assignment CONFIRMED correct

An early on-device read-back looked, at first glance, like the signature of a pair of I²C GPIO
expanders sitting behind the two addresses (a plausible reading, since `0x38`–`0x3F` is also the
PCF8574A's address range): reading both addresses returned identical values that changed together
over time, rather than the independent values two real port-expander registers would hold. Tracing
the vendor's own low-level send function instruction-by-instruction settles this: it is exactly one
`i2c_master_transmit()` call per byte, with no strobe, no control byte, and no read path anywhere in
either firmware image. The two addresses are the UC1611 family's own I²C addressing scheme — the
controller's command/data (CD) pin folded into the low bit of a single I²C slave address — not two
independent expander chips. The identical read-back values are explained more simply: both reads
were sampling the same electrical node (the shared SDA line sitting idle, then stuck low), not two
independent registers.

A second concern followed from the reset preamble in §4.1: the vendor sends `0xE2` — the UC1611's
own "System Reset" opcode — to the *data* address, which reads as nonsensical if `0xE2` really means
System Reset there. This raised the question of whether the command/data assignment might in fact be
inverted (i.e. the "data" address is really command, and vice versa). Re-deriving the assignment two
independent ways settles this too:

- **From the machine code.** The vendor's low-level send routine branches on one flag: input zero
  unconditionally reaches the handle configured with address `0x38`; input nonzero unconditionally
  reaches the handle configured with address `0x39`. There is no other path into either handle.
- **From the published UC1611 datasheet.** Fetching the actual UltraChip UC1611 datasheet and
  checking every opcode in the 21-entry table (§4.2) against its documented command/data (C/D) bit:
  **16 of 19 checkable table entries** — 12 exact single-byte opcode matches plus 4 exact
  double-byte-command parameter-shape matches — agree with the existing `0x38`=command/`0x39`=data
  assignment, and **none contradicts it**. A real inversion would require all 16 of those exact
  matches to be coincidences.

| Question | Finding | Grade |
|---|---|---|
| Is the command/data assignment inverted? | No | CONFIRMED (16/16 agreeing datasheet matches, plus the unconditional branch trace) |
| Does the `0xE1`/data-`0xE2` preamble issue a real System Reset? | No — `0xE2` requires the command address to activate System Reset; the preamble sends it to the data address | CONFIRMED (datasheet text) |
| Is the read-back pattern evidence of I²C expanders? | No | CONFIRMED negative |

---

## 7. Framebuffer, windowing and partial refresh

* **Framebuffer:** 2400 bytes, cleared to `0xFF` at init. Because the init table's last entry is
  `0xA7` (Set Inverse Display ON), `0xFF` renders as *blank* and `0x00` as *all pixels on* under
  that inversion.
* **Byte index:** `(y >> 3) × 240 + x` — CONFIRMED from the driver's own address arithmetic. Bit
  order within a byte (which row of the 8-row page each bit represents) was originally a
  best-effort guess, self-flagged as never observed on real pixels; a first-light photograph of this
  project's own rendered output showed upright, continuous glyphs spanning an 8-row page boundary,
  which a wrong bit order would have mirrored — **upgraded from STRONGLY INDICATED to CONFIRMED**
  by that observation.
* **Window program**, issued before every refresh (full or partial), addressed in pixel coordinates
  and converted to pages internally:

  ```
  0x89                    Set RAM Address Control, AC = 1
  0xF8                    Set Window Program Mode = OFF
  0xF5, (y_start >> 3)    window start PAGE
  0xF7, (y_end   >> 3)    window end   PAGE
  0xF4, x_start           window start COLUMN
  0xF6, x_end             window end   COLUMN
  0xF9                    Set Window Program Mode = ON
  0x01                    trailer (meaning UNVERIFIED)
  ```

  CONFIRMED, except the final `0x01` trailer byte's specific meaning.
* **One window per refresh, not one per page:** a full or partial refresh issues exactly one
  window-program call covering the whole (possibly multi-page) dirty rectangle, then streams bytes
  page-row by page-row inside that single programmed window, relying on the controller's own column
  auto-increment rather than re-addressing per page.
* **Partial-update granularity:** the vertical start of a dirty rectangle is always snapped **down**
  to a page boundary (8-row granularity); horizontal granularity is one column.
* **Nothing is sent after the pixel data** of a refresh — no trailing hardware command follows the
  last data byte.

---

## 8. Scroll-line / vertical display origin — a 15-row offset, and its fix

The first photograph of this project's own rendered output (a resting self-test screen, once the
display-enable step in §5 was added) confirmed the panel renders correctly in every respect except
one: the image sat **15 pixel rows too high**. The top 15 rows of the framebuffer were pushed off
the top of the glass, and the 15 rows of glass vacated at the bottom showed whatever the controller's
own RAM happened to hold there. The shift is not a multiple of 8 (so it is not a page/window-program
addressing error) and the image was not mirrored within pages (so bit order, confirmed correct by
the same photograph, is not the cause).

**Cause — STRONGLY INDICATED:** the UC1611's row-granular vertical display origin, "Set Scroll
Line" (opcode pair `0x40 | SL[3:0]` / `0x50 | SL[7:4]`), is the controller's only register that
shifts the whole image vertically without touching RAM. Neither this project's firmware nor the
vendor's own firmware ever programs it anywhere — an exhaustive scan of every low-level send call
in the vendor's disassembled firmware, plus this project's own 21-entry table, finds no byte in the
`0x40`–`0x7F` range sent by either. Because the panel has no reset GPIO and nothing in either
bring-up sequence issues a real System Reset (§4.1), both firmwares simply inherit whatever value
this register happened to power up holding — 15, on the unit this was observed on.

**Fix:** send `0x50` then `0x40` (both to the command address) at the end of the reset/init
sequence, forcing the scroll-line register to 0. The MSB-first order is deliberate: it lands on
`SL=0` under either a UC1611-style split LSB/MSB pair or a single 6-bit `0x40`–`0x7F` decode,
whereas the opposite order would leave a residual offset under the latter interpretation. This step
is strictly additive — it is not part of the vendor's own sequence, and its default is on.

---

## 9. Contrast, text metrics, backlight

**Contrast.** `cmd 0x81` (already in the init table, §4.2) followed by `data = percent × 255 / 99`,
with the input percentage clamped to `[0, 99]`. The init table's own default parameter is `0x50`
(register value 80). CONFIRMED.

**Text metrics.** Character-cell placement maps `(col, row)` to pixel coordinates as
`px = font_w × col`, `py = (row + 1) × font_h − 1` — i.e. the cursor's y coordinate is the *bottom*
row of the cell, not the top. The vendor's compiled-in font is 6×8, giving 40×10 character cells at
240×80. This project's own driver ships a minimal, independently authored 8×8 font (not derived
from or copied out of the vendor's font system) covering space, `A`–`Z`, `0`–`9`, and basic
punctuation; any other byte renders as a filled fallback glyph.

**Backlight.** GPIO 2, LEDC low-speed timer 0 / channel 0, 5 kHz, 13-bit duty resolution (max duty
8191), with `ledc_set_fade_with_time` fades on every change. `duty = percent × 8191 / 100`. All
CONFIRMED.

---

## 10. What is still unresolved

* **The physical panel itself** — true size, dot pitch, any printed part number, front-light or
  touch layer — see §2. Nothing in this document depends on these being known.
* Whether 240×80 is the panel's native resolution or a window programmed into larger glass (§2).
* The meaning of opcodes `0xC9`+parameter, `0xC8`+parameter, and `0x95` (§4.3) — replayed verbatim,
  not decoded.
* The exact meaning of `0xE1` in the reset preamble (§4.1) — almost certainly harmless (whatever
  RAM location it lands in as a data byte is immediately overwritten by the init table's own RAM
  address-control command a few bytes later), but not itself confirmed to be inert.
* The window-program trailer byte `0x01`'s specific meaning (§7).
* Which of the connector's 14 contacts carries which signal — no continuity trace has been made
  from the connector to either processor (§2).

---

## 11. Reproducing this analysis

Everything above was derived by disassembling the vendor's own official firmware release, obtained
directly from the maker, and cross-checking the result against the publicly available UltraChip
UC1611 datasheet. None of that vendor firmware is redistributed in this repository — see
[LEGAL.md](../LEGAL.md). To regenerate the raw evidence independently:

1. Obtain the current stock firmware update package from the maker's own distribution channel (see
   [external-references.md](external-references.md)) and extract the S3 application image from it.
2. Run a static image inspector (for example `esptool image_info`) to confirm the image header,
   segment layout, and checksums match the figures in [flash-layout-and-updater.md](flash-layout-and-updater.md).
3. Disassemble the extracted image with a standard Xtensa-aware disassembler and locate the display
   component by its own internal log tag; the reset preamble, the 21-entry table, and the
   display-enable pair are a short, contiguous run of `i2c_master_transmit()` call sites.
4. Cross-check each opcode against the UltraChip UC1611 datasheet's own command table (§4.2/§6) —
   any UC1611 or UC1611s datasheet revision that documents the I²C mode would additionally resolve
   the three still-unplaced opcodes in §4.3.

This is offered as a reproducibility path, not a claim that the exact file offsets cited during that
work are portable across every vendor firmware release — a different release may relocate the same
routines.
