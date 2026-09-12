# Panel geometry, framebuffer packing and the refresh window

This diagram shows how the 240×80, 1-bit-per-pixel panel maps onto its 2400-byte framebuffer, how a dirty rectangle gets snapped to page boundaries before a partial refresh, and the full power-on bring-up sequence from bus creation through the first frame — including the one step (`CMD 0xC9` / `DATA 0xAD`) whose omission was the historical cause of a correctly-fed but dark panel, and two additive steps (a genuine system reset, and forcing the scroll-line register to zero) that are not part of the original vendor sequence. A transaction-cost table and the contrast/text-metric formulas are included for reference.

## Facts and confidence

| Fact | Confidence | Source |
|---|---|---|
| Panel is 240×80 px, 1 bpp | CONFIRMED | firmware/common/hw_config.h |
| Byte index = (y >> 3) × 240 + x | CONFIRMED | firmware/common/hw_config.h |
| Stride 240 bytes/page row, 10 pages, 2400 bytes total | CONFIRMED | firmware/common/hw_config.h |
| Bit order: LSB = topmost row of the page | CONFIRMED (upgraded from strongly indicated once glyphs rendered upright across page boundaries) | firmware/common/hw_config.h; docs/display.md |
| Framebuffer cleared to 0xFF at init; inverse display ON, so 0xFF is BLANK | CONFIRMED | firmware/common/hw_config.h |
| Dirty-rect y start snapped down to a page boundary (8-row granularity); x granularity is 1 column | CONFIRMED | docs/display.md |
| Step 1 — I2C bus + two device handles (cmd 0x38, data 0x39); no GPIO touched, no display reset line | CONFIRMED (negative) | firmware/common/hw_config.h |
| Step 3 — CMD 0xE1 / DATA 0xE2 reset preamble; whether 0xE1 is a double-byte opcode taking 0xE2 as parameter | UNVERIFIED | firmware/common/hw_config.h |
| 21-entry init table, replayed verbatim in order | CONFIRMED (bytes/order); several opcode meanings POSSIBLE (0xC9, 0x85, 0x95) | firmware/common/hw_config.h; docs/display.md |
| Step 8 — CMD 0xC9 / DATA 0xAD, display-enable step | CONFIRMED (bytes/position); meaning "display enable after RAM is valid" STRONGLY INDICATED | firmware/common/hw_config.h; docs/display.md |
| Additive step A — true system reset (CMD 0xE2, CD=0) ahead of preamble | Optional, additive (not part of stock sequence) | firmware/common/hw_config.h |
| Additive step B — force scroll line to zero (CMD 0x50 then CMD 0x40) | STRONGLY INDICATED as the fix for a 15-row vertical offset traced to an unprogrammed scroll-line register | docs/display.md |
| Window-program 8-step sequence (0x89→0xF8→0xF5→0xF7→0xF4→0xF6→0xF9→0x01) | CONFIRMED (0x01 trailer meaning unverified) | firmware/common/hw_config.h; docs/display.md |
| Transaction costs (1 / 2 / 12 / 2412 / 23) and measured ~91 µs/transaction | CONFIRMED | docs/display.md |
| Contrast: CMD 0x81 + DATA (percent × 255 / 99), clamp 0..99, default register 0x50 | CONFIRMED | firmware/common/hw_config.h; docs/display.md |
| Stock font 6×8 → 40×10 cells at 240×80; cursor y is bottom row of cell | CONFIRMED | docs/display.md |
| Controller family UC1611/UC1611s-class | STRONGLY INDICATED | firmware/common/hw_config.h |
| Panel technology, physical size, dot pitch | UNVERIFIED | docs/display.md |
