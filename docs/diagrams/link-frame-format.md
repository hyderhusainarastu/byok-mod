# Host link frame format, byte for byte

Every frame on the host↔device link is a fixed 9-byte header (magic `BK`, version, type, flags, a little-endian sequence number, and a little-endian payload length) followed by a type-specific payload of up to 4096 bytes and a little-endian CRC-32 trailer computed over the header and payload together, making frames range from 13 to 4109 bytes; a self-synchronising HUNT/HEADER/PAYLOAD/VERIFY receiver state machine lets either side recover mid-stream without ever trusting frame boundaries in the raw byte stream, and the protocol itself has no primitive for memory, flash, filesystem, or code-execution access — see `link-frame-format.svg` for the full byte-layout diagram.

This page is a specification, not a hardware survey: every field, offset, size limit, flag meaning, and the worked `DRAW_TEXT` example are drawn verbatim from the protocol document, so no confidence suffixes are used anywhere in the diagram.

## Facts table

| Fact | Confidence | Source doc |
|---|---|---|
| Frame layout: MAGIC(2) VERSION(1) TYPE(1) FLAGS(1) SEQ(2 LE) LEN(2 LE) PAYLOAD(LEN) CRC32(4 LE) | CONFIRMED (spec) | docs/protocol.md — §3 Frame format |
| Header is 9 bytes; frame size = 9 + LEN + 4, so 13 B minimum / 4109 B maximum | CONFIRMED (spec) | docs/protocol.md — §3 Frame format |
| MAGIC = `0x42 0x4B` ("BK"), constant, resync anchor | CONFIRMED (spec) | docs/protocol.md — §3 field table |
| VERSION = `0x01`; unknown version → `NACK/E_BAD_VERSION`, frame discarded | CONFIRMED (spec) | docs/protocol.md — §3 field table |
| SEQ is little-endian u16, wraps 0xFFFF→0x0000, sender-scoped (host and device keep independent counters) | CONFIRMED (spec) | docs/protocol.md — §3 field table |
| LEN is little-endian u16, hard maximum 4096; over-limit → `NACK/E_BAD_LENGTH` and resync | CONFIRMED (spec) | docs/protocol.md — §3 field table |
| Payload: type-specific, all multi-byte integers little-endian, coordinates in pixels with origin top-left | CONFIRMED (spec) | docs/protocol.md — §3 field table |
| CRC32 = IEEE 802.3/zlib, reflected poly `0xEDB88320`, init `0xFFFFFFFF`, input/output reflected, final XOR `0xFFFFFFFF`, computed over the 9 header bytes + payload (magic/version inside CRC, CRC field itself not) | CONFIRMED (spec) | docs/protocol.md — §3.1 CRC32 |
| FLAGS bit meanings: bit0 ACK_REQ, bit1 IS_REPLY (echoes request's SEQ), bit2 MORE, bit3 RLE (legal only on FRAME_DATA/DRAW_BITMAP), bit4 EVENT (never acked/retried), bits5–7 reserved (send 0, ignore on receipt, never reject for a set reserved bit) | CONFIRMED (spec) | docs/protocol.md — §4 FLAGS |
| Worked example: 25-byte DRAW_TEXT frame for "BYOK" at (4,8), font 1, ACK_REQ, SEQ 5, raw hex and CRC32 `0x14C728E8`, with ACK reply `42 4B 01 09 02 05 00 00 00 <crc32>` | CONFIRMED (spec) | docs/protocol.md — §11 Worked example |
| Resync state machine: HUNT (scan for `BK`) → HEADER (read 7 bytes, reject unknown VERSION or LEN>4096) → PAYLOAD (read LEN + 4 CRC bytes under 200 ms stall timeout) → VERIFY (CRC mismatch rejects) → dispatch/reject → HUNT | CONFIRMED (spec) | docs/protocol.md — §7.6 timeouts table, §7.7 Resync |
| A literal `BK` inside a payload is harmless because HUNT is only entered outside a frame and frame extent is fixed by LEN | CONFIRMED (spec) | docs/protocol.md — §7.7 Resync |
| Size limits: max payload 4096 B, max frame on wire 4109 B, min frame 13 B, max DRAW_TEXT text 4088 B, max FRAME_DATA chunk 4092 B, max declared frame buffer 256 KiB, at most one concurrent frame transaction | CONFIRMED (spec) | docs/protocol.md — §8 Size limits |
| Primary transport is USB CDC-ACM on the native USB PHY; baud rate irrelevant; host must not assert DTR-toggle reset; USB-Serial-JTAG is debug console only, carries no protocol bytes | CONFIRMED (spec) | docs/protocol.md — §2 Transport |
| Protocol has no memory/flash/partition/NVS access, no register/I2C/GPIO poke, no code execution, no filesystem access, no message takes an address; worst case a hostile host can show the wrong thing, drain the battery via backlight, or reboot the device | CONFIRMED (spec) | docs/protocol.md — §1 scope note (read alongside §3–§9) |
