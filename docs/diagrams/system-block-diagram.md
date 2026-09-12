# BYOK v2.1 system block diagram

This is the whole-board wiring picture: the ESP32-S3 as the hub, the ESP32-PICO-MINI-02 as its
Bluetooth-keyboard co-processor, the seven-wire link between them, and every peripheral each one
drives — display, sensor bus, USB-C, microSD, buttons, status LED, console, power/charge path, and
the battery. See `system-block-diagram.svg` for the full drawing. Every block and edge in it carries
a confidence suffix; anything short of CONFIRMED is written out in words on the diagram itself
("strongly indicated", "unverified", "unknown") rather than implied by color alone, and the legend
maps those same three grades onto line style (solid / dashed / dotted). No part number appears on
the drawing unless it was actually read off a package marking or a firmware string — candidate
identifications (the charger IC, the LCD controller) are named only as "-class" or "candidate" and
graded accordingly.

Two negatives are drawn as explicitly as the positives: there is no IO0/BOOT strap between the two
MCUs (the PICO firmware update rides the same UART as an application-level protocol), and there is
no display reset GPIO anywhere (bring-up is I2C-only). A third negative worth calling out even
though it isn't its own box: there is no external USB data mux — the TUSB320 only ever reports
CC/attach state over I2C, and the switch between the S3's own USB-Serial-JTAG and USB-OTG
controllers happens inside the S3 on the same D+/D− pads.

## Facts table

| Fact | Confidence | Source doc |
|---|---|---|
| ESP32-S3-WROOM-1, marking `MCN16R2`; 16 MB flash, 2 MB PSRAM; dual Xtensa LX7 @ 160 MHz; Wi-Fi + BLE + native USB-OTG | CONFIRMED — silkscreen marking; flash from `esptool` JEDEC id `46 40 18`; PSRAM and 160 MHz from the device's own boot log | docs/architecture.md (§1 table, boot-log excerpt); docs/hardware.md (module inventory, boot-log finding) |
| ESP32-PICO-MINI-02, marking `MGN8R2`; dual Xtensa LX6; Bluetooth Classic + BLE (the only part on the board with BT Classic) | module marking CONFIRMED; 8 MB flash / 2 MB PSRAM is suffix-decode only (unverified) | docs/architecture.md §1; docs/hardware.md (module inventory) |
| PICO's role as the Bluetooth-keyboard bridge | STRONGLY INDICATED | docs/architecture.md §4 responsibility matrix |
| Inter-MCU link — S3 GPIO36 TX → PICO RX, UART1, 921600 8N1, no flow control | CONFIRMED | firmware/common/hw_config.h §5; docs/pinout.md §1.1 |
| S3 GPIO37 ← PICO TX (same UART) | CONFIRMED | firmware/common/hw_config.h §5; docs/pinout.md §1.1 |
| S3 GPIO17 → PICO RESET, idle LOW, 10 ms active-HIGH pulse | CONFIRMED | firmware/common/hw_config.h §5; docs/pinout.md §1.1 |
| S3 GPIO35 → PICO REQ (the PICO's own "READY_IN") | STRONGLY INDICATED | firmware/common/hw_config.h §5; docs/pinout.md §1.1 |
| S3 GPIO47 → PICO STROBE, asserted around a framed send | STRONGLY INDICATED | firmware/common/hw_config.h §5; docs/pinout.md §1.1 |
| S3 GPIO46 ← PICO READY, HIGH = ready to receive, any-edge + ISR | CONFIRMED | firmware/common/hw_config.h §5; docs/pinout.md §1.1 |
| S3 GPIO48 ← unknown line, any-edge input, ISR posts values nothing in the image consumes | wiring CONFIRMED, meaning unverified | firmware/common/hw_config.h §5; docs/pinout.md §1.1 |
| No IO0/BOOT strap between the two MCUs; PICO firmware update is an application-level protocol over the same UART | CONFIRMED negative | firmware/common/hw_config.h §5; docs/pinout.md §1.1 |
| Display I2C — port 0, SDA GPIO18 / SCL GPIO10, 1 MHz, external pull-ups required, ACK-check disabled | CONFIRMED | firmware/common/hw_config.h §1 |
| LCD controller, UC1611/UC1611s command class; command device `0x38` / data device `0x39` (7-bit); 240×80, 1 bpp, 2400-byte RAM; one I2C transaction per byte, no bulk write in the stock image | UC1611/UC1611s class STRONGLY INDICATED (component name `gclcd1611` + opcode map match); addresses, geometry, RAM size and per-byte transfer CONFIRMED | firmware/common/hw_config.h §1, §3 |
| J5 — 14-contact FPC/ZIF, ~1.0 mm pitch | STRONGLY INDICATED (directly counted and measured against two independent scale references) | docs/hardware.md §5.3 |
| FSTN panel, 240×80 | panel technology as photographed: unverified | (owner/board observation only — outside the cited firmware/hardware sources) |
| No display reset GPIO anywhere; bring-up is I2C-only | CONFIRMED negative | firmware/common/hw_config.h §3 (reset preamble is I2C commands, no GPIO) |
| Backlight — S3 GPIO2, LEDC low-speed timer 0 / channel 0, 5 kHz, 13-bit (duty max 8191) | CONFIRMED | firmware/common/hw_config.h §4 |
| Sensor I2C — port 1, SDA GPIO8 / SCL GPIO9, 400 kHz, external pull-ups | CONFIRMED | firmware/common/hw_config.h §2 |
| TUSB320 USB-C CC/port-role controller, addr `0x60`; only status register `0x09` is ever read, bit `0x10` written back to clear the interrupt; `MODE_SELECT` (`0x0A`) never written — CC role is strap-fixed; used only as an attach/charger sensor | CONFIRMED | firmware/common/hw_config.h §2 |
| S3 GPIO41 ← TUSB320 INT, input, no pull, NEGEDGE + ISR | CONFIRMED | firmware/common/hw_config.h §7; docs/pinout.md §1.1 |
| PCF8563 real-time clock (NXP), addr `0x51` | part CONFIRMED — package marking read directly | firmware/common/hw_config.h §2; docs/hardware.md (module inventory, `IC8` row) |
| Y1 32.768 kHz crystal as the PCF8563's timebase | STRONGLY INDICATED (only crystal on the board, adjacent to the part) | docs/hardware.md (module inventory, `Y1` row) |
| No external USB data mux; TUSB320 only reports CC/attach state over I2C; USB-Serial-JTAG ↔ USB-OTG switch is internal to the S3 on the same D+/D− pads | CONFIRMED (TUSB320 register scope) / general ESP32-S3 SoC architecture for the internal switch | firmware/common/hw_config.h §2, §7 |
| USB-C receptacle J1 — 16 contacts countable; 16-pin vs 24-pin not settled | CONFIRMED (receptacle); pin count unverified | docs/hardware.md §5.4 |
| D−/D+ on S3 GPIO19/GPIO20, native pads, never `gpio_config`'d | CONFIRMED | firmware/common/hw_config.h §7; docs/pinout.md §1.1 |
| J1→S3 PCB pair assignment | unverified — no trace followed | docs/hardware.md §5.4; docs/architecture.md §2 |
| R56/R57 + C43 near the CC contacts may be the 5.1 kΩ Rd sink pull-downs | possible | docs/hardware.md §5.4 |
| VBUS sense GPIO: none. USB power-switch GPIO: none | CONFIRMED negatives | firmware/common/hw_config.h §7 |
| microSD J3 — SDMMC slot 1, 1-bit: CLK GPIO5, CMD GPIO3, D0 GPIO4, 40 MHz max, mounted at `/SDCARD` | CONFIRMED | firmware/common/hw_config.h §6 |
| Card detect GPIO40, input + internal pull-up, LOW = card present; driver `cd`/`wp` both left at −1 (detect is polled separately, not wired into the SDMMC driver) | CONFIRMED | firmware/common/hw_config.h §6 |
| Five buttons, active-LOW, no internal pulls (board must supply external pull-ups), polled: WAKE GPIO6, DOWN GPIO7, BRIGHTNESS GPIO11, UP GPIO15, EXECUTE GPIO16 | pins CONFIRMED | firmware/common/hw_config.h §8; docs/pinout.md §1.1 |
| Front power button = WAKE | STRONGLY INDICATED | docs/architecture.md §4 responsibility matrix |
| Mapping of the four back switches to DOWN/BRIGHTNESS/UP/EXECUTE | not individually mapped — unverified | docs/architecture.md §4 responsibility matrix |
| Status LED — S3 GPIO14, single WS2812, RMT channel 0, `clk_div = 2` | CONFIRMED | firmware/common/hw_config.h §10 |
| LED1 — 4-terminal part | part's existence STRONGLY INDICATED; that it is (vs. a connector) an LED: possible | docs/hardware.md (module inventory, `LED1` row) |
| Console — S3 GPIO43/GPIO44, UART0, IDF default pins, never re-pinned; secondary console on USB-Serial-JTAG | STRONGLY INDICATED | firmware/common/hw_config.h (boot-order note); docs/pinout.md §1.1 |
| Power latch — S3 GPIO42, OUTPUT, driven LOW at boot = power held ON; driving it HIGH cuts power | CONFIRMED | firmware/common/hw_config.h §9; docs/pinout.md §1.1 |
| Battery sense — resistive divider 5/3 (V_pin = 0.6 × V_batt) → ADC1_CH0 = GPIO1, 12-bit, 12 dB atten | CONFIRMED from firmware; the divider ratio itself has not been bench-measured | firmware/common/hw_config.h §9 |
| Charger status — GPIO12 = CHRG, GPIO13 = STDBY, both input + internal pull-up, polled, active LOW; 12=0,13=1 → charging; 12=1,13=0 → charge complete; 12=1,13=1 → no charger | pin→state mapping CONFIRMED (three independent open-codings in the image) | firmware/common/hw_config.h §9 |
| Driving IC is "TP4056-class" | STRONGLY INDICATED — position/behavior only, no part number printed on the block | firmware/common/hw_config.h §9 (comment) |
| Battery — Li-polymer pouch cell, 3.7 V / 2600 mAh / 9.62 Wh, model `357090` | CONFIRMED — read directly from the cell label | docs/hardware.md §6.1 |
| J2 (VBAT) — 2-pin red/black connector | CONFIRMED | docs/hardware.md §5.6 |
| Power/charge chain (adjacency only): J2 → Q5/Q8 + passives → IC5 → D6 → L1 (1R0) → IC6 → IC7 → L2 (1R0) → 3V3 | chain order CONFIRMED by adjacency; no trace followed and no measurement taken | docs/hardware.md §6.2 |
| IC5 top mark `AAAC` / `2118`; charge/power-path candidate by position only | marking CONFIRMED; part and function unread — unknown | docs/hardware.md (module inventory, `IC5` row) |
| L1 = 1R0 | STRONGLY INDICATED (marking read, matches L2's confirmed body/mark) | docs/hardware.md (module inventory, `L1` row) |
| L2 = 1R0 | CONFIRMED (marking) | docs/hardware.md (module inventory, `L2` row) |
| IC6, IC7 — SOT-23-6, unmarked | package/populated CONFIRMED; marking and part unknown | docs/hardware.md (module inventory, `IC6`/`IC7` rows) |
| Test points VBUS, CHRG, SW, 3V3, 5V, GND | CONFIRMED — all six read directly, gold-plated through-holes | docs/hardware.md §5.7 |
| Two shielded switching inductors ⇒ at least two switching stages | STRONGLY INDICATED | docs/hardware.md §6.2 |
| No PD controller, fuse, PTC, or fuel gauge identified | UNKNOWN — none identified | docs/hardware.md §6.2 |
| 3V3 rail feeds every logic block | STRONGLY INDICATED | docs/architecture.md §2 block diagram |
| 5V rail exists as a labelled rail; boost output vs. VBUS pass-through | UNKNOWN | docs/hardware.md §6.2 |
| GPIO39 — output, driven LOW once at boot, never written again anywhere in either image | wiring CONFIRMED; purpose unknown | firmware/common/hw_config.h §5 (GPIO39 guard); docs/pinout.md §1.1 |
| J4 — a second 2-pin red/black connector, not labelled VBAT | CONFIRMED (existence); purpose unknown — do not assume it is a battery | docs/hardware.md §5.7 |
| IC1, IC2, IC4, IC9, IC10 — unmarked or unread | UNKNOWN | docs/hardware.md (module inventory) |
| IC3 top mark `TPS` / `25910` / `TI 85I` / `C400` | marking CONFIRMED (all four lines); device and function unknown | docs/hardware.md (module inventory, `IC3` row) |
