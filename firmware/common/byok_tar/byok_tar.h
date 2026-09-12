/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_tar.h — minimal POSIX ustar header parser (flat names, typeflag '0')
 * ============================================================================
 *
 * Pure parsing of a single 512-byte ustar header block: no filesystem I/O,
 * no allocation, no dependency on ESP-IDF. This is the piece
 * components/byok_sd_updater (firmware/s3) streams through while reading
 * `/SDCARD/Updates/BYOK.tar` block-by-block off the SD card, and it is also
 * built standalone for the host test suite at tests/tar/ — see that
 * directory's Makefile.
 *
 * Scope, deliberately narrow: this project only ever produces or consumes
 * tars built by `scripts/make-update-tar.sh` (`tar --format ustar`, bare
 * member names, no "./" prefix, no PAX/GNU extended headers — see D-015 and
 * that script's own header comment). So this parser:
 *   - reads the `name` field as a flat, already-NUL-or-space-terminated
 *     string (does NOT implement the ustar `prefix` field for names >100
 *     bytes — an archive that needs it is rejected as BYOK_TAR_HDR_BAD_MAGIC
 *     only insofar as it wouldn't have a name we recognise; the header
 *     itself still parses, callers just won't match on a truncated name);
 *   - verifies the POSIX ustar magic and the header checksum (unsigned byte
 *     sum, the POSIX-standard algorithm and what GNU tar/bsdtar both write);
 *   - parses `size` as ASCII octal only (no GNU base-256 extension).
 *
 * A consumer walks the archive as: read 512 bytes, byok_tar_parse_header(),
 * branch on the returned status, then (for BYOK_TAR_HDR_OK) read/skip
 * byok_tar_size_to_blocks(out->size) * 512 bytes of file data before the
 * next header block. Two consecutive BYOK_TAR_HDR_END blocks is the
 * standard ustar end-of-archive marker.
 */
#ifndef BYOK_TAR_H_
#define BYOK_TAR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BYOK_TAR_BLOCK_SIZE 512u
#define BYOK_TAR_NAME_MAX   100u /* ustar `name` field width */

typedef enum {
    BYOK_TAR_HDR_OK = 0,       /* valid header; *out is filled in            */
    BYOK_TAR_HDR_END,          /* an all-zero block (end-of-archive marker) */
    BYOK_TAR_HDR_BAD_MAGIC,    /* not "ustar" at offset 257                 */
    BYOK_TAR_HDR_BAD_CHECKSUM, /* magic OK, header checksum does not match  */
    BYOK_TAR_HDR_BAD_SIZE,     /* `size` field is not valid ASCII octal     */
} byok_tar_hdr_status_t;

typedef struct {
    char     name[BYOK_TAR_NAME_MAX + 1]; /* NUL-terminated */
    uint64_t size;                        /* bytes of file data that follow */
    char     typeflag;                    /* '0' or '\0' = regular file     */
} byok_tar_header_t;

/** Parses one 512-byte ustar header block (`block` must point to exactly
 * BYOK_TAR_BLOCK_SIZE readable bytes). Returns the status; `out` is only
 * written on BYOK_TAR_HDR_OK. Pure function — no I/O, no globals, safe to
 * call from a host unit test. */
byok_tar_hdr_status_t byok_tar_parse_header(const uint8_t block[BYOK_TAR_BLOCK_SIZE],
                                             byok_tar_header_t *out);

/** Number of BYOK_TAR_BLOCK_SIZE blocks a file of `size` bytes occupies in
 * the archive once padded up to the next block boundary (0 -> 0).
 *
 * CONTRACT for callers that then compute a byte offset or seek amount from
 * this result (`blocks * BYOK_TAR_BLOCK_SIZE`): `size` itself is untrusted
 * input straight from a ustar header (byok_tar_parse_header() accepts up to
 * 12 ASCII-octal digits, i.e. `size` up to ~2^36), and this function does
 * NOT cap it -- it is a pure, allocation-free arithmetic helper with no
 * policy opinion on what a "reasonable" member size is. A caller MUST
 * reject `size` against a bound of its own choosing (the target it will
 * stream into is usually the natural one) BEFORE calling this and before
 * doing any `blocks * BYOK_TAR_BLOCK_SIZE` multiply, especially if that
 * multiply or the resulting seek offset uses a 32-bit type (e.g. `long` on
 * a 32-bit target) -- see components/byok_sd_updater/byok_sd_updater.c's
 * MAX_MEMBER_SIZE_BYTES for a worked example (16 MiB, chosen so the
 * multiply stays well inside `long` range). */
uint32_t byok_tar_size_to_blocks(uint64_t size);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_TAR_H_ */
