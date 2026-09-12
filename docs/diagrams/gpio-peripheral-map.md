# ESP32-S3 pin assignments, recovered from the stock firmware image

This is a per-pin map of every GPIO the stock firmware touches, colour-banded by the subsystem each pin belongs to (display bus, sensor bus, SD, buttons, inter-MCU link, power, USB, console, or unknown), with each row carrying the confidence grade behind its claim — see `gpio-peripheral-map.svg` for the full table plus three supporting panels: a closure statement showing the pin lists are exhaustive (not a partial survey), a side-by-side comparison of the two I²C buses, and the exact order the firmware drives its earliest boot-time GPIO block in (which is not numeric pin order).

Every value on the diagram is what the firmware's own `gpio_config()` / `gpio_set_level()` / `gpio_get_level()` calls and peripheral-driver setup write into hardware registers, recovered by disassembly — not a measurement taken on the board. GPIO 42 (power latch — driving it HIGH cuts power) is called out with a red band and a hazard glyph because it is the one pin in this table that cuts power the instant it is driven the wrong way. Two pins (39 and 48) are wired with total confidence but their purpose is not: nothing in the stock image ever reads GPIO 39 again after boot, and nothing consumes the interrupt GPIO 48 raises.

## Facts table

| Fact | Confidence | Source doc |
|---|---|---|
| GPIO 1: ADC1_CH0 (never `gpio_config`'d), battery voltage sense, 12-bit, `ADC_ATTEN_DB_12` | CONFIRMED | firmware/common/hw_config.h — battery ADC section; docs/pinout.md §1.1 |
| GPIO 2: LEDC low-speed timer 0 / channel 0, LCD backlight PWM, 5 kHz, 13-bit duty resolution | CONFIRMED | firmware/common/hw_config.h — backlight (LEDC) section |
| GPIO 3/4/5: SDMMC slot 1 — CMD / D0 / CLK | CONFIRMED | firmware/common/hw_config.h — SD card section; docs/pinout.md §1.1 |
| GPIO 6: input, no pull, polled — button WAKE (front power) | CONFIRMED (pin); part-level name from docs/pinout.md | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 7: input, no pull, polled — button DOWN; held at boot enters the updater escape hatch | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 8/9: I2C port 1 (SDA/SCL) — TUSB320 (`0x60`) + PCF8563 (`0x51`), 400 kHz, XTAL clock, glitch-ignore 7, internal pull-up OFF | CONFIRMED | firmware/common/hw_config.h:141-188 |
| GPIO 10/18: I2C port 0 (SCL/SDA) — display only, 1 MHz, XTAL clock, glitch-ignore 7, internal pull-up left ON in this header (vendor runs the bus with it OFF — external pull-ups required at 1 MHz) | CONFIRMED | firmware/common/hw_config.h:61-139 |
| GPIO 11: input, no pull, polled — button BRIGHTNESS | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 12/13: input, internal pull-up, polled — two charger status lines, active LOW | CONFIRMED (pin→state, three independent open-codings in the image); part-level naming (CHRG/STDBY, TP4056-class driver) STRONGLY INDICATED, not a schematic or die-marking read | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 14: RMT channel 0, WS2812 data, 1 LED, `clk_div = 2` (80 MHz APB / 2) | CONFIRMED | firmware/common/hw_config.h — WS2812 section |
| GPIO 15: input, no pull, polled — button UP; held at boot enters USB console mode | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 16: input, no pull, polled — button EXECUTE | CONFIRMED (pin) | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 17: OUTPUT, driven LOW at boot — PICO reset, 10 ms active-HIGH pulse | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 and §5 |
| GPIO 19/20: never `gpio_config`'d — native USB D-/D+ | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 35: OUTPUT, driven LOW at boot — PICO request / ready-out line | STRONGLY INDICATED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 and §5 |
| GPIO 36/37: UART1 TX/RX to/from the PICO, 921600 8N1 | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 39: OUTPUT, driven LOW once at boot, never written again anywhere in either firmware image — purpose UNKNOWN | UNKNOWN / unverified (CONFIRMED negative that it is never touched again) | firmware/common/hw_config.h:886-938, §13 guard; docs/pinout.md §1.1, §5 and §6 |
| GPIO 40: input, internal pull-up, polled — SD card detect, LOW = card present | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 41: input, no pull, NEGEDGE + ISR — TUSB320 interrupt | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 42: OUTPUT, driven LOW at boot — power latch; driving it HIGH cuts power | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 (safety note) |
| GPIO 43/44: UART0 TX/RX, IDF default pins, never re-pinned — console / boot log | STRONGLY INDICATED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 46: input, pull-down, ANYEDGE + ISR — PICO READY, HIGH = ready | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| GPIO 47: OUTPUT, driven LOW at boot — PICO transfer strobe | STRONGLY INDICATED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 and §5 |
| GPIO 48: input, pull-down, ANYEDGE + ISR — a second status line whose events nothing in the image consumes | wiring CONFIRMED; meaning unverified | firmware/common/hw_config.h:886-938, §13 guard; docs/pinout.md §1.1 and §6 |
| Pin-name closure: `gpio_config()` names exactly `{6,7,11,12,13,15,16,17,35,39,40,41,42,46,47,48}` across six call sites; `gpio_set_level()` names exactly `{17,35,39,42,47}`; `gpio_get_level()` names exactly `{6,12,13,16,40}` plus the ISR's own argument; every other listed pin belongs to a peripheral driver, which is why it never appears in a `gpio_config()` | CONFIRMED | firmware/common/hw_config.h:886-938; docs/pinout.md §1.1 |
| I2C bus 0 (display): port 0, SDA 18 / SCL 10, 1 MHz, XTAL clock source, glitch-ignore 7, `disable_ack_check` set on both devices (`0x38` command, `0x39` data), one byte per I2C transaction, 1000 ms timeout; vendor runs it with internal pull-up OFF, so external pull-ups on GPIO18/GPIO10 are required to meet the 1 MHz rise time | CONFIRMED | firmware/common/hw_config.h:61-139 |
| I2C bus 1 (sensors): port 1, SDA 8 / SCL 9, 400 kHz, XTAL clock source, glitch-ignore 7, internal pull-up OFF, devices `0x60` (TUSB320) and `0x51` (PCF8563) | CONFIRMED | firmware/common/hw_config.h:141-188 |
| Boot-order strip: a single `gpio_config()` covers pins 17, 35, 39, 42, 47 as OUTPUT with no pulls and no interrupt, immediately followed by five back-to-back `gpio_set_level(pin, 0)` calls with no delay between any of them, in the order 17 → 35 → 47 → 39 → 42 (not numeric pin order) | CONFIRMED | docs/pinout.md §5 items 1–2 |
| This five-pin GPIO block is the earliest hardware GPIO action of the entire boot — before the I2C buses, the RTC, the boot-mode fork, and display init — on every boot path without exception | CONFIRMED | docs/pinout.md §5 item 3 |
| Pin-to-pad mapping is out of scope: no pin in this table has been mapped to a J6/J7 spring-pin footprint pad, a test point, or a connector contact | not attempted (no evidence, by design) | docs/pinout.md §3 |
