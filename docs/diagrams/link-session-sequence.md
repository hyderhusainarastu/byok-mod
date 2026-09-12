# Link session: handshake, frame streaming, acknowledgement

This diagram traces one link session end to end, in the order the protocol itself lays it out — handshake, drawing into the back buffer, full-frame streaming with its CRC-checked close, the sequence-gap edge case that trips up most host implementations, and the unsolicited event stream — closing with the acknowledgement, sequencing, and timeout rules that apply throughout. Every message name, byte count, field, flag bit, and numeric limit below is taken directly from the link protocol specification in [`../protocol.md`](../protocol.md), which is the authoritative source for anything this diagram compresses. The handshake, drawing and framebuffer paths, and most of the message set are implemented in both the firmware and the host library; a few later additions are device-side only, and `../protocol.md` §13 says which shipped when.

```mermaid
sequenceDiagram
    autonumber
    participant Host
    participant Device

    Note over Host,Device: Phase 1 - Handshake

    Host->>Device: HELLO (0x01, ACK_REQ) - 8B: proto_ver_min=1, proto_ver_max=1, host_max_payload up to 4096, host_caps
    Device-->>Host: HELLO_ACK (0x02, IS_REPLY, SEQ echoed) - 32B
    Note over Device: fw_major/fw_minor/fw_patch; display_w, display_h; native and max bpp; dev_max_payload; capability bitmap; boot_slot - 0 factory, 1 ota_0, 2 ota_1, 0xFF unknown; font count; uptime_ms; an 8-byte device_id derived from a SHA-256 of the eFuse MAC, not the MAC itself
    Note over Host: The host MUST take panel geometry from HELLO_ACK and never hard-code it; it must check capability bit 1 before sending 2 bpp and bit 8 before offering "return to original"

    Host->>Device: GET_STATUS (0x05)
    Device-->>Host: STATUS (0x06) - 20B: battery_mv, battery_pct, charging - 0/1/2/0xFF, mode, backlight, contrast, flags, uptime_ms, free_heap, min_free_heap
    Note over Device: STATUS flags - bit0 host connected, bit1 SD present, bit2 USB attached, bit3 display sleeping, bits4-6 selected preset index, bit7 preset menu open

    Note over Host,Device: Phase 2 - Drawing into the back buffer

    Host->>Device: CLEAR (0x10)
    Host->>Device: DRAW_TEXT (0x11, ACK_REQ)
    Device-->>Host: ACK (0x09, IS_REPLY, same SEQ)
    Note over Host,Device: Drawing mutates a back buffer only. Nothing reaches the panel until a refresh - the panel is I2C-bound, so the host must be able to compose several draws and pay for one bus transfer.

    Note over Host,Device: Phase 3 - Full-frame streaming

    Host->>Device: FRAME_BEGIN (0x14) - 8B: w, h, bpp, flags
    Device-->>Host: ACK (always, regardless of ACK_REQ)
    loop chunks
        Host->>Device: FRAME_DATA (0x15) - u32 offset + up to 4092 bytes
    end
    Note over Host,Device: Chunks are offset-addressed, may arrive in any order, may be retransmitted; an overlapping chunk overwrites. FRAME_DATA is normally unacknowledged - the host may set ACK_REQ on the last chunk of a burst to pace itself. A duplicate FRAME_DATA is never dropped as a duplicate.
    Host->>Device: FRAME_END (0x16) - u32 CRC32 over the whole uncompressed packed frame + u8 refresh - 0 none, 1 partial, 2 full, 3 device chooses
    alt reassembled CRC matches and the frame is complete
        Device-->>Host: ACK
    else CRC differs
        Device-->>Host: NACK code 0x0B E_FRAME_CRC
    else bytes missing
        Device-->>Host: NACK code 0x0C E_INCOMPLETE - detail = missing regions, saturating at 255
    end
    Note over Device: On ANY nack the frame is discarded and the panel is not updated - a half-written screen is worse than a stale one

    Note over Host,Device: Phase 4 - The sequence-gap case

    Host->>Device: DRAW_RECT (0x12, ACK_REQ, SEQ = n) - an earlier frame was lost in transit
    Device-->>Host: NACK code 0x09 E_SEQ_GAP, IS_REPLY, SEQ = n - detail = frames missed
    Device-->>Host: ACK, IS_REPLY, SEQ = n
    Note over Host: E_SEQ_GAP is INFORMATIONAL and carries the request's own SEQ, so it looks exactly like the real reply. The host must log it and keep waiting, within the same request's timeout budget, for the real ACK or typed reply that follows. Gaps are reported, not repaired - there is no retransmission machinery.

    Note over Host,Device: Phase 5 - Unsolicited events

    Device->>Host: EVT_BUTTON (0x30, EVENT) - 6B: button 1 up / 2 down / 3 execute / 4 brightness / 5 wake, state 0 released / 1 pressed / 2 auto-repeat / 3 long-press, u32 uptime at the edge
    Device->>Host: EVT_STATUS (0x31, EVENT) - same 20-byte layout as STATUS, rate-limited to 1 Hz
    Device->>Host: EVT_PRESET_CHANGED (0x33, EVENT) - 1B index
    Note over Device: Events use the device's own SEQ counter, are never acknowledged and are never retried. A host that misses one misses it.

    Note over Host,Device: Closing notes
    Note over Host,Device: Host and device each own an independent u16 SEQ counter; replies do not consume a counter value.
    Note over Host,Device: A receiver keeps the last 8 sequence numbers per peer and silently drops idempotent duplicates, re-sending the previous reply if an ACK was requested.
    Note over Host,Device: Frame-begin, frame-end, set-mode, reboot and return-to-original are ALWAYS acknowledged regardless of the ACK_REQ flag - they are state transitions and the host must know they took.
    Note over Host,Device: Timeouts - host waits 250 ms for an acknowledged frame (retry once with the same SEQ, then twice more before declaring the link dead); 1000 ms for the handshake reply; pings after 5 s idle. Device aborts an open frame transaction after 3000 ms, returns to its own idle screen after 30 s of no host traffic, and discards a partial frame after a 200 ms mid-frame stall without sending a nack.
```

## Facts table

| Fact | Confidence | Source |
|---|---|---|
| Message set and IDs: HELLO 0x01, HELLO_ACK 0x02, GET_STATUS 0x05, STATUS 0x06, ACK 0x09, NACK 0x0A, CLEAR 0x10, DRAW_TEXT 0x11, DRAW_RECT 0x12, FRAME_BEGIN 0x14, FRAME_DATA 0x15, FRAME_END 0x16, EVT_BUTTON 0x30, EVT_STATUS 0x31, EVT_PRESET_CHANGED 0x33 | CONFIRMED (specified) | protocol.md |
| HELLO is 8 B: proto_ver_min, proto_ver_max, host_max_payload (u16, up to 4096), host_caps (u32) | CONFIRMED (specified) | protocol.md |
| HELLO_ACK is 32 B and includes proto_ver, fw_major/minor/patch, display_w/h, display_bpp_native/max, dev_max_payload, caps bitmap, boot_slot, is_our_firmware, fonts, uptime_ms, device_id | CONFIRMED (specified) | protocol.md |
| device_id is 8 bytes, the first 8 bytes of SHA-256(eFuse MAC) — read-only, not the MAC itself | CONFIRMED (specified) | protocol.md |
| Host must take panel geometry from HELLO_ACK and never hard-code it | CONFIRMED (specified, explicit normative statement) | protocol.md |
| Capability bit 1 = 2 bpp accepted natively; bit 8 = BOOT_ORIGINAL / "return to original" available; host must check both before using the corresponding feature | CONFIRMED (specified) | protocol.md |
| STATUS is 20 B: battery_mv, battery_pct, charging (0/1/2/0xFF), mode, backlight, contrast, flags, uptime_ms, free_heap, min_free_heap; identical layout reused by EVT_STATUS | CONFIRMED (specified) | protocol.md |
| STATUS.flags bit layout: bit0 host-connected, bit1 SD present, bit2 USB attached, bit3 display sleeping, bits4-6 selected preset index (0-7), bit7 preset menu open | CONFIRMED (specified) | protocol.md |
| Drawing commands mutate a back buffer only; nothing reaches the panel until a refresh, because the panel is I2C-bound (design decision D-013) | CONFIRMED (specified) | protocol.md |
| DRAW_TEXT / CLEAR reply with ACK if ACK_REQ was set; ACK echoes the request's SEQ | CONFIRMED (specified) | protocol.md |
| FRAME_BEGIN is 8 B (w, h, bpp, flags) and opens a frame transaction; only one may be open at a time; it is always acknowledged regardless of ACK_REQ | CONFIRMED (specified) | protocol.md |
| FRAME_DATA carries a u32 offset plus up to 4092 bytes; chunks are offset-addressed, may arrive out of order or be retransmitted, and an overlapping chunk overwrites; normally unacknowledged, host may set ACK_REQ on the last chunk of a burst | CONFIRMED (specified) | protocol.md |
| A duplicate FRAME_DATA chunk is not dropped by the duplicate-sequence filter, because chunk retransmission is legitimate | CONFIRMED (specified) | protocol.md |
| FRAME_END is 5 B: u32 CRC32 over the entire uncompressed packed frame + u8 refresh (0 none, 1 partial, 2 full, 3 device chooses); always acknowledged | CONFIRMED (specified) | protocol.md |
| FRAME_END outcome: CRC match + complete frame -> ACK; CRC mismatch -> NACK 0x0B E_FRAME_CRC; missing bytes -> NACK 0x0C E_INCOMPLETE with detail = missing 4 KiB-aligned regions, saturating at 255 | CONFIRMED (specified) | protocol.md |
| On any NACK to FRAME_END the frame is discarded and the panel is not updated | CONFIRMED (specified) | protocol.md |
| NACK code 0x09 = E_SEQ_GAP, informational, detail = number of missing frames (saturating at 255); the receiver still processes the frame that triggered it normally afterward | CONFIRMED (specified) | protocol.md |
| E_SEQ_GAP echoes the triggering request's own SEQ, so it is shaped identically to that request's real reply; a host must not treat it as the reply and must keep waiting, within the same timeout budget, for the actual ACK / typed reply | CONFIRMED (specified; explicit host-behavior correction dated 2026-09-04) | protocol.md |
| There is no retransmission machinery in v1 — sequence gaps are reported and logged, never repaired by the protocol itself | CONFIRMED (specified) | protocol.md |
| EVT_BUTTON is 6 B: button (1 up, 2 down, 3 execute, 4 brightness, 5 wake), state (0 released, 1 pressed, 2 auto-repeat, 3 long-press >= 1 s), u32 uptime at the edge | CONFIRMED (specified) | protocol.md |
| EVT_STATUS reuses the 20-byte STATUS layout and is rate-limited to at most 1 Hz | CONFIRMED (specified) | protocol.md |
| EVT_PRESET_CHANGED is 1 B (index), fired once per completed menu selection, never on a selection timeout | CONFIRMED (specified) | protocol.md |
| Events use the device's own SEQ counter, are never acknowledged, and are never retried | CONFIRMED (specified) | protocol.md |
| Host and device each keep an independent u16 SEQ counter; replies do not consume a counter value | CONFIRMED (specified) | protocol.md |
| A receiver keeps the last 8 sequence numbers per peer and silently drops idempotent duplicates, re-sending the previous reply if ACK_REQ was set | CONFIRMED (specified) | protocol.md |
| FRAME_BEGIN, FRAME_END, SET_MODE, REBOOT and BOOT_ORIGINAL are always acknowledged regardless of ACK_REQ, because they are state transitions | CONFIRMED (specified) | protocol.md |
| Timeouts: host waits 250 ms for an ACK_REQ reply (retries per the timeout table before declaring the link dead), 1000 ms for the HELLO_ACK handshake reply, and pings after 5 s of link idle; device aborts an open frame transaction after 3000 ms, leaves host mode after 30 s of no host traffic, and silently discards a partial frame after a 200 ms mid-frame stall with no NACK | CONFIRMED (specified) | protocol.md |
| Protocol status: implemented — the frame format, handshake, and drawing/framebuffer paths are live in both the firmware and the host library; a few later message-set additions are device-side only | CONFIRMED (stated by the protocol spec itself) | protocol.md §13 |
