# Hardware

This is a component-level catalogue of what is visible on the board's top side, plus everything
firmware disassembly and two read-only device sessions were able to add to it. It was built from
close-macro photographs of the opened case — no photographs are published in this repository, but
every finding below traces back to one and is graded by how legible the marking actually was, not
by what a comparable part "should" be. The solder side of the board has never been photographed;
treat every statement here as top-side-only unless it says otherwise.

Evidence labels follow [SAFETY.md](../SAFETY.md) §2: **CONFIRMED** / **STRONGLY INDICATED** /
**POSSIBLE** / **UNKNOWN**. A marking that was not clearly legible is recorded as UNKNOWN — nothing
here is inferred from what a reference design "should" have, and no part number appears that was
not actually read off a package or a firmware string. For the system-level picture this feeds into,
see [architecture.md](architecture.md) and the whole-board drawing in
[`diagrams/system-block-diagram.md`](diagrams/system-block-diagram.md); for the complete GPIO/I²C
pin map, see [pinout.md](pinout.md).

---

## 1. Board identity

| Item | Value | Confidence |
|---|---|---|
| Product silkscreen | `BYOK` (large wordmark, top edge) | CONFIRMED |
| Revision silkscreen | `v2.1` (top-left corner) | CONFIRMED |
| Finish | Matte black soldermask, white silkscreen, gold (ENIG-like) pad finish | CONFIRMED |
| Approximate board size | ~66 × 78 mm | POSSIBLE (single estimate against the ESP32-S3-WROOM-1 module's known 18.0 mm shield dimension; no ruler was ever in frame) |
| Side examined | Top / component side only | CONFIRMED |

---

## 2. The two MCU modules

### 2.1 ESP32-S3-WROOM-1

Bottom-right quadrant, board-upright, label printed 180° rotated relative to the `BYOK` wordmark.
No board-side reference designator was ever found for this module.

| Line (verbatim) | Confidence |
|---|---|
| `ESPRESSIF` + swirl logo | CONFIRMED |
| `ESP32-S3-WROOM-1` | CONFIRMED |
| `FCC ID:2AC7Z-ESPS3WROOM1` | CONFIRMED |
| `IC:21098-ESPS3WROOM1` | STRONGLY INDICATED (numeric block at the resolution limit) |
| `CMIIT ID: 2022DP2892` | CONFIRMED |
| `MCN16R2` (variant code) | CONFIRMED |

The trailing `N16R2` decodes, by Espressif's own module-naming convention, to 16 MB flash / 2 MB
PSRAM. That decode is now **CONFIRMED electrically**, not just read off the label: `esptool
flash_id` over USB-Serial-JTAG returned JEDEC manufacturer `0x46`, device `0x4018` (capacity byte
`0x18` = 16,777,216 bytes), and the device's own boot log reports `Found 2MB PSRAM device` and
passes its memory test.

**Package.** Metal-shielded module, ~18 × 25.5 mm, castellated pads on three sides; the fourth
short edge carries no pads and is the integrated PCB meander antenna, pointed at the board's bottom
edge over a component-free keep-out region silkscreened `X1`. The `-1` suffix (not `-1U`) is the
PCB-antenna variant — confirmed by the printed part number; a `-1U` module would carry a u.FL
connector, and none exists here.

### 2.2 ESP32-PICO-MINI-02

Top-left quadrant, board-upright. Board designator is `IC11` — STRONGLY INDICATED.

| Line (verbatim) | Confidence |
|---|---|
| `ESPRESSIF` + swirl logo | CONFIRMED |
| `ESP32-PICO-MINI-02` | CONFIRMED |
| `FCC ID:2AC7Z-ESPPICOMINI` | CONFIRMED |
| `IC:21098-ESPPICOMINI` | CONFIRMED |
| `CMIIT ID: 2021DP1369` | CONFIRMED |
| `MGN8R2` (variant code) | CONFIRMED as read |

The `N8R2` → 8 MB flash / 2 MB PSRAM decode is **POSSIBLE** only — this module has never been
electrically probed and has no known download-mode route.

> **Naming trap worth stating plainly.** The board silkscreens this module's domain as `PICO`
> (`J7 PICO`, `RESET PICO`). That refers to the Espressif **PICO-MINI** module family — it is an
> Espressif ESP32 system-in-package, **not** a Raspberry Pi Pico / RP2040. This is confirmed
> directly by the module's own printed part number, and it is worth stating because the silkscreen
> alone invites the opposite guess.

**Package.** Metal-shielded SiP, ~13 × 12–17 mm, castellated/LGA edge pads. The antenna is
integrated inside the module package under its shield can, so no board-level antenna trace exists
for this radio.

**Silicon implication.** The ESP32-PICO-MINI-02 contains an ESP32-PICO-V3: dual-core Xtensa LX6,
Wi-Fi 802.11 b/g/n **plus Bluetooth Classic and BLE**, and no native USB. The ESP32-S3 has Wi-Fi
and BLE only (no Bluetooth Classic) and does have native USB-OTG. This asymmetry is the single most
useful architectural fact this project has — see [architecture.md](architecture.md) §1.

---

## 3. Component inventory

Confidence applies to the identification, not merely to whether a designator exists. Where a
designator is legible but the part behind it is not, that is stated explicitly. This list is
necessarily incomplete for anything not resolvable from the top side — treat a blank identity as
"not yet read," never as "not present."

| Designator | Marking | Package | Function | Confidence |
|---|---|---|---|---|
| `IC3` | `TPS` / `25910` / `TI 85I` / `C400` | Wettable-flank QFN, ~4 mm sq. | A Texas Instruments part (the `TI` line is explicit). Position — between USB-C, the microSD socket, and pad group `J6` — fits a power-path switch or load switch, but the exact device was not matched to a TI catalogue part | CONFIRMED (marking) / UNKNOWN (exact part and function) |
| `IC5` | `AAAC` / `2118` | MSOP/TSSOP-10 (5 leads per side) | `AAAC` is a top-mark code that did not resolve to a known manufacturer; `2118` reads as a date code (2021, week 18). Position — beneath the `VBUS` test point, next to the battery connector and the first inductor — fits a battery charger or power-path IC | CONFIRMED (marking, package) / UNKNOWN (part) |
| `IC6` | not legible | SOT-23-6 | Power-block device, populated (solder fillets visible on all six leads) | CONFIRMED (populated) / UNKNOWN (part) |
| `IC7` | not legible | SOT-23-6 | Power-block device between the two inductors; package is consistent with a small synchronous buck controller | CONFIRMED (populated) / POSSIBLE (function) |
| `IC8` | `PCF8563` / NXP logo / `96 08` / `n6544` | SOIC-8, ~1.3 mm lead pitch | **NXP PCF8563 — I²C real-time clock.** Crystal `Y1` beside it is its 32.768 kHz timebase | **CONFIRMED** |
| `Y1` | not legible | 2-pad metal-lid SMD crystal, ~3.2 × 1.5 mm | Timebase for `IC8` — the PCF8563 requires an external 32.768 kHz crystal and this is the only crystal on the board | CONFIRMED (part class) / STRONGLY INDICATED (belongs to `IC8`) |
| `L1` | `1R0` | Shielded moulded SMD power inductor, ~4–5 mm sq. | 1.0 µH switching inductor; the `CHRG` and `SW` test points sit at its corner | STRONGLY INDICATED |
| `L2` | `1R0` | Same package as `L1` | 1.0 µH; second switching inductor, `3V3` test point immediately beside it | **CONFIRMED** |
| `R48` | `20R0` | Chip resistor, larger than its neighbours | 20.0 Ω; the large body plus an adjacent SOT-23 transistor (`Q10`) fits a current-sense or gate-drive role | CONFIRMED (marking) |
| `Q10` | `N61` (plus 1–2 unresolved glyphs) | SOT-23-3 | Transistor / MOSFET; the marking alone does not identify the exact part | STRONGLY INDICATED (marking) / UNKNOWN (part) |
| `D2` | not legible | SOT-23-6 / SC-70-6 | Position near the USB-C contact field is consistent with a USB ESD/TVS protection array; no trace was actually followed to a pad | POSSIBLE |
| `U7` | not legible top mark | 2-terminal chip SMD, beside switch `SW4` | Anomalous designator for this board's own scheme (ICs here are `IC1..IC11+`, diodes `D`, transistors `Q`) — flagged rather than explained | UNKNOWN |
| `LED1` | silkscreen `LED1` | White rectangular body, 4 terminals on one face | Status indicator LED — a 4-pin body suggests RGB or bicolour-with-common, though a 4-way connector reading is not fully excluded | STRONGLY INDICATED (part exists) / POSSIBLE (that it is an LED) |
| `IC1`, `IC2`, `IC4`, `IC9`, `IC10` | not legible | Small SOT/DFN packages | Unidentified | UNKNOWN |
| `D1`, `D3`, `D6`, `D8`–`D12` | not legible | SOD-123/SOD-323 | Diodes; `D6` sits directly above the `CHRG` test point in the charge path | UNKNOWN |
| `Q1`–`Q13` (except `Q10`) | not legible | SOT-23 / SOT-323 / SOT-363 | Discrete transistors/FETs, clustered around the battery/charge block and a top-centre strip that has the general motif of level-shifter pairs (unconfirmed) | UNKNOWN |
| `X1` | silkscreen only | No component present | Sits inside the ESP32-S3's antenna keep-out; no footprint exists beside it — do not assume a crystal is fitted here | UNKNOWN |

---

## 4. Switches

Six switches are present on the top side; only two carry a function silkscreen directly.

| Designator | Function | Type | Confidence |
|---|---|---|---|
| `S1` | `RESET S3` (resets the ESP32-S3) | Tact switch, ~4.5 mm, gold/brass plunger | CONFIRMED |
| `S3` | `RESET PICO` (resets the ESP32-PICO-MINI-02) | Same part as `S1` | CONFIRMED |
| `SW4` | `up` — GPIO15 | Tact switch, ~6 mm, ivory plunger | CONFIRMED |
| `SW3` | `execute` — GPIO16 | Same part | CONFIRMED |
| `SW5` | `down` — GPIO7 | Same part | CONFIRMED |
| `SW2` | `brightness` — GPIO11 | Same part | CONFIRMED |
| `SW1` | `wake` — GPIO6, the front power button | Front-panel button, not one of the four back switches | CONFIRMED |

> **Naming collision worth flagging.** The token `S3` appears on this board in three different
> senses: the ESP32-S3 chip itself, the function word beside pad group `J6`, and the reference
> designator of the switch that resets the **PICO**. The switch labelled `S3` does **not** reset
> the ESP32-S3 — that is `S1`. Don't conflate them when reading the silkscreen.

The button-to-GPIO mapping above is the final, firmware-confirmed state (see
[pinout.md](pinout.md) §4 for the full pin table and how it was recovered). No button on the top
side — labelled or not — is wired to `GPIO0`/`GPIO46`, the ESP32-S3's own strapping pins; the
five buttons the firmware polls are a completely separate set of GPIOs from the S3's boot-strap
pins.

---

## 5. Connectors and pad groups

Physical layout is described as photographed, with the `BYOK` wordmark upright.

### 5.1 `J1` — USB-C receptacle

Right board edge, mouth facing outward. A single vertical SMT contact column runs along its inboard
face at roughly 0.5 mm pitch; the exact contact count (16-pin vs. 24-pin) was never resolved —
POSSIBLE either way, the shell is blown out by glare in every available photograph. A matched
resistor pair sits just below-left of the contact field, in a position consistent with (but not
confirmed as) the CC1/CC2 sink pull-downs a USB-C port needs.

### 5.2 `J2` — battery connector (`VBAT`)

White 2-position wire-to-board header, left edge, lower third, silkscreened `J2` with a `VBAT`
label directly below it. Mated with a red/black pair running to the battery pouch cell. An adjacent
solder pad pair, marked `+`/`−`, sits to the connector's right; whether it is populated copper or
silkscreen-only was not determined.

### 5.3 `J3` — microSD card socket

Push-push metal-shell socket, ~14.5 × 15–16 mm (microSD, not full-size). Card mouth faces the same
board edge as the USB-C connector. A column of six series resistors plus one capacitor sits
immediately inboard of the contact row, consistent with conditioning the SD bus's four data/command
lines, though no trace was followed to confirm it. A 128 MB microSD card was reported inserted at
the time this project examined the unit — consistent with the ~125.8 MB Disk-Mode volume the stock
firmware exposes once filesystem overhead is accounted for.

### 5.4 `J2`/`J4` — a second, unexplained 2-pin connector

`J4` is physically identical to the battery connector `J2` — same white 2-position header, same
left edge, also fitted with a red/black wire pair, also carrying its own `+`/`−` solder-pad
markers — but the board does **not** silkscreen it `VBAT`. Two independent red/black wire pairs run
down the outside of the left edge at once. **Do not assume `J4` is a second battery connection** —
it could equally be a second power input, a speaker, a haptic motor, or a thermistor. Its function
is UNKNOWN.

### 5.5 `J5` — display FPC connector

Single-row, bottom-contact ZIF connector with a flip/slide actuator, mounted vertically along the
board's left edge, mated to an amber polyimide display flex. The contact row spans roughly
13.1–13.5 mm and carries **14 contacts at approximately 1.0 mm pitch** — counted directly, at high
native resolution, against two independent scale references in the same frame. No trace from this
connector to either MCU module has ever been followed; the S3 is assigned as its owner in
[architecture.md](architecture.md) on firmware evidence (the display driver and its I²C bus are
exclusively S3-side), not on a traced copper path.

### 5.6 `J6` / `J7` — MCU programming footprints

Each MCU has its own footprint beside its own reset switch: `J6` (labelled `S3`) beside `S1`, and
`J7` (labelled `PICO`) beside `S3`. Neither is a bare 2×3 pad grid — each is **six small bare-gold
signal pads plus three larger plated through-holes**, arranged as an orientation triangle (a
widely-spaced pair at one end, a single hole on the centreline at the other). Never soldered. No
per-pad label, no pin-1 marker, and no traceable copper exists on either group — the leg-hole
triangle is the only reliable orientation key. Full geometry, addressing convention, and what is
and is not known about individual pad functions are in [pinout.md](pinout.md) §3, since identifying
these pads is a pinout question, not a component-inventory one.

### 5.7 Test points

Six gold-plated through-holes, each with a boxed silkscreen label: `GND`, `VBUS`, `3V3`, `5V`,
`CHRG`, `SW`. All six labels are directly legible. `GND` is an open plated barrel (useful for a
hook clip or fine wire); `3V3`, `5V`, `CHRG` and `SW` are solder-filled and domed. There is no test
point named `BOOT`, `IO0`, `GPIO0`, `EN`, `DL`, `PROG` or `FLASH` anywhere on the board. See
[pinout.md](pinout.md) §2 for what each of these points is believed to carry and at what
confidence.

> **Naming trap:** the label `SW` on this board names the power converter's switching node — a
> high-dv/dt point between the two inductors — not a switch, and not anything boot-related.

---

## 6. Power, battery and charging

### 6.1 Battery

Label read directly off the pouch cell:

```
LITER ENERGY BATTERY
357090
+ 3.7V  9.62WH  2600MAH
```

| Property | Value | Confidence |
|---|---|---|
| Chemistry | Single-cell Li-polymer pouch, 3.7 V nominal | CONFIRMED |
| Capacity | 2600 mAh / 9.62 Wh | CONFIRMED |
| Model | `357090` (nominally 3.5 × 70 × 90 mm) | CONFIRMED (code) / STRONGLY INDICATED (dimension decode) |
| Connection | 2-wire red/black, to `J2` | CONFIRMED |
| Protection | A narrow bare-PCB strip with small SMD parts, taped along the cell's edge under Kapton — consistent with a protection circuit module | STRONGLY INDICATED |

### 6.2 Charging / conversion block

Everything below is inferred from silkscreen placement and adjacency; no trace was followed and no
measurement was taken. Ordered by physical adjacency only:

```
J2 (VBAT) -> IC5 (under the VBUS test point) -> D6 -> [CHRG]/[SW] test points
           -> L1 (1R0) -> IC6 -> IC7 -> L2 (1R0) -> [3V3] rail
```

Two shielded switching inductors (`L1`, `L2`) mean at least two switching stages exist — CONFIRMED
as parts, STRONGLY INDICATED as two converter stages. USB-C (`J1`) appears to be the only
identified external power input on the top side, though `J4`'s unresolved function (§5.4) keeps
this at STRONGLY INDICATED rather than CONFIRMED. A separate `5V` rail is brought out at the board
centre; whether it is a boost output or a straight VBUS pass-through is UNKNOWN. No fuse, PTC,
fuel-gauge IC, or USB-PD controller was identified anywhere on the board.

### 6.3 Charge behaviour observed

With the device connected over USB-C in its normal writing mode, a charge indicator lights while a
connected host observes **zero** USB enumeration and zero USB bus transactions — consistent with a
power-only or silently-idle link. Whether the connector actually carries a data-capable link that
simply presents nothing, or is genuinely power-only in this mode, cannot be distinguished from the
host side alone. UNKNOWN.

---

## 7. Antennas and RF

Two 2.4 GHz radios exist, both using module-integrated antennas, sitting at diagonally opposite
corners of the board (~36 mm apart) — the conventional layout for reducing coupling between two
co-located radios. The ESP32-S3-WROOM-1's PCB meander antenna points at the board's bottom edge,
over a component-free keep-out silkscreened `X1`; the ESP32-PICO-MINI-02's antenna is fully
integrated inside its module package, with no board-level structure. No u.FL/IPEX/MHF/SMA
connector, chip antenna, external antenna wire, NFC coil, pi-matching network, or RF shield can
other than the two module lids exists anywhere on the examined (top) side.

---

## 8. Open questions

These are the identifications this project was never able to close from the top side alone, listed
so a future contributor knows exactly what is still worth a better photograph or a solder-side
inspection:

- **`IC1`, `IC2`, `IC4`, `IC9`, `IC10`** — designators are legible; the parts behind them are not.
- **`IC3`'s exact TI part number**, and **`IC5`'s manufacturer** — top-mark codes were read but not
  matched to a datasheet.
- **`J4`'s function** — a second 2-pin connector, wired like a battery connector but not labelled
  as one.
- **Which of `J6`/`J7`'s six pads carries which signal** — the footprint's *purpose* (a factory
  programming jig, one per MCU) is well supported, but no individual pad has ever been identified.
  See [pinout.md](pinout.md) §3 for what would be needed to close this, and the safety rules around
  attempting it.
- **The USB-C receptacle's exact pin count** (16 vs. 24) — never resolved past POSSIBLE.
- **Whether the solder side carries anything** — it has never been photographed. A second flash IC,
  additional test points, or bottom-side pin labels for `J6`/`J7` could all exist there and would
  change some of the confidence grades above.
