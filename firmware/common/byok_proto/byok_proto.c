/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_proto.c — BYOK Link protocol v1: frame encoder + incremental decoder
 * ============================================================================
 * See byok_proto.h for the contract. Section numbers below refer to
 * docs/protocol.md.
 */
#include "byok_proto.h"
#include "byok_crc32.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Little-endian helpers (wire format is LE throughout, §3/§6)            */
/* ---------------------------------------------------------------------- */

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------------------------------------------------------------------- */
/* Encoder                                                                 */
/* ---------------------------------------------------------------------- */

size_t byok_frame_encode(uint8_t type, uint8_t flags, uint16_t seq,
                          const uint8_t *payload, uint16_t payload_len,
                          uint8_t *out_buf, size_t out_buf_len)
{
    if (payload_len > BYOK_MAX_PAYLOAD) {
        return 0;
    }
    if (payload_len != 0 && payload == NULL) {
        return 0;
    }
    size_t total = byok_frame_wire_size(payload_len);
    if (out_buf == NULL || out_buf_len < total) {
        return 0;
    }

    uint8_t *hdr = out_buf;
    hdr[0] = BYOK_MAGIC0;
    hdr[1] = BYOK_MAGIC1;
    hdr[2] = (uint8_t)BYOK_VERSION;
    hdr[3] = type;
    hdr[4] = flags;
    put_u16le(&hdr[5], seq);
    put_u16le(&hdr[7], payload_len);

    if (payload_len != 0) {
        memcpy(out_buf + BYOK_HEADER_LEN, payload, payload_len);
    }

    uint32_t crc = byok_crc32_init();
    crc = byok_crc32_update(crc, out_buf, BYOK_HEADER_LEN);
    crc = byok_crc32_update(crc, out_buf + BYOK_HEADER_LEN, payload_len);
    crc = byok_crc32_finish(crc);
    put_u32le(out_buf + BYOK_HEADER_LEN + payload_len, crc);

    return total;
}

/* ---------------------------------------------------------------------- */
/* Decoder                                                                 */
/* ---------------------------------------------------------------------- */

void byok_parser_init(byok_parser_t *p, byok_frame_cb on_frame,
                       byok_error_cb on_error, void *user_ctx)
{
    memset(p, 0, sizeof(*p));
    p->on_frame = on_frame;
    p->on_error = on_error;
    p->user_ctx = user_ctx;
    p->state = BYOK_PARSE_HUNT;
    p->hunt_stage = 0;
    p->have_last_seq = false;
}

void byok_parser_reset_to_hunt(byok_parser_t *p)
{
    p->state = BYOK_PARSE_HUNT;
    p->hunt_stage = 0;
    p->buf_used = 0;
    p->frame_total = 0;
    /* SEQ window is deliberately left untouched — this is a framing-level
     * resync, not a new session (§7.6 mid-frame stall: "No NACK ... the
     * sender may simply have been unplugged", not grounds to forget what
     * SEQs we've already processed). */
}

void byok_parser_new_session(byok_parser_t *p)
{
    p->have_last_seq = false;
    p->last_seq = 0;
    p->seq_window_count = 0;
    p->seq_window_next = 0;
    /* HUNT/framing state (state, hunt_stage, buf_used, frame_total) is
     * untouched — a new session doesn't imply a partial frame in flight
     * became invalid. */
}

/* §7.5: was `seq` seen in the last 8 distinct SEQs from this peer? */
static bool seq_in_window(const byok_parser_t *p, uint16_t seq)
{
    for (uint8_t i = 0; i < p->seq_window_count; i++) {
        if (p->seq_window[i] == seq) {
            return true;
        }
    }
    return false;
}

static void seq_window_push(byok_parser_t *p, uint16_t seq)
{
    p->seq_window[p->seq_window_next] = seq;
    p->seq_window_next = (uint8_t)((p->seq_window_next + 1) % 8);
    if (p->seq_window_count < 8) {
        p->seq_window_count++;
    }
}

/* A forward jump bigger than this is treated as a new session rather than
 * a gap (§7.5's "wild jump"), matching HELLO's explicit
 * byok_parser_new_session() call for the common case and covering a
 * reconnect that, for whatever reason, didn't go through HELLO again. */
#define BYOK_SEQ_WILD_JUMP_THRESHOLD 1024

/* Fills is_duplicate/seq_gap on `frame` (whose seq/type are already set)
 * per §7.5, and updates the parser's per-peer SEQ tracking state. */
static void track_sequence(byok_parser_t *p, byok_frame_t *frame)
{
    bool dup = seq_in_window(p, frame->seq) &&
               (frame->type != BYOK_TYPE_FRAME_DATA);

    frame->is_duplicate = dup;
    frame->seq_gap = 0;

    if (dup) {
        /* Retransmission of a known SEQ: not new, not a gap, and the
         * window/last_seq already reflect this SEQ from when it first
         * arrived — nothing to advance. */
        return;
    }

    if (p->have_last_seq) {
        /* Unsigned subtraction wraps mod 2^16, so `diff` is only a
         * meaningful "how far forward" count for the low half of that
         * range; the high half is a SEQ that moved BACKWARD (a small
         * negative step, e.g. diff=0xFFFF means -1). §7.5 explicitly
         * blesses a backward/out-of-order FRAME_DATA (chunk retransmission
         * "is legitimate and offset-addressed") — that must not be scored
         * as a ~65535-frame gap, and must not drag last_seq backward
         * either, or the *next* in-order frame would then look like a
         * huge forward jump. */
        uint16_t diff = (uint16_t)(frame->seq - p->last_seq);

        if (diff == 0) {
            /* Exact repeat of the last-seen SEQ (e.g. a FRAME_DATA
             * retransmit of the most recent chunk). Not a gap; state
             * already reflects it. */
            return;
        }
        if (diff > 0x8000u) {
            /* Backward SEQ — see above. Leave last_seq/window untouched. */
            return;
        }
        if (diff > BYOK_SEQ_WILD_JUMP_THRESHOLD) {
            /* §7.5: "a device that sees a wild jump treats it as a new
             * session, clears its duplicate window, and does not
             * complain." */
            p->seq_window_count = 0;
            p->seq_window_next = 0;
        } else if (diff > 1) {
            uint16_t missed = (uint16_t)(diff - 1);
            frame->seq_gap = (missed > 255) ? 255 : (uint8_t)missed;
        }
    }

    p->have_last_seq = true;
    p->last_seq = frame->seq;
    seq_window_push(p, frame->seq);
}

static void report_error(byok_parser_t *p, byok_parse_err_t err)
{
    p->resync_events++;
    if (p->on_error != NULL) {
        p->on_error(p->user_ctx, err, p->buf, p->buf_used);
    }
}

static void dispatch_frame(byok_parser_t *p)
{
    /* p->buf[0..buf_used) is a complete, CRC-verified frame at this point:
     * header (9) + payload (len) + crc (4) == frame_total == buf_used. */
    uint16_t len = get_u16le(&p->buf[7]);

    byok_frame_t frame;
    frame.version = p->buf[2];
    frame.type    = p->buf[3];
    frame.flags   = p->buf[4];
    frame.seq     = get_u16le(&p->buf[5]);
    frame.len     = len;
    frame.payload = (len != 0) ? &p->buf[BYOK_HEADER_LEN] : NULL;

    track_sequence(p, &frame);

    if (p->on_frame != NULL) {
        p->on_frame(p->user_ctx, &frame);
    }
}

/* Processes exactly one incoming byte through the HUNT/HEADER/PAYLOAD/CRC
 * state machine (§7.7). May call on_frame or on_error zero or one time. */
static void feed_byte(byok_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case BYOK_PARSE_HUNT:
        if (b == BYOK_MAGIC0) {
            p->hunt_stage = 1;
        } else if (p->hunt_stage == 1 && b == BYOK_MAGIC1) {
            p->buf[0] = BYOK_MAGIC0;
            p->buf[1] = BYOK_MAGIC1;
            p->buf_used = 2;
            p->hunt_stage = 0;
            p->state = BYOK_PARSE_HEADER;
        } else {
            p->hunt_stage = 0;
        }
        break;

    case BYOK_PARSE_HEADER:
        p->buf[p->buf_used++] = b;
        if (p->buf_used == BYOK_HEADER_LEN) {
            uint8_t version = p->buf[2];
            uint16_t len = get_u16le(&p->buf[7]);

            if (version != BYOK_VERSION || len > BYOK_MAX_PAYLOAD) {
                report_error(p, (version != BYOK_VERSION) ? BYOK_ERR_BAD_VERSION
                                                            : BYOK_ERR_BAD_LENGTH);
                /* §7.7 step 2: "back to HUNT starting after the magic just
                 * consumed" -- not after the whole rejected header. Save
                 * the 7 post-magic bytes before reset_to_hunt() clears the
                 * buffer, then re-scan them for an embedded magic so a
                 * `42 4B` hiding inside a bad header isn't thrown away
                 * with it. Bounded to one pass of BYOK_HEADER_LEN-2 bytes:
                 * a full new header can't complete from this leftover
                 * alone (not enough bytes remain after any in-range magic
                 * offset), so feed_byte() only recurses once here. */
                uint8_t saved[BYOK_HEADER_LEN - 2];
                size_t saved_len = sizeof(saved);
                memcpy(saved, &p->buf[2], saved_len);
                byok_parser_reset_to_hunt(p);
                for (size_t i = 0; i < saved_len; i++) {
                    feed_byte(p, saved[i]);
                }
                break;
            }

            p->frame_total = (size_t)BYOK_HEADER_LEN + len + BYOK_CRC_LEN;
            p->state = (len != 0) ? BYOK_PARSE_PAYLOAD : BYOK_PARSE_CRC;
        }
        break;

    case BYOK_PARSE_PAYLOAD:
        p->buf[p->buf_used++] = b;
        if (p->buf_used == p->frame_total - BYOK_CRC_LEN) {
            p->state = BYOK_PARSE_CRC;
        }
        break;

    case BYOK_PARSE_CRC:
        p->buf[p->buf_used++] = b;
        if (p->buf_used == p->frame_total) {
            uint16_t len = get_u16le(&p->buf[7]);
            uint32_t computed = byok_crc32(p->buf, BYOK_HEADER_LEN + len);
            uint32_t received = get_u32le(&p->buf[BYOK_HEADER_LEN + len]);

            if (computed != received) {
                report_error(p, BYOK_ERR_BAD_CRC);
                byok_parser_reset_to_hunt(p);
                break;
            }

            dispatch_frame(p);
            byok_parser_reset_to_hunt(p);
        }
        break;
    }
}

void byok_parser_feed(byok_parser_t *p, const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        feed_byte(p, data[i]);
    }
}
