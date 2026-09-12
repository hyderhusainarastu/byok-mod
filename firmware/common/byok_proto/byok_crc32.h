/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_crc32.h — table-driven IEEE 802.3 / zlib CRC-32
 * ============================================================================
 *
 * Reflected polynomial 0xEDB88320, initial value 0xFFFFFFFF, input and output
 * reflected, final XOR 0xFFFFFFFF. Byte-for-byte identical to CPython's
 * `zlib.crc32(data) & 0xFFFFFFFF`. This is the exact CRC required by
 * docs/protocol.md §3.1 for the BYOK Link frame trailer.
 *
 * Builds standalone with plain clang (no ESP-IDF headers) and as an
 * idf_component_register()'d component — see CMakeLists.txt.
 */
#ifndef BYOK_CRC32_H
#define BYOK_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot CRC32 over a whole buffer. Returns the finished (post-final-XOR)
 * value — i.e. exactly what docs/protocol.md §3.1 and §12 call CRC32. */
uint32_t byok_crc32(const uint8_t *data, size_t len);

/* Incremental form, for computing a CRC across pieces that arrive
 * separately (e.g. header then payload) without concatenating them into one
 * buffer first.
 *
 *   uint32_t crc = byok_crc32_init();
 *   crc = byok_crc32_update(crc, header, 9);
 *   crc = byok_crc32_update(crc, payload, len);
 *   crc = byok_crc32_finish(crc);
 *
 * byok_crc32(data, len) is exactly
 * byok_crc32_finish(byok_crc32_update(byok_crc32_init(), data, len)).
 */
uint32_t byok_crc32_init(void);
uint32_t byok_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t byok_crc32_finish(uint32_t crc);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_CRC32_H */
