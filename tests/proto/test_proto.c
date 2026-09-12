/* SPDX-License-Identifier: MIT
 * ============================================================================
 * test_proto.c — host tests for firmware/common/byok_proto
 * ============================================================================
 * No framework, plain assert(). Build/run via tests/proto/Makefile:
 *
 *   make -C tests/proto test
 *
 * Also writes tests/proto/vectors.json — the §12 test vectors plus a few
 * extra encoder outputs, byte-exact, for tests/host/test_proto.py to load
 * and cross-check the Python reference implementation against this C
 * implementation's actual output (not just against each side's own idea of
 * what the spec says).
 */
#include "byok_proto.h"
#include "byok_crc32.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------- */
/* Small helpers                                                          */
/* ---------------------------------------------------------------------- */

static void print_hex(FILE *f, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        fprintf(f, "%02x", buf[i]);
    }
}

/* Capturing context used by most decoder tests below. */
typedef struct {
    int frame_count;
    byok_frame_t last_frame;
    uint8_t last_payload_copy[BYOK_MAX_PAYLOAD];

    int error_count;
    byok_parse_err_t last_err;
    uint8_t last_err_header[BYOK_MAX_FRAME];
    size_t last_err_header_len;
} capture_t;

static void on_frame_capture(void *ctx, const byok_frame_t *frame)
{
    capture_t *c = (capture_t *)ctx;
    c->frame_count++;
    c->last_frame = *frame;
    if (frame->len != 0) {
        memcpy(c->last_payload_copy, frame->payload, frame->len);
        c->last_frame.payload = c->last_payload_copy; /* survive past callback */
    } else {
        c->last_frame.payload = NULL;
    }
}

static void on_error_capture(void *ctx, byok_parse_err_t err,
                              const uint8_t *header, size_t header_len)
{
    capture_t *c = (capture_t *)ctx;
    c->error_count++;
    c->last_err = err;
    size_t n = header_len < sizeof(c->last_err_header) ? header_len : sizeof(c->last_err_header);
    memcpy(c->last_err_header, header, n);
    c->last_err_header_len = n;
}

static void capture_init(capture_t *c)
{
    memset(c, 0, sizeof(*c));
}

/* ---------------------------------------------------------------------- */
/* §12 test vectors — normative, byte-exact                               */
/* ---------------------------------------------------------------------- */

typedef struct {
    const char *name;
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    const uint8_t *payload;
    uint16_t payload_len;
    uint32_t expect_crc;
    const uint8_t *expect_frame;
    size_t expect_frame_len;
} spec_vector_t;

static void run_spec_vectors_and_write_json(void)
{
    static const uint8_t v1_payload[1] = { 0 }; /* unused, len 0 -- never dereferenced */
    static const uint8_t v2_payload[8] = { 0x01,0x01,0x00,0x10,0x0F,0x00,0x00,0x00 };
    static const uint8_t v3_payload[1] = { 0x00 };
    static const uint8_t v4_payload[3] = { 0x80,0xF4,0x01 };
    static const uint8_t v5_payload[12] = { 0x04,0x00,0x08,0x00,0x01,0x00,0x04,0x00,0x42,0x59,0x4F,0x4B };

    static const uint8_t v1_frame[] = { 0x42,0x4B,0x01,0x07,0x00,0x01,0x00,0x00,0x00, 0xFD,0xBD,0x54,0x61 };
    static const uint8_t v2_frame[] = { 0x42,0x4B,0x01,0x01,0x01,0x02,0x00,0x08,0x00, 0x01,0x01,0x00,0x10,0x0F,0x00,0x00,0x00, 0x47,0x68,0xE0,0x60 };
    static const uint8_t v3_frame[] = { 0x42,0x4B,0x01,0x10,0x00,0x03,0x00,0x01,0x00, 0x00, 0x86,0xAE,0x14,0xC8 };
    static const uint8_t v4_frame[] = { 0x42,0x4B,0x01,0x21,0x01,0x04,0x00,0x03,0x00, 0x80,0xF4,0x01, 0x87,0xDD,0x0B,0xB1 };
    static const uint8_t v5_frame[] = { 0x42,0x4B,0x01,0x11,0x01,0x05,0x00,0x0C,0x00, 0x04,0x00,0x08,0x00,0x01,0x00,0x04,0x00,0x42,0x59,0x4F,0x4B, 0xE8,0x28,0xC7,0x14 };

    const spec_vector_t vecs[] = {
        { "PING",          0x07, 0x00, 1, v1_payload, 0,  0x6154BDFD, v1_frame, sizeof(v1_frame) },
        { "HELLO",         0x01, 0x01, 2, v2_payload, 8,  0x60E06847, v2_frame, sizeof(v2_frame) },
        { "CLEAR",         0x10, 0x00, 3, v3_payload, 1,  0xC814AE86, v3_frame, sizeof(v3_frame) },
        { "SET_BACKLIGHT", 0x21, 0x01, 4, v4_payload, 3,  0xB10BDD87, v4_frame, sizeof(v4_frame) },
        { "DRAW_TEXT",     0x11, 0x01, 5, v5_payload, 12, 0x14C728E8, v5_frame, sizeof(v5_frame) },
    };
    const size_t n_vecs = sizeof(vecs) / sizeof(vecs[0]);

    /* §12 check #6: CRC of a 16-byte checkerboard row on its own (no frame
     * wrapper — just the raw CRC32 primitive). */
    uint8_t checkerboard[16];
    memset(checkerboard, 0xAA, sizeof(checkerboard));
    assert(byok_crc32(checkerboard, sizeof(checkerboard)) == 0xC79B40E0);

    FILE *jf = fopen("vectors.json", "w");
    assert(jf != NULL);
    fprintf(jf, "[\n");

    for (size_t i = 0; i < n_vecs; i++) {
        const spec_vector_t *v = &vecs[i];

        /* Encoder must reproduce the exact frame bytes. */
        uint8_t buf[BYOK_MAX_FRAME];
        size_t n = byok_frame_encode(v->type, v->flags, v->seq, v->payload_len ? v->payload : NULL,
                                      v->payload_len, buf, sizeof(buf));
        assert(n == v->expect_frame_len);
        assert(memcmp(buf, v->expect_frame, n) == 0);

        uint32_t crc = byok_crc32(buf, 9 + v->payload_len);
        assert(crc == v->expect_crc);

        /* Decoder must recover the same fields, fed as one chunk. */
        capture_t cap;
        capture_init(&cap);
        byok_parser_t p;
        byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
        byok_parser_feed(&p, buf, n);
        assert(cap.frame_count == 1);
        assert(cap.error_count == 0);
        assert(cap.last_frame.type == v->type);
        assert(cap.last_frame.flags == v->flags);
        assert(cap.last_frame.seq == v->seq);
        assert(cap.last_frame.len == v->payload_len);
        if (v->payload_len) {
            assert(memcmp(cap.last_frame.payload, v->payload, v->payload_len) == 0);
        }

        fprintf(jf, "  {\n");
        fprintf(jf, "    \"name\": \"%s\",\n", v->name);
        fprintf(jf, "    \"type\": %u,\n", v->type);
        fprintf(jf, "    \"flags\": %u,\n", v->flags);
        fprintf(jf, "    \"seq\": %u,\n", v->seq);
        fprintf(jf, "    \"payload_hex\": \"");
        print_hex(jf, v->payload, v->payload_len);
        fprintf(jf, "\",\n");
        fprintf(jf, "    \"crc32\": %u,\n", (unsigned)v->expect_crc);
        fprintf(jf, "    \"frame_hex\": \"");
        print_hex(jf, buf, n);
        fprintf(jf, "\"\n");
        fprintf(jf, "  }%s\n", (i + 1 < n_vecs) ? "," : "");
    }

    fprintf(jf, "]\n");
    fclose(jf);
    printf("[ok] spec vectors #1-#5 + checkerboard CRC check #6 (vectors.json written)\n");
}

/* ---------------------------------------------------------------------- */
/* Round trip for every message type in §5                                */
/* ---------------------------------------------------------------------- */

typedef struct {
    const char *name;
    uint8_t type;
    uint16_t payload_len; /* representative length; DRAW_TEXT/EVT_LOG use n>0 */
} type_case_t;

static void test_roundtrip_all_types(void)
{
    static const type_case_t cases[] = {
        { "HELLO",           0x01, 8 },
        { "HELLO_ACK",       0x02, 32 },
        { "GET_INFO",        0x03, 0 },
        { "INFO",             0x04, 64 },
        { "GET_STATUS",      0x05, 0 },
        { "STATUS",          0x06, 20 },
        { "PING",            0x07, 0 },
        { "PING_cookie",     0x07, 4 },
        { "PONG",            0x08, 0 },
        { "PONG_cookie",     0x08, 4 },
        { "ACK",             0x09, 0 },
        { "NACK",            0x0A, 4 },
        { "CLEAR",           0x10, 1 },
        { "DRAW_TEXT",       0x11, 12 },
        { "DRAW_RECT",       0x12, 10 },
        { "DRAW_BITMAP",     0x13, 18 },
        { "FRAME_BEGIN",     0x14, 8 },
        { "FRAME_DATA",      0x15, 16 },
        { "FRAME_END",       0x16, 5 },
        { "PARTIAL_REFRESH", 0x17, 8 },
        { "FULL_REFRESH",    0x18, 1 },
        { "SET_MODE",        0x20, 2 },
        { "SET_BACKLIGHT",   0x21, 3 },
        { "SET_CONTRAST",    0x22, 1 },
        { "GET_BATTERY",     0x23, 0 },
        { "BATTERY",         0x24, 8 },
        { "REBOOT",          0x25, 2 },
        { "BOOT_ORIGINAL",   0x26, 4 },
        { "EVT_BUTTON",      0x30, 6 },
        { "EVT_STATUS",      0x31, 20 },
        { "EVT_LOG",         0x32, 10 },
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);

    for (size_t i = 0; i < n_cases; i++) {
        const type_case_t *tc = &cases[i];
        uint8_t payload[BYOK_MAX_PAYLOAD];
        for (uint16_t j = 0; j < tc->payload_len; j++) {
            payload[j] = (uint8_t)((tc->type * 31 + j * 7 + i) & 0xFF);
        }

        uint8_t buf[BYOK_MAX_FRAME];
        uint16_t seq = (uint16_t)(1000 + i);
        uint8_t flags = (uint8_t)((tc->type & BYOK_FLAG_EVENT) ? BYOK_FLAG_EVENT : BYOK_FLAG_ACK_REQ);
        size_t n = byok_frame_encode(tc->type, flags, seq,
                                      tc->payload_len ? payload : NULL, tc->payload_len,
                                      buf, sizeof(buf));
        assert(n == byok_frame_wire_size(tc->payload_len));

        capture_t cap;
        capture_init(&cap);
        byok_parser_t p;
        byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
        byok_parser_feed(&p, buf, n);

        assert(cap.error_count == 0);
        assert(cap.frame_count == 1);
        assert(cap.last_frame.version == BYOK_VERSION);
        assert(cap.last_frame.type == tc->type);
        assert(cap.last_frame.flags == flags);
        assert(cap.last_frame.seq == seq);
        assert(cap.last_frame.len == tc->payload_len);
        if (tc->payload_len) {
            assert(memcmp(cap.last_frame.payload, payload, tc->payload_len) == 0);
        } else {
            assert(cap.last_frame.payload == NULL);
        }
        /* First frame from a fresh parser is never a duplicate/gap. */
        assert(cap.last_frame.is_duplicate == false);
        assert(cap.last_frame.seq_gap == 0);
    }
    printf("[ok] encode/decode round trip for all %zu message types (§5)\n", n_cases);
}

/* ---------------------------------------------------------------------- */
/* Malformed length: LEN within range but not what the sender meant       */
/* ---------------------------------------------------------------------- */

static void test_malformed_length(void)
{
    /* Build a valid CLEAR frame (LEN=1, 14 bytes total), then corrupt just
     * the LEN field to a different in-range value (5) without touching
     * payload/CRC. The frame is no longer internally consistent -- the 5
     * "payload" bytes the parser now reads before hitting the CRC field
     * are actually the original 1 payload byte + the original 4 CRC bytes,
     * so the CRC check necessarily fails: the sender's CRC was computed
     * over the true (LEN=1) bytes, and no LEN value other than the true
     * one reproduces it. This is what a "malformed length" (in-range but
     * wrong for the actual content) looks like on the wire: the framing
     * layer cannot know LEN is wrong for CLEAR specifically (§9
     * E_BAD_LENGTH "wrong for this type" is a dispatch-layer judgement --
     * see byok_proto.h), but the CRC backstop still catches the
     * corruption. */
    uint8_t buf[BYOK_MAX_FRAME];
    uint8_t payload[1] = { 0x00 };
    size_t n = byok_frame_encode(BYOK_TYPE_CLEAR, 0x00, 9, payload, 1, buf, sizeof(buf));
    assert(n == 14);

    uint8_t corrupted[64];
    memcpy(corrupted, buf, n);
    corrupted[7] = 5; /* LEN (LE lo byte): 1 -> 5, still <= BYOK_MAX_PAYLOAD */
    corrupted[8] = 0;

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
    /* Feed the corrupted frame, then extra bytes so the parser (now
     * expecting a 5-byte payload + 4-byte CRC = 9 more bytes after the
     * header, but the original frame only had 4 bytes left after the
     * header) has enough bytes to reach its CRC check. */
    byok_parser_feed(&p, corrupted, n);
    uint8_t filler[16] = { 0 };
    byok_parser_feed(&p, filler, sizeof(filler));

    assert(cap.frame_count == 0);
    assert(cap.error_count == 1);
    assert(cap.last_err == BYOK_ERR_BAD_CRC);
    printf("[ok] malformed length (in-range but wrong for content) -> CRC catches it\n");
}

/* ---------------------------------------------------------------------- */
/* Oversized length: LEN > BYOK_MAX_PAYLOAD                                */
/* ---------------------------------------------------------------------- */

static void test_oversized_length(void)
{
    uint8_t header[9] = { 0x42,0x4B,0x01,0x07,0x00, 0x2A,0x00, 0xFF,0xFF }; /* LEN = 0xFFFF */

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
    byok_parser_feed(&p, header, sizeof(header));

    assert(cap.frame_count == 0);
    assert(cap.error_count == 1);
    assert(cap.last_err == BYOK_ERR_BAD_LENGTH);
    assert(byok_parser_resync_count(&p) == 1);

    /* Parser must have resynced to HUNT, not gotten stuck: a following
     * valid frame decodes normally. */
    uint8_t buf[64];
    size_t n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 1, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.frame_count == 1);
    assert(cap.last_frame.type == BYOK_TYPE_PING);
    printf("[ok] oversized length (LEN > 4096) -> E_BAD_LENGTH, resyncs cleanly\n");
}

/* ---------------------------------------------------------------------- */
/* Bad CRC                                                                 */
/* ---------------------------------------------------------------------- */

static void test_bad_crc(void)
{
    uint8_t buf[64];
    uint8_t payload[3] = { 0x80, 0xF4, 0x01 };
    size_t n = byok_frame_encode(BYOK_TYPE_SET_BACKLIGHT, 0x01, 4, payload, 3, buf, sizeof(buf));
    assert(n == 16);
    buf[n - 1] ^= 0xFF; /* flip a CRC byte */

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
    byok_parser_feed(&p, buf, n);

    assert(cap.frame_count == 0);
    assert(cap.error_count == 1);
    assert(cap.last_err == BYOK_ERR_BAD_CRC);
    printf("[ok] bad CRC -> E_BAD_CRC, frame dropped\n");
}

/* ---------------------------------------------------------------------- */
/* Bad version                                                            */
/* ---------------------------------------------------------------------- */

static void test_bad_version(void)
{
    uint8_t buf[64];
    size_t n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 7, NULL, 0, buf, sizeof(buf));
    buf[2] = 0x02; /* VERSION field: 1 -> 2 (unsupported) */
    /* CRC now also mismatches, but VERSION is checked first (§7.7 step 2). */

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
    byok_parser_feed(&p, buf, n);

    assert(cap.frame_count == 0);
    assert(cap.error_count == 1);
    assert(cap.last_err == BYOK_ERR_BAD_VERSION);
    printf("[ok] bad version -> E_BAD_VERSION (checked before CRC)\n");
}

/* ---------------------------------------------------------------------- */
/* Truncated frame, then completion in a later feed                       */
/* ---------------------------------------------------------------------- */

static void test_truncated_then_complete(void)
{
    uint8_t buf[64];
    uint8_t payload[12] = { 0x04,0x00,0x08,0x00,0x01,0x00,0x04,0x00,0x42,0x59,0x4F,0x4B };
    size_t n = byok_frame_encode(BYOK_TYPE_DRAW_TEXT, 0x01, 5, payload, 12, buf, sizeof(buf));
    assert(n == 25);

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);

    byok_parser_feed(&p, buf, 10); /* header + 1 payload byte */
    assert(cap.frame_count == 0);
    assert(cap.error_count == 0); /* no error -- just waiting, per §7.7 PAYLOAD */

    byok_parser_feed(&p, buf + 10, n - 10); /* rest of the frame */
    assert(cap.frame_count == 1);
    assert(cap.error_count == 0);
    assert(cap.last_frame.type == BYOK_TYPE_DRAW_TEXT);
    assert(cap.last_frame.len == 12);
    assert(memcmp(cap.last_frame.payload, payload, 12) == 0);
    printf("[ok] truncated frame waits, then completes correctly on later bytes\n");
}

/* ---------------------------------------------------------------------- */
/* Garbage before magic                                                  */
/* ---------------------------------------------------------------------- */

static void test_garbage_before_magic(void)
{
    uint8_t buf[64];
    size_t n = byok_frame_encode(BYOK_TYPE_GET_STATUS, 0x00, 42, NULL, 0, buf, sizeof(buf));

    uint8_t stream[128];
    size_t sn = 0;
    /* Noise including partial/false magics: a lone 0x42, a lone 0x4B, a
     * 0x42 0x42 run, and an unrelated byte run -- none of it should ever
     * produce a spurious frame or error callback; HUNT just discards it. */
    uint8_t noise[] = { 0x00, 0xFF, 0x42, 0x00, 0x4B, 0x42, 0x42, 0x01, 0x99, 0x10, 0x20, 0x30 };
    memcpy(stream + sn, noise, sizeof(noise)); sn += sizeof(noise);
    memcpy(stream + sn, buf, n); sn += n;

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
    byok_parser_feed(&p, stream, sn);

    assert(cap.error_count == 0);
    assert(cap.frame_count == 1);
    assert(cap.last_frame.type == BYOK_TYPE_GET_STATUS);
    assert(cap.last_frame.seq == 42);
    printf("[ok] garbage (including false-magic runs) before a real frame is silently discarded\n");
}

/* ---------------------------------------------------------------------- */
/* Duplicate SEQ                                                          */
/* ---------------------------------------------------------------------- */

static void test_duplicate_seq(void)
{
    /* Idempotent type (PING): a resend of the same SEQ is flagged
     * is_duplicate, per §7.5. */
    uint8_t buf[64];
    size_t n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 100, NULL, 0, buf, sizeof(buf));

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);

    byok_parser_feed(&p, buf, n);
    assert(cap.frame_count == 1);
    assert(cap.last_frame.is_duplicate == false);

    byok_parser_feed(&p, buf, n); /* exact same bytes again */
    assert(cap.frame_count == 2);
    assert(cap.last_frame.is_duplicate == true);
    assert(cap.last_frame.seq_gap == 0);

    /* FRAME_DATA is explicitly exempt from dedup (§7.5: "a duplicate
     * FRAME_DATA is not dropped -- chunk retransmission is legitimate"). */
    uint8_t fd_payload[8] = { 0,0,0,0, 1,2,3,4 };
    uint8_t fbuf[64];
    size_t fn = byok_frame_encode(BYOK_TYPE_FRAME_DATA, 0x00, 200, fd_payload, 8, fbuf, sizeof(fbuf));
    byok_parser_feed(&p, fbuf, fn);
    assert(cap.last_frame.is_duplicate == false);
    byok_parser_feed(&p, fbuf, fn); /* retransmit same chunk */
    assert(cap.last_frame.is_duplicate == false);
    printf("[ok] duplicate SEQ flagged for idempotent types, exempted for FRAME_DATA\n");
}

/* ---------------------------------------------------------------------- */
/* SEQ gap                                                                */
/* ---------------------------------------------------------------------- */

static void test_seq_gap(void)
{
    uint8_t buf[64];
    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);

    size_t n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 10, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.last_frame.seq_gap == 0);

    /* Jump from 10 to 15: 4 frames missed (11,12,13,14). Frame still
     * processes normally (§7.5: "not an error condition for the link"). */
    n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 15, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.frame_count == 2);
    assert(cap.error_count == 0); /* gap is informational, not a decode error */
    assert(cap.last_frame.seq_gap == 4);
    assert(cap.last_frame.is_duplicate == false);

    /* Consecutive SEQ: no gap. */
    n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 16, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.last_frame.seq_gap == 0);

    /* Saturation: jump far ahead, gap caps at 255. */
    n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 1000, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.last_frame.seq_gap == 255);
    printf("[ok] SEQ gap detection, including saturation at 255\n");
}

/* ---------------------------------------------------------------------- */
/* Regression: backward SEQ (retransmit) must not be scored as a gap      */
/* (was: (uint16_t)(seq - last_seq) wrapped a backward step into a        */
/* ~65535-frame "gap", and clobbered last_seq backward too, so the NEXT   */
/* in-order frame then looked like a huge forward jump).                  */
/* ---------------------------------------------------------------------- */

static void test_seq_backward_retransmit(void)
{
    uint8_t fbuf[64];
    uint8_t payload[8] = { 0, 0, 0, 0, 1, 2, 3, 4 };

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);

    /* FRAME_DATA seq 1, 2, 3 -- normal forward progression. */
    for (uint16_t seq = 1; seq <= 3; seq++) {
        size_t n = byok_frame_encode(BYOK_TYPE_FRAME_DATA, 0x00, seq, payload, 8, fbuf, sizeof(fbuf));
        byok_parser_feed(&p, fbuf, n);
        assert(cap.last_frame.seq_gap == 0);
    }

    /* Legitimate retransmit of an EARLIER chunk (seq 1, not the last-seen
     * seq 3). §7.5 blesses this for FRAME_DATA. Must not be scored as a
     * gap, and must not drag last_seq backward. */
    size_t n = byok_frame_encode(BYOK_TYPE_FRAME_DATA, 0x00, 1, payload, 8, fbuf, sizeof(fbuf));
    byok_parser_feed(&p, fbuf, n);
    assert(cap.last_frame.seq_gap == 0);
    assert(cap.last_frame.is_duplicate == false); /* FRAME_DATA is dedup-exempt */

    /* The NEXT in-order frame (seq 4) must still read as a clean,
     * consecutive step -- not a ~65531-frame gap from the backward seq 1
     * having overwritten last_seq. */
    n = byok_frame_encode(BYOK_TYPE_FRAME_DATA, 0x00, 4, payload, 8, fbuf, sizeof(fbuf));
    byok_parser_feed(&p, fbuf, n);
    assert(cap.last_frame.seq_gap == 0);

    printf("[ok] backward/retransmitted SEQ is not scored as a gap, and doesn't corrupt tracking\n");
}

/* ---------------------------------------------------------------------- */
/* Regression: a wild forward jump is a new session (§7.5), not a huge    */
/* gap report, and it clears the duplicate window.                        */
/* ---------------------------------------------------------------------- */

static void test_seq_wild_jump_is_new_session(void)
{
    uint8_t buf[64];
    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);

    size_t n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 10, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.last_frame.seq_gap == 0);

    /* A forward jump bigger than the wild-jump threshold (but well under
     * the halfway point of u16 wrap space, so it's unambiguously "forward
     * far", not read as a small backward step -- see the backward-SEQ
     * comment in track_sequence()): treated as a new session, not
     * complained about (seq_gap stays 0, not saturated). */
    n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 20000, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.last_frame.seq_gap == 0);
    assert(cap.last_frame.is_duplicate == false);

    /* The old SEQ (10) is no longer in the (cleared) duplicate window, so
     * seeing it again now reads as a plain backward SEQ, not a duplicate. */
    n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 10, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.last_frame.is_duplicate == false);

    printf("[ok] a wild forward SEQ jump is treated as a new session, not a huge gap\n");
}

/* ---------------------------------------------------------------------- */
/* Regression: byok_parser_new_session() forgets last_seq/window without  */
/* touching HUNT/framing state, e.g. safe to call from inside a HELLO     */
/* handler mid-dispatch (app_main.c does exactly this).                   */
/* ---------------------------------------------------------------------- */

static void test_new_session_api(void)
{
    uint8_t buf[64];
    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);

    size_t n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 9000, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.frame_count == 1);

    byok_parser_new_session(&p);

    /* A host that reconnected and restarted its SEQ at 1 must not look
     * like a ~65535-frame gap, or a duplicate. */
    n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 1, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.frame_count == 2);
    assert(cap.last_frame.seq_gap == 0);
    assert(cap.last_frame.is_duplicate == false);

    printf("[ok] byok_parser_new_session() clears SEQ tracking without touching framing state\n");
}

/* ---------------------------------------------------------------------- */
/* Regression: §7.7 step 2 -- a rejected header's embedded magic must     */
/* still be found, not discarded along with the rest of the bad header.   */
/* (was: byok_parser_reset_to_hunt() discarded all 9 header bytes,        */
/* including a `42 4B` sitting inside them.)                              */
/* ---------------------------------------------------------------------- */

static void test_bad_header_recovers_embedded_magic(void)
{
    /* A real, well-formed frame, which we'll embed inside a bad header. */
    uint8_t fbuf[64];
    size_t fn = byok_frame_encode(BYOK_TYPE_PING, 0x00, 555, NULL, 0, fbuf, sizeof(fbuf));

    /* 5-byte bogus prefix: magic, an invalid VERSION, then two don't-care
     * bytes (TYPE, FLAGS) -- the header is rejected on VERSION before LEN
     * is even inspected, so their content doesn't matter here. */
    uint8_t stream[5 + 64];
    stream[0] = 0x42;
    stream[1] = 0x4B;
    stream[2] = 0xFF; /* invalid VERSION */
    stream[3] = 0x00;
    stream[4] = 0x00;
    memcpy(stream + 5, fbuf, fn); /* the embedded, well-formed frame */
    size_t total = 5 + fn;

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
    byok_parser_feed(&p, stream, total);

    assert(cap.error_count == 1);
    assert(cap.last_err == BYOK_ERR_BAD_VERSION);
    assert(cap.frame_count == 1); /* the embedded frame was still found and decoded */
    assert(cap.last_frame.type == BYOK_TYPE_PING);
    assert(cap.last_frame.seq == 555);

    printf("[ok] a magic embedded inside a rejected header is recovered, not discarded\n");
}

/* ---------------------------------------------------------------------- */
/* Back-to-back frames in one chunk                                       */
/* ---------------------------------------------------------------------- */

static void test_back_to_back_one_chunk(void)
{
    uint8_t f1[64], f2[64], f3[64];
    size_t n1 = byok_frame_encode(BYOK_TYPE_PING, 0x00, 1, NULL, 0, f1, sizeof(f1));
    uint8_t p2[1] = { 1 };
    size_t n2 = byok_frame_encode(BYOK_TYPE_CLEAR, 0x00, 2, p2, 1, f2, sizeof(f2));
    size_t n3 = byok_frame_encode(BYOK_TYPE_GET_BATTERY, 0x00, 3, NULL, 0, f3, sizeof(f3));

    uint8_t stream[256];
    size_t sn = 0;
    memcpy(stream + sn, f1, n1); sn += n1;
    memcpy(stream + sn, f2, n2); sn += n2;
    memcpy(stream + sn, f3, n3); sn += n3;

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
    byok_parser_feed(&p, stream, sn); /* whole thing, one call */

    assert(cap.error_count == 0);
    assert(cap.frame_count == 3);
    assert(cap.last_frame.type == BYOK_TYPE_GET_BATTERY);
    assert(cap.last_frame.seq == 3);
    printf("[ok] three back-to-back frames delivered in a single feed() call\n");
}

/* ---------------------------------------------------------------------- */
/* Frame split across 1-byte feeds                                        */
/* ---------------------------------------------------------------------- */

static void test_split_one_byte_feeds(void)
{
    uint8_t buf[64];
    uint8_t payload[10] = { 0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A };
    size_t n = byok_frame_encode(BYOK_TYPE_DRAW_RECT, 0x01, 77, payload, 10, buf, sizeof(buf));

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);

    for (size_t i = 0; i < n; i++) {
        byok_parser_feed(&p, &buf[i], 1);
        if (i + 1 < n) {
            assert(cap.frame_count == 0);
        }
    }
    assert(cap.error_count == 0);
    assert(cap.frame_count == 1);
    assert(cap.last_frame.type == BYOK_TYPE_DRAW_RECT);
    assert(cap.last_frame.seq == 77);
    assert(memcmp(cap.last_frame.payload, payload, 10) == 0);
    printf("[ok] frame split across %zu one-byte feed() calls decodes identically\n", n);
}

/* ---------------------------------------------------------------------- */
/* Fuzz: random bytes must never crash, 10^6 iterations                   */
/* ---------------------------------------------------------------------- */

static void test_fuzz_random_bytes(void)
{
    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);

    srand(0xB4B0Cu);
    const long ITERATIONS = 1000000;
    uint8_t chunk[8];
    for (long i = 0; i < ITERATIONS; i++) {
        int chunk_len = 1 + (rand() % (int)sizeof(chunk));
        for (int j = 0; j < chunk_len; j++) {
            chunk[j] = (uint8_t)(rand() & 0xFF);
        }
        byok_parser_feed(&p, chunk, (size_t)chunk_len);
    }
    /* Reaching here without an assert/UBSan/ASan trap is the pass
     * condition. Also sanity-check the parser is still alive and can
     * decode a well-formed frame after a million bytes of noise. */
    capture_init(&cap);
    uint8_t buf[64];
    size_t n = byok_frame_encode(BYOK_TYPE_PING, 0x00, 1, NULL, 0, buf, sizeof(buf));
    byok_parser_feed(&p, buf, n);
    assert(cap.frame_count == 1);
    printf("[ok] fuzz: %ld random-byte iterations, no crash, parser still functional\n", ITERATIONS);
}

/* ---------------------------------------------------------------------- */
/* Reserved flag bits are ignored, not rejected (§4)                      */
/* ---------------------------------------------------------------------- */

static void test_reserved_flags_ignored(void)
{
    uint8_t buf[64];
    /* Hand-build a PING frame with reserved bits 5-7 set, recomputing CRC
     * (the encoder itself doesn't forbid setting them -- that's a caller
     * discipline the spec asks of senders, not something the encode
     * function enforces). */
    uint8_t hdr[9] = { 0x42,0x4B,0x01,0x07, 0xE0 /* all reserved bits */, 0x09,0x00, 0x00,0x00 };
    uint32_t crc = byok_crc32(hdr, 9);
    memcpy(buf, hdr, 9);
    buf[9]  = (uint8_t)(crc & 0xFF);
    buf[10] = (uint8_t)((crc >> 8) & 0xFF);
    buf[11] = (uint8_t)((crc >> 16) & 0xFF);
    buf[12] = (uint8_t)((crc >> 24) & 0xFF);

    capture_t cap;
    capture_init(&cap);
    byok_parser_t p;
    byok_parser_init(&p, on_frame_capture, on_error_capture, &cap);
    byok_parser_feed(&p, buf, 13);

    assert(cap.error_count == 0);
    assert(cap.frame_count == 1);
    assert(cap.last_frame.flags == 0xE0); /* decoder reports them; app must ignore them */
    printf("[ok] reserved flag bits round-trip without being rejected\n");
}

int main(void)
{
    run_spec_vectors_and_write_json();
    test_roundtrip_all_types();
    test_malformed_length();
    test_oversized_length();
    test_bad_crc();
    test_bad_version();
    test_truncated_then_complete();
    test_garbage_before_magic();
    test_duplicate_seq();
    test_seq_gap();
    test_seq_backward_retransmit();
    test_seq_wild_jump_is_new_session();
    test_new_session_api();
    test_bad_header_recovers_embedded_magic();
    test_back_to_back_one_chunk();
    test_split_one_byte_feeds();
    test_reserved_flags_ignored();
    test_fuzz_random_bytes();

    printf("\nALL TESTS PASSED\n");
    return 0;
}
