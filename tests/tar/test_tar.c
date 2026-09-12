/* SPDX-License-Identifier: MIT
 * ============================================================================
 * test_tar.c — host tests for firmware/common/byok_tar
 * ============================================================================
 * No framework, plain assert(). Build/run via tests/tar/Makefile:
 *
 *   make -C tests/tar test
 *
 * Header fixtures below are built independently of byok_tar.c's own
 * checksum/octal logic (a hand-rolled ustar writer, not a call into the
 * code under test) so the checksum tests actually exercise verification
 * rather than round-tripping the same implementation against itself.
 */
#include "byok_tar.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* ustar fixture builder — independent of byok_tar.c                      */
/* ---------------------------------------------------------------------- */

#define OFF_NAME     0
#define LEN_NAME     100
#define OFF_SIZE     124
#define LEN_SIZE     12
#define OFF_CHKSUM   148
#define LEN_CHKSUM   8
#define OFF_TYPEFLAG 156
#define OFF_MAGIC    257

static void write_octal_field(uint8_t *field, size_t len, uint64_t value)
{
    /* "%0(len-1)llo" + NUL, right-aligned with leading zeros, matching what
     * GNU tar / bsdtar write. */
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%0*llo", (int)(len - 1), (unsigned long long)value);
    memcpy(field, tmp, len - 1);
    field[len - 1] = '\0';
}

static uint32_t header_checksum(const uint8_t block[512])
{
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; i++) {
        if (i >= OFF_CHKSUM && i < OFF_CHKSUM + LEN_CHKSUM) {
            sum += (uint32_t)' ';
        } else {
            sum += block[i];
        }
    }
    return sum;
}

/* Builds a complete, valid ustar header block for a regular file. */
static void build_header(uint8_t block[512], const char *name, uint64_t size, char typeflag)
{
    memset(block, 0, 512);
    strncpy((char *)block + OFF_NAME, name, LEN_NAME);
    write_octal_field(block + 100, 8, 0644);  /* mode */
    write_octal_field(block + 108, 8, 0);     /* uid */
    write_octal_field(block + 116, 8, 0);     /* gid */
    write_octal_field(block + OFF_SIZE, LEN_SIZE, size);
    write_octal_field(block + 136, 12, 0); /* mtime */
    memset(block + OFF_CHKSUM, ' ', LEN_CHKSUM); /* placeholder before summing */
    block[OFF_TYPEFLAG] = (uint8_t)typeflag;
    memcpy(block + OFF_MAGIC, "ustar\0" "00", 8); /* magic + version */

    /* POSIX checksum field format: 6 octal digits, NUL, space. */
    char chk[9];
    snprintf(chk, sizeof(chk), "%06o", header_checksum(block));
    memcpy(block + OFF_CHKSUM, chk, 6);
    block[OFF_CHKSUM + 6] = '\0';
    block[OFF_CHKSUM + 7] = ' ';
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                  */
/* ---------------------------------------------------------------------- */

static void test_valid_header(void)
{
    uint8_t block[512];
    build_header(block, "BYOK.bin", 12345, '0');

    byok_tar_header_t out;
    byok_tar_hdr_status_t st = byok_tar_parse_header(block, &out);
    assert(st == BYOK_TAR_HDR_OK);
    assert(strcmp(out.name, "BYOK.bin") == 0);
    assert(out.size == 12345);
    assert(out.typeflag == '0');
}

static void test_valid_header_typeflag_nul(void)
{
    /* Some writers leave typeflag 0x00 for a plain file (pre-POSIX tar);
     * byok_tar.h documents this as an accepted "regular file" typeflag
     * (the caller checks for it, not this parser). */
    uint8_t block[512];
    build_header(block, "assets.tar", 999, '\0');

    byok_tar_header_t out;
    byok_tar_hdr_status_t st = byok_tar_parse_header(block, &out);
    assert(st == BYOK_TAR_HDR_OK);
    assert(out.typeflag == '\0');
}

static void test_max_length_name(void)
{
    /* Exactly 100 bytes, no room for a NUL terminator inside the field
     * itself — byok_tar_parse_header must still produce a valid C string. */
    char name[101];
    memset(name, 'A', 100);
    name[100] = '\0';

    uint8_t block[512];
    memset(block, 0, 512);
    memcpy(block + OFF_NAME, name, 100); /* no trailing NUL in the field */
    write_octal_field(block + 100, 8, 0644);
    write_octal_field(block + OFF_SIZE, LEN_SIZE, 42);
    memset(block + OFF_CHKSUM, ' ', LEN_CHKSUM);
    block[OFF_TYPEFLAG] = '0';
    memcpy(block + OFF_MAGIC, "ustar\0" "00", 8);
    char chk[9];
    snprintf(chk, sizeof(chk), "%06o", header_checksum(block));
    memcpy(block + OFF_CHKSUM, chk, 6);
    block[OFF_CHKSUM + 6] = '\0';
    block[OFF_CHKSUM + 7] = ' ';

    byok_tar_header_t out;
    byok_tar_hdr_status_t st = byok_tar_parse_header(block, &out);
    assert(st == BYOK_TAR_HDR_OK);
    assert(strlen(out.name) == 100);
    assert(strcmp(out.name, name) == 0);
}

static void test_end_marker(void)
{
    uint8_t block[512];
    memset(block, 0, 512);

    byok_tar_header_t out;
    byok_tar_hdr_status_t st = byok_tar_parse_header(block, &out);
    assert(st == BYOK_TAR_HDR_END);
}

static void test_bad_magic(void)
{
    uint8_t block[512];
    build_header(block, "BYOK.bin", 100, '0');
    block[OFF_MAGIC] = 'x'; /* corrupt "ustar" -> "xstar" */

    byok_tar_header_t out;
    byok_tar_hdr_status_t st = byok_tar_parse_header(block, &out);
    assert(st == BYOK_TAR_HDR_BAD_MAGIC);
}

static void test_bad_checksum(void)
{
    uint8_t block[512];
    build_header(block, "BYOK.bin", 100, '0');
    /* Corrupt a content byte (not touching magic) after the checksum was
     * already computed and written -- the stored checksum no longer
     * matches the block's actual byte sum. */
    block[OFF_NAME] = (uint8_t)(block[OFF_NAME] ^ 0xFF);

    byok_tar_header_t out;
    byok_tar_hdr_status_t st = byok_tar_parse_header(block, &out);
    assert(st == BYOK_TAR_HDR_BAD_CHECKSUM);
}

static void test_bad_size(void)
{
    uint8_t block[512];
    build_header(block, "BYOK.bin", 100, '0');
    /* Overwrite the size field with non-octal ASCII, then recompute and
     * rewrite the checksum over the corrupted block so checksum
     * verification passes and the failure is isolated to size parsing. */
    memcpy(block + OFF_SIZE, "NOTANUMBER!!", LEN_SIZE);
    memset(block + OFF_CHKSUM, ' ', LEN_CHKSUM);
    char chk[9];
    snprintf(chk, sizeof(chk), "%06o", header_checksum(block));
    memcpy(block + OFF_CHKSUM, chk, 6);
    block[OFF_CHKSUM + 6] = '\0';
    block[OFF_CHKSUM + 7] = ' ';

    byok_tar_header_t out;
    byok_tar_hdr_status_t st = byok_tar_parse_header(block, &out);
    assert(st == BYOK_TAR_HDR_BAD_SIZE);
}

static void test_size_to_blocks(void)
{
    assert(byok_tar_size_to_blocks(0) == 0);
    assert(byok_tar_size_to_blocks(1) == 1);
    assert(byok_tar_size_to_blocks(511) == 1);
    assert(byok_tar_size_to_blocks(512) == 1);
    assert(byok_tar_size_to_blocks(513) == 2);
    assert(byok_tar_size_to_blocks(1024) == 2);
    assert(byok_tar_size_to_blocks(1025) == 3);
}

/* Regression for the overflow finding in the PoC review: a checksum-valid
 * ustar header can claim a huge size (up to 12 ASCII-octal digits). This
 * builds one with size = 077777777777 octal (11 sevens -- the max an
 * 11-digit octal field written by write_octal_field's "%0*llo" over
 * LEN_SIZE-1=11 chars can hold) = 8589934591 decimal (~8 GiB), and checks
 * that:
 *   - byok_tar_parse_header() parses it correctly (this layer has no size
 *     policy of its own -- see byok_tar.h's contract comment on
 *     byok_tar_size_to_blocks());
 *   - byok_tar_size_to_blocks() computes the mathematically correct block
 *     count without silently wrapping (built and run under UBSan/ASan --
 *     tests/tar/Makefile -- so any actual signed-overflow UB in this
 *     library would itself abort the test binary);
 *   - the value clears components/byok_sd_updater's own
 *     MAX_MEMBER_SIZE_BYTES (16 MiB) by a wide margin, which is the caller-
 *     side guard that actually prevents the overflowing
 *     `blocks * BYOK_TAR_BLOCK_SIZE` seek-offset multiply this finding was
 *     about -- byok_tar itself is a pure parser and rejects nothing here by
 *     design. */
static void test_huge_size_no_overflow(void)
{
    uint8_t block[512];
    build_header(block, "BYOK.bin", 0, '0'); /* placeholder size, overwritten below */

    /* Write the field directly from the ASCII octal literal the finding
     * names, rather than round-tripping through write_octal_field's own
     * uint64_t encoder, so this test does not simply check the encoder
     * against itself. */
    const char *size_field = "77777777777"; /* 11 octal 7s, NUL-padded to 12 */
    memset(block + OFF_SIZE, 0, LEN_SIZE);
    memcpy(block + OFF_SIZE, size_field, strlen(size_field));
    memset(block + OFF_CHKSUM, ' ', LEN_CHKSUM);
    char chk[9];
    snprintf(chk, sizeof(chk), "%06o", header_checksum(block));
    memcpy(block + OFF_CHKSUM, chk, 6);
    block[OFF_CHKSUM + 6] = '\0';
    block[OFF_CHKSUM + 7] = ' ';

    byok_tar_header_t out;
    byok_tar_hdr_status_t st = byok_tar_parse_header(block, &out);
    assert(st == BYOK_TAR_HDR_OK);

    const uint64_t expected_size = 8589934591ULL; /* 077777777777 octal */
    assert(out.size == expected_size);

    uint32_t blocks = byok_tar_size_to_blocks(out.size);
    const uint32_t expected_blocks = 16777216u; /* ceil(8589934591 / 512) */
    assert(blocks == expected_blocks);

    /* The property this whole finding is about: blocks * 512 must equal the
     * true (padded) byte length -- computed here in uint64_t, the type any
     * caller MUST use for this multiply (byok_tar.h's contract comment) --
     * and that true length is far past any sane single-member cap. */
    uint64_t padded_bytes = (uint64_t)blocks * BYOK_TAR_BLOCK_SIZE;
    assert(padded_bytes == 8589934592ULL);
    assert(padded_bytes > (16u * 1024u * 1024u)); /* clears the 16 MiB caller-side cap */
}

/* Walks a hand-built two-member archive (BYOK.bin + assets.tar, the D-015
 * shape) exactly the way components/byok_sd_updater consumes a real one:
 * header, data blocks (skipped/read in BYOK_TAR_BLOCK_SIZE units), repeat,
 * stop at two consecutive end markers. */
static void test_full_archive_walk(void)
{
    const char *bin_payload = "fake-esp-image-bytes"; /* content is irrelevant to */
    const char *assets_payload = "fake-assets-tar-payload!"; /* the tar layer itself   */
    size_t bin_len = strlen(bin_payload);
    size_t assets_len = strlen(assets_payload);

    uint8_t archive[512 * 8];
    memset(archive, 0, sizeof(archive));
    size_t off = 0;

    uint8_t hdr1[512];
    build_header(hdr1, "BYOK.bin", bin_len, '0');
    memcpy(archive + off, hdr1, 512);
    off += 512;
    memcpy(archive + off, bin_payload, bin_len);
    off += byok_tar_size_to_blocks(bin_len) * 512;

    uint8_t hdr2[512];
    build_header(hdr2, "assets.tar", assets_len, '0');
    memcpy(archive + off, hdr2, 512);
    off += 512;
    memcpy(archive + off, assets_payload, assets_len);
    off += byok_tar_size_to_blocks(assets_len) * 512;

    /* Two zero blocks already present from the initial memset -- `off` now
     * points at them. */
    assert(off + 1024 <= sizeof(archive));

    bool found_bin = false, found_assets = false;
    uint64_t bin_size_seen = 0;
    size_t pos = 0;
    int zero_streak = 0;
    int members_seen = 0;
    while (pos + 512 <= sizeof(archive)) {
        byok_tar_header_t hdr;
        byok_tar_hdr_status_t st = byok_tar_parse_header(archive + pos, &hdr);
        pos += 512;
        if (st == BYOK_TAR_HDR_END) {
            if (++zero_streak >= 2) {
                break;
            }
            continue;
        }
        assert(st == BYOK_TAR_HDR_OK);
        zero_streak = 0;
        members_seen++;
        if (strcmp(hdr.name, "BYOK.bin") == 0) {
            found_bin = true;
            bin_size_seen = hdr.size;
        } else if (strcmp(hdr.name, "assets.tar") == 0) {
            found_assets = true;
        }
        pos += (size_t)byok_tar_size_to_blocks(hdr.size) * 512;
    }

    assert(members_seen == 2);
    assert(found_bin);
    assert(found_assets);
    assert(bin_size_seen == bin_len);
}

int main(void)
{
    test_valid_header();
    test_valid_header_typeflag_nul();
    test_max_length_name();
    test_end_marker();
    test_bad_magic();
    test_bad_checksum();
    test_bad_size();
    test_size_to_blocks();
    test_huge_size_no_overflow();
    test_full_archive_walk();

    printf("test_tar: all tests passed\n");
    return 0;
}
