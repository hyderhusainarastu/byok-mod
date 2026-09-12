/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_proto.h — BYOK Link protocol v1: frame encoder + incremental decoder
 * ============================================================================
 *
 * Implements the wire format specified in full at docs/protocol.md (read
 * that document — this header follows it section by section and the section
 * numbers in these comments refer to it). This library is deliberately
 * framing-only:
 *
 *   - It builds and parses the 9-byte header + payload + 4-byte CRC frame
 *     (§3), including the exact CRC32 definition (§3.1).
 *   - The decoder is a byte-at-a-time state machine that never blocks, never
 *     allocates, and re-synchronises on the magic after any corruption (§7.7
 *     HUNT/HEADER/PAYLOAD/CRC).
 *   - It tracks each peer's SEQ stream just enough to report duplicates and
 *     gaps per §7.5 (window of the last 8 distinct SEQs seen, FRAME_DATA
 *     exempted from dedup).
 *
 * It deliberately does NOT know:
 *
 *   - What payload length is correct for a given TYPE. §9's E_BAD_LENGTH
 *     covers both "LEN out of range" (which this library rejects) and "LEN
 *     wrong for this type" (which is a dispatch-layer concern — the
 *     generic framing layer has no per-type schema table). A frame whose LEN
 *     is wrong for its TYPE but otherwise well-formed decodes successfully
 *     here; the CRC will not save you from a sender bug that computed a
 *     wrong LEN, because the sender computed its CRC over the same wrong
 *     bytes. Validate payload shape against TYPE at the call site.
 *   - Timers. The 250 ms / 1000 ms / 200 ms timeouts in §7.6 are the
 *     transport's job (the caller decides when "no bytes arrived" means
 *     "give up on this partial frame" and calls byok_parser_reset()).
 *   - Anything about what a TYPE means, how to build a reply, or ACK/NACK
 *     dispatch policy. That's the application/dispatch layer, one level up.
 *
 * Zero dynamic allocation: byok_parser_t embeds a BYOK_MAX_FRAME-byte
 * buffer and is meant to be allocated once (static, stack, or caller's
 * heap) and reused for the life of a connection.
 *
 * Builds standalone with plain clang (no ESP-IDF headers) and as an
 * idf_component_register()'d component — see CMakeLists.txt.
 */
#ifndef BYOK_PROTO_H
#define BYOK_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* §3 Frame format — sizes                                                 */
/* ---------------------------------------------------------------------- */

#define BYOK_MAGIC0        0x42u /* 'B' */
#define BYOK_MAGIC1        0x4Bu /* 'K' */
#define BYOK_VERSION       0x01u

#define BYOK_HEADER_LEN     9u   /* MAGIC(2) VERSION(1) TYPE(1) FLAGS(1) SEQ(2) LEN(2) */
#define BYOK_CRC_LEN         4u
#define BYOK_MAX_PAYLOAD  4096u
#define BYOK_MIN_FRAME      13u  /* 9 + 0    + 4 */
#define BYOK_MAX_FRAME    4109u  /* 9 + 4096 + 4 */

/* §8: hard cap on a declared framebuffer size (FRAME_BEGIN w*h*bpp/8). Not
 * enforced by this header/payload-agnostic library — listed here only
 * because callers implementing FRAME_BEGIN need the constant. */
#define BYOK_MAX_DECLARED_FRAME_BYTES (256u * 1024u)

/* ---------------------------------------------------------------------- */
/* §4 FLAGS                                                                */
/* ---------------------------------------------------------------------- */

#define BYOK_FLAG_ACK_REQ   0x01u
#define BYOK_FLAG_IS_REPLY  0x02u
#define BYOK_FLAG_MORE      0x04u
#define BYOK_FLAG_RLE       0x08u
#define BYOK_FLAG_EVENT     0x10u
#define BYOK_FLAG_RESERVED  0xE0u /* must be sent 0; ignored on receipt */

/* ---------------------------------------------------------------------- */
/* §5 Message types                                                       */
/* ---------------------------------------------------------------------- */

#define BYOK_TYPE_HELLO             0x01u
#define BYOK_TYPE_HELLO_ACK         0x02u
#define BYOK_TYPE_GET_INFO          0x03u
#define BYOK_TYPE_INFO              0x04u
#define BYOK_TYPE_GET_STATUS        0x05u
#define BYOK_TYPE_STATUS            0x06u
#define BYOK_TYPE_PING              0x07u
#define BYOK_TYPE_PONG              0x08u
#define BYOK_TYPE_ACK               0x09u
#define BYOK_TYPE_NACK              0x0Au

#define BYOK_TYPE_CLEAR             0x10u
#define BYOK_TYPE_DRAW_TEXT         0x11u
#define BYOK_TYPE_DRAW_RECT         0x12u
#define BYOK_TYPE_DRAW_BITMAP       0x13u
#define BYOK_TYPE_FRAME_BEGIN       0x14u
#define BYOK_TYPE_FRAME_DATA        0x15u
#define BYOK_TYPE_FRAME_END         0x16u
#define BYOK_TYPE_PARTIAL_REFRESH   0x17u
#define BYOK_TYPE_FULL_REFRESH      0x18u

#define BYOK_TYPE_SET_MODE          0x20u
#define BYOK_TYPE_SET_BACKLIGHT     0x21u
#define BYOK_TYPE_SET_CONTRAST      0x22u
#define BYOK_TYPE_GET_BATTERY       0x23u
#define BYOK_TYPE_BATTERY           0x24u
#define BYOK_TYPE_REBOOT            0x25u
#define BYOK_TYPE_BOOT_ORIGINAL     0x26u
/* v1.1 addition (docs/protocol.md Sec.6.4, Sec.13 "Version history"): sets
 * the device's PCF8563 RTC (hw_config.h Sec.2, byok_rtc component). Additive
 * within v1 per Sec.13's own compatibility rule -- a v1.0-only receiver
 * NACKs it E_UNKNOWN_TYPE, which is the whole point of that rule. Device-
 * side only as of this release: no host (host/macos/byok/proto.py) sends it
 * yet -- that file is maintained separately. */
#define BYOK_TYPE_SET_TIME          0x27u
/* v1.1 addition, byok-mod 0.1.13 (docs/protocol.md Sec.6.4, Sec.13 "Version
 * history"): sets the device's STATIC NOTE text (byok_modes BYOK_MODE_
 * STATIC_NOTE, byok_note component) -- payload is 0-200 raw UTF-8/ASCII
 * bytes, no fixed-field header (unlike DRAW_TEXT, there is no x/y/font_id:
 * the device owns the whole layout, word-wrapping the text itself). Additive
 * within v1 per Sec.13's own compatibility rule -- a v1.0-only (pre-SET_NOTE)
 * receiver NACKs it E_UNKNOWN_TYPE, which is the whole point of that rule.
 * Chosen from the same reserved-type convention SET_TIME (0x27, 0.1.12)
 * already used: the next free code immediately after the existing 0x20-0x27
 * "device control" block, keeping every SET_* type contiguous rather than
 * jumping to the 0x30+ EVT_* range. */
#define BYOK_TYPE_SET_NOTE          0x28u
/* v1.2 additions, byok-mod 0.1.14 (docs/protocol.md Sec.6.4b, Sec.13
 * "Version history"). Same reserved-type convention as SET_TIME/SET_NOTE
 * above: the next free codes immediately after the existing 0x20-0x28
 * "device control" block, keeping every SET_x / GET_x device-control type
 * contiguous. Additive within v1 per Sec.13's own compatibility rule. */
#define BYOK_TYPE_SET_PRESETS       0x29u /* H->D: up to 8 preset names, byok_menu component */
#define BYOK_TYPE_GET_DOCSTATS      0x2Au /* H->D, 0 B: request the SD-card writing-stats snapshot */
#define BYOK_TYPE_DOCSTATS          0x2Bu /* D->H, 20 B: GET_DOCSTATS reply, byok_docstats component */
#define BYOK_TYPE_DISPLAY_CFG       0x2Cu /* H->D, 1 B: runtime per-byte/bulk display write toggle */

#define BYOK_TYPE_EVT_BUTTON        0x30u
#define BYOK_TYPE_EVT_STATUS        0x31u
#define BYOK_TYPE_EVT_LOG           0x32u
/* v1.2 addition, byok-mod 0.1.14 (docs/protocol.md Sec.6.5): fired once on
 * every completed preset-menu selection (byok_menu component). */
#define BYOK_TYPE_EVT_PRESET_CHANGED 0x33u

/* EVT_BUTTON payload field values (docs/protocol.md Sec.6.5) -- specified
 * since v1.0/v1.1 but only actually emitted starting 0.1.14
 * (components/byok_idle). Kept here (not in byok_idle.h) because they are
 * wire-format constants, same as every other BYOK_TYPE_x / BYOK_E_x above. */
#define BYOK_BUTTON_UP         1u /* GPIO15 */
#define BYOK_BUTTON_DOWN       2u /* GPIO7  */
#define BYOK_BUTTON_EXECUTE    3u /* GPIO16 */
#define BYOK_BUTTON_BRIGHTNESS 4u /* GPIO11 */
#define BYOK_BUTTON_WAKE       5u /* GPIO6 -- not emitted by byok_idle (owned by byok_power) as of 0.1.14 */

#define BYOK_BUTTON_STATE_RELEASED   0u
#define BYOK_BUTTON_STATE_PRESSED    1u
#define BYOK_BUTTON_STATE_AUTOREPEAT 2u /* not emitted as of 0.1.14 -- see byok_idle.c */
#define BYOK_BUTTON_STATE_LONGPRESS  3u /* >= 1 s */

/* ---------------------------------------------------------------------- */
/* §9 Error codes (NACK.code)                                              */
/* ---------------------------------------------------------------------- */

#define BYOK_E_BAD_CRC        0x01u
#define BYOK_E_BAD_VERSION    0x02u
#define BYOK_E_UNKNOWN_TYPE   0x03u
#define BYOK_E_BAD_LENGTH     0x04u
#define BYOK_E_BAD_PARAM      0x05u
#define BYOK_E_OUT_OF_RANGE   0x06u
#define BYOK_E_BUSY           0x07u
#define BYOK_E_NO_MEM         0x08u
#define BYOK_E_SEQ_GAP        0x09u
#define BYOK_E_STATE          0x0Au
#define BYOK_E_FRAME_CRC      0x0Bu
#define BYOK_E_INCOMPLETE     0x0Cu
#define BYOK_E_UNSUPPORTED    0x0Du
#define BYOK_E_NOT_FOUND      0x0Eu
#define BYOK_E_NOT_PERMITTED  0x0Fu
#define BYOK_E_TIMEOUT        0x10u
#define BYOK_E_INTERNAL       0x11u

/* "no usable SEQ to echo" sentinel for NACK.seq_echo, per §6.1. */
#define BYOK_SEQ_ECHO_UNKNOWN 0xFFFFu

/* ---------------------------------------------------------------------- */
/* Encoder                                                                 */
/* ---------------------------------------------------------------------- */

/* Builds one complete wire frame into out_buf (caller-provided).
 *
 *   type         - §5 TYPE byte.
 *   flags        - §4 FLAGS bitmap. Caller must clear reserved bits 5-7.
 *   seq          - §3 SEQ (this frame's own sequence number, or the echoed
 *                  request SEQ when flags has IS_REPLY set — the encoder
 *                  does not care which, it just writes what it's given).
 *   payload      - payload bytes, or NULL if payload_len == 0.
 *   payload_len  - 0 .. BYOK_MAX_PAYLOAD.
 *   out_buf      - destination buffer, caller-owned.
 *   out_buf_len  - capacity of out_buf.
 *
 * Returns the number of bytes written (BYOK_HEADER_LEN + payload_len +
 * BYOK_CRC_LEN), or 0 on error: payload_len > BYOK_MAX_PAYLOAD, payload_len
 * != 0 with payload == NULL, or out_buf_len too small for the resulting
 * frame. Never partially writes out_buf on error.
 */
size_t byok_frame_encode(uint8_t type, uint8_t flags, uint16_t seq,
                          const uint8_t *payload, uint16_t payload_len,
                          uint8_t *out_buf, size_t out_buf_len);

/* Convenience: bytes a frame with this payload length will occupy on the
 * wire. Does not validate payload_len against BYOK_MAX_PAYLOAD. */
static inline size_t byok_frame_wire_size(uint16_t payload_len)
{
    return (size_t)BYOK_HEADER_LEN + payload_len + BYOK_CRC_LEN;
}

/* ---------------------------------------------------------------------- */
/* Decoder                                                                 */
/* ---------------------------------------------------------------------- */

/* A fully decoded, CRC-verified frame. payload points into the parser's own
 * internal buffer and is valid ONLY for the duration of the on_frame
 * callback that hands it to you — copy out anything you need to keep. */
typedef struct {
    uint8_t     version;
    uint8_t     type;
    uint8_t     flags;
    uint16_t    seq;
    uint16_t    len;
    const uint8_t *payload; /* len bytes, NULL if len == 0 */

    /* §7.5 sequence tracking, computed against this parser instance's own
     * per-peer window (one byok_parser_t per direction of traffic). */
    bool        is_duplicate; /* SEQ seen in the last 8 distinct SEQs from
                                * this peer, and type != FRAME_DATA (§7.5:
                                * "a duplicate FRAME_DATA is not dropped"). */
    uint8_t     seq_gap;      /* 0 = none; else frames missed before this
                                * one, saturating at 255 (§7.5, §9
                                * E_SEQ_GAP.detail). Always 0 when
                                * is_duplicate is true. */
} byok_frame_t;

/* Reasons the decoder rejects bytes and returns to HUNT (§7.7 step 2/4).
 * Maps directly to the NACK codes the caller should send, per §9. */
typedef enum {
    BYOK_ERR_BAD_VERSION = 1, /* VERSION field not BYOK_VERSION */
    BYOK_ERR_BAD_LENGTH,      /* declared LEN > BYOK_MAX_PAYLOAD */
    BYOK_ERR_BAD_CRC,         /* CRC32 mismatch after a full frame arrived */
} byok_parse_err_t;

/* Called once per successfully decoded, CRC-good frame. `frame->payload`
 * (and everything it points at) is only valid until this call returns. */
typedef void (*byok_frame_cb)(void *user_ctx, const byok_frame_t *frame);

/* Called once per rejected frame, i.e. once per resync event. `header`
 * points at whatever header bytes were captured before the reject was
 * detected (always >= 3 bytes: at least VERSION is known for
 * BAD_VERSION/BAD_LENGTH; always the full 9 for BAD_CRC) — enough to read
 * a best-effort SEQ for a NACK.seq_echo; it is only valid for the duration
 * of this call. `detail` mirrors the NACK.detail semantics of §9 for the
 * corresponding code (highest version supported / 0 / 0). */
typedef void (*byok_error_cb)(void *user_ctx, byok_parse_err_t err,
                               const uint8_t *header, size_t header_len);

typedef enum {
    BYOK_PARSE_HUNT = 0,
    BYOK_PARSE_HEADER,
    BYOK_PARSE_PAYLOAD,
    BYOK_PARSE_CRC,
} byok_parse_state_t;

typedef struct {
    byok_frame_cb   on_frame;
    byok_error_cb   on_error;   /* may be NULL to ignore errors */
    void           *user_ctx;

    /* --- internal state; do not touch directly --- */
    byok_parse_state_t state;
    uint8_t  buf[BYOK_MAX_FRAME]; /* accumulates header+payload+crc in place */
    size_t   buf_used;
    size_t   frame_total;   /* buf_used target for the current frame once
                              * LEN is known: 9 + len + 4 */
    int      hunt_stage;    /* 0 = no partial magic seen; 1 = saw 0x42 */
    uint32_t resync_events; /* count of rejected frames (§7.7 note) */

    /* §7.5 per-peer SEQ window */
    bool     have_last_seq;
    uint16_t last_seq;
    uint16_t seq_window[8];
    uint8_t  seq_window_count;
    uint8_t  seq_window_next;
} byok_parser_t;

/* Initialise/reset a parser to a fresh HUNT state with an empty SEQ window
 * (as if a brand-new session had started, §7.5's "wild jump ... clears its
 * duplicate window"). on_frame is required; on_error and user_ctx may be
 * set (or left NULL/ignored) by the caller after this call too. */
void byok_parser_init(byok_parser_t *p, byok_frame_cb on_frame,
                       byok_error_cb on_error, void *user_ctx);

/* Discard any partially-accumulated frame and return to HUNT, WITHOUT
 * touching the SEQ window. Intended for the §7.6 200 ms mid-frame stall
 * timeout, which the caller (owning a clock) detects and acts on; this
 * library has no notion of time. */
void byok_parser_reset_to_hunt(byok_parser_t *p);

/* Forget the peer's last-seen SEQ and duplicate window, WITHOUT touching
 * HUNT/framing state. §7.5: "a host that reconnects starts again from any
 * SEQ; a device that sees a wild jump treats it as a new session, clears
 * its duplicate window, and does not complain." track_sequence() already
 * does this on its own for a large forward jump, but a HELLO is an
 * explicit, unambiguous new-session signal (the new SEQ need not be far
 * from the old one), so callers should call this from their HELLO handler
 * rather than rely on the jump heuristic alone. */
void byok_parser_new_session(byok_parser_t *p);

/* Feed `len` newly-arrived bytes into the parser. Processes them one at a
 * time internally, so it makes no difference whether bytes arrive as one
 * chunk, in arbitrary pieces, or one byte per call — the result is
 * identical either way. May invoke on_frame / on_error any number of times
 * (including zero) before returning. */
void byok_parser_feed(byok_parser_t *p, const uint8_t *data, size_t len);

/* Number of frames this parser has rejected and resynced from since init
 * (§7.7: "a rising count is the first symptom of a transport bug"). */
static inline uint32_t byok_parser_resync_count(const byok_parser_t *p)
{
    return p->resync_events;
}

#ifdef __cplusplus
}
#endif

#endif /* BYOK_PROTO_H */
