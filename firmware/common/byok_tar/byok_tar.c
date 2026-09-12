/* SPDX-License-Identifier: MIT */
#include "byok_tar.h"

#include <string.h>

/* POSIX ustar header field offsets (all within the 512-byte block). */
#define OFF_NAME     0
#define LEN_NAME     100
#define OFF_SIZE     124
#define LEN_SIZE     12
#define OFF_CHKSUM   148
#define LEN_CHKSUM   8
#define OFF_TYPEFLAG 156
#define OFF_MAGIC    257
#define LEN_MAGIC    5 /* "ustar", ignoring the NUL/version byte that follows */

static bool is_all_zero(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Parses an ASCII octal field (optionally space/NUL padded on either side)
 * into a uint64_t. Returns false if the field contains anything other than
 * octal digits, spaces, and a single trailing NUL/space run. */
static bool parse_octal(const uint8_t *field, size_t len, uint64_t *out)
{
    size_t i = 0;
    while (i < len && field[i] == ' ') {
        i++; /* skip leading spaces */
    }
    uint64_t value = 0;
    bool saw_digit = false;
    for (; i < len; i++) {
        uint8_t c = field[i];
        if (c == '\0' || c == ' ') {
            break; /* terminator reached */
        }
        if (c < '0' || c > '7') {
            return false;
        }
        value = (value << 3) + (uint64_t)(c - '0');
        saw_digit = true;
    }
    /* Whatever remains (if anything) must be NUL/space padding only. */
    for (; i < len; i++) {
        if (field[i] != '\0' && field[i] != ' ') {
            return false;
        }
    }
    if (!saw_digit) {
        return false; /* an all-blank field is not a valid size */
    }
    *out = value;
    return true;
}

byok_tar_hdr_status_t byok_tar_parse_header(const uint8_t block[BYOK_TAR_BLOCK_SIZE],
                                             byok_tar_header_t *out)
{
    if (is_all_zero(block, BYOK_TAR_BLOCK_SIZE)) {
        return BYOK_TAR_HDR_END;
    }

    if (memcmp(block + OFF_MAGIC, "ustar", LEN_MAGIC) != 0) {
        return BYOK_TAR_HDR_BAD_MAGIC;
    }

    /* Checksum: unsigned byte sum of the whole 512-byte header with the
     * checksum field itself treated as 8 ASCII spaces (0x20) — the POSIX
     * ustar algorithm, and what both GNU tar and bsdtar write. */
    uint64_t chksum_field;
    if (!parse_octal(block + OFF_CHKSUM, LEN_CHKSUM, &chksum_field)) {
        return BYOK_TAR_HDR_BAD_CHECKSUM;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < BYOK_TAR_BLOCK_SIZE; i++) {
        if (i >= OFF_CHKSUM && i < OFF_CHKSUM + LEN_CHKSUM) {
            sum += (uint32_t)' ';
        } else {
            sum += block[i];
        }
    }
    if (sum != (uint32_t)chksum_field) {
        return BYOK_TAR_HDR_BAD_CHECKSUM;
    }

    uint64_t size;
    if (!parse_octal(block + OFF_SIZE, LEN_SIZE, &size)) {
        return BYOK_TAR_HDR_BAD_SIZE;
    }

    byok_tar_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.name, block + OFF_NAME, LEN_NAME);
    /* The name field may or may not be NUL-terminated within its 100 bytes
     * (a name using all 100 bytes has no terminator); forcing [100] to NUL
     * guarantees hdr.name is always a valid C string either way — if an
     * embedded NUL appears earlier, that's already where a string reader
     * stops. */
    hdr.name[BYOK_TAR_NAME_MAX] = '\0';
    hdr.size = size;
    hdr.typeflag = (char)block[OFF_TYPEFLAG];

    *out = hdr;
    return BYOK_TAR_HDR_OK;
}

uint32_t byok_tar_size_to_blocks(uint64_t size)
{
    return (uint32_t)((size + (BYOK_TAR_BLOCK_SIZE - 1)) / BYOK_TAR_BLOCK_SIZE);
}
