/* SPDX-License-Identifier: MIT
 * byok_font8x8.h -- BYOK placeholder font
 * ============================================================================
 * ORIGINAL, hand-authored 8x8 bitmap font (5 active columns + 3px inter-glyph
 * spacing, 7 active rows + 1px inter-line spacing). It is NOT reverse-engineered
 * from the vendor firmware and is NOT copied from any third-party font table --
 * it exists only so app_main can draw "BYOK LAB" at boot and so DRAW_TEXT has
 * something to render before a real font (or the vendor .byf SD-font loader,
 * BYOK_FONT_DIR in hw_config.h) is implemented. Treat it as a placeholder -- see
 * firmware/s3/README.md "guesses" list.
 *
 * Coverage: space, A-Z, 0-9, and . , : - _ ! ? / ' (47 glyphs). Any other
 * codepoint falls back to a filled box per protocol.md 6.2 DRAW_TEXT.
 *
 * Row byte format: bit7 = leftmost column, bit0 = rightmost column (matches the
 * link protocol's 1bpp packing in docs/protocol.md 7.2, MSB = leftmost).
 * ============================================================================
 */
#ifndef BYOK_FONT8X8_H
#define BYOK_FONT8X8_H

#include <stdint.h>

#define BYOK_FONT8X8_GLYPH_W   8u   /* advance width, px  (5 active + 3 spacing) */
#define BYOK_FONT8X8_GLYPH_H   8u   /* cell height, px    (7 active + 1 spacing) */
/* Fallback glyph for any codepoint not in the table: a filled box, per
 * docs/protocol.md 6.2 ("Glyphs outside the built-in font render as U+FFFD or a
 * filled box; the device never fails a frame over an unmappable codepoint"). */
static const uint8_t byok_font8x8_fallback[8] = {
    0xF8, 0xF8, 0xF8, 0xF8, 0xF8, 0xF8, 0xF8, 0x00,
};

static const uint8_t byok_font8x8_space[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t byok_font8x8_A[8] = { 0x20, 0x50, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x00 };
static const uint8_t byok_font8x8_B[8] = { 0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00 };
static const uint8_t byok_font8x8_C[8] = { 0x78, 0x80, 0x80, 0x80, 0x80, 0x80, 0x78, 0x00 };
static const uint8_t byok_font8x8_D[8] = { 0xF0, 0x88, 0x88, 0x88, 0x88, 0x88, 0xF0, 0x00 };
static const uint8_t byok_font8x8_E[8] = { 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8, 0x00 };
static const uint8_t byok_font8x8_F[8] = { 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0x80, 0x00 };
static const uint8_t byok_font8x8_G[8] = { 0x78, 0x80, 0x80, 0xB8, 0x88, 0x88, 0x78, 0x00 };
static const uint8_t byok_font8x8_H[8] = { 0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00 };
static const uint8_t byok_font8x8_I[8] = { 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0xF8, 0x00 };
static const uint8_t byok_font8x8_J[8] = { 0x38, 0x10, 0x10, 0x10, 0x10, 0x90, 0x60, 0x00 };
static const uint8_t byok_font8x8_K[8] = { 0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x88, 0x00 };
static const uint8_t byok_font8x8_L[8] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8, 0x00 };
static const uint8_t byok_font8x8_M[8] = { 0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x88, 0x00 };
static const uint8_t byok_font8x8_N[8] = { 0x88, 0xC8, 0xA8, 0xA8, 0x98, 0x88, 0x88, 0x00 };
static const uint8_t byok_font8x8_O[8] = { 0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00 };
static const uint8_t byok_font8x8_P[8] = { 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80, 0x00 };
static const uint8_t byok_font8x8_Q[8] = { 0x70, 0x88, 0x88, 0x88, 0xA8, 0x90, 0x68, 0x00 };
static const uint8_t byok_font8x8_R[8] = { 0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88, 0x00 };
static const uint8_t byok_font8x8_S[8] = { 0x78, 0x80, 0x80, 0x70, 0x08, 0x08, 0xF0, 0x00 };
static const uint8_t byok_font8x8_T[8] = { 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00 };
static const uint8_t byok_font8x8_U[8] = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00 };
static const uint8_t byok_font8x8_V[8] = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x50, 0x20, 0x00 };
static const uint8_t byok_font8x8_W[8] = { 0x88, 0x88, 0x88, 0xA8, 0xA8, 0xD8, 0x88, 0x00 };
static const uint8_t byok_font8x8_X[8] = { 0x88, 0x88, 0x50, 0x20, 0x50, 0x88, 0x88, 0x00 };
static const uint8_t byok_font8x8_Y[8] = { 0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x20, 0x00 };
static const uint8_t byok_font8x8_Z[8] = { 0xF8, 0x08, 0x10, 0x20, 0x40, 0x80, 0xF8, 0x00 };
static const uint8_t byok_font8x8_0[8] = { 0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70, 0x00 };
static const uint8_t byok_font8x8_1[8] = { 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 };
static const uint8_t byok_font8x8_2[8] = { 0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8, 0x00 };
static const uint8_t byok_font8x8_3[8] = { 0x70, 0x88, 0x08, 0x30, 0x08, 0x88, 0x70, 0x00 };
static const uint8_t byok_font8x8_4[8] = { 0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10, 0x00 };
static const uint8_t byok_font8x8_5[8] = { 0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70, 0x00 };
static const uint8_t byok_font8x8_6[8] = { 0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70, 0x00 };
static const uint8_t byok_font8x8_7[8] = { 0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40, 0x00 };
static const uint8_t byok_font8x8_8[8] = { 0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00 };
static const uint8_t byok_font8x8_9[8] = { 0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60, 0x00 };
static const uint8_t byok_font8x8_dot[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x60, 0x00 };
static const uint8_t byok_font8x8_comma[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40, 0x00 };
static const uint8_t byok_font8x8_colon[8] = { 0x00, 0x60, 0x60, 0x00, 0x60, 0x60, 0x00, 0x00 };
static const uint8_t byok_font8x8_dash[8] = { 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t byok_font8x8_underscore[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00 };
static const uint8_t byok_font8x8_bang[8] = { 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00 };
static const uint8_t byok_font8x8_quest[8] = { 0x70, 0x88, 0x08, 0x30, 0x20, 0x00, 0x20, 0x00 };
static const uint8_t byok_font8x8_slash[8] = { 0x08, 0x10, 0x10, 0x20, 0x40, 0x40, 0x80, 0x00 };
static const uint8_t byok_font8x8_apos[8] = { 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* Returns an 8-byte glyph bitmap for `codepoint` (ASCII only -- UTF-8 callers
 * must decode first), or byok_font8x8_fallback if not covered. */
static inline const uint8_t *byok_font8x8_lookup(uint32_t codepoint)
{
    switch (codepoint) {
    case ' ': return byok_font8x8_space;
    case 'A': return byok_font8x8_A;
    case 'B': return byok_font8x8_B;
    case 'C': return byok_font8x8_C;
    case 'D': return byok_font8x8_D;
    case 'E': return byok_font8x8_E;
    case 'F': return byok_font8x8_F;
    case 'G': return byok_font8x8_G;
    case 'H': return byok_font8x8_H;
    case 'I': return byok_font8x8_I;
    case 'J': return byok_font8x8_J;
    case 'K': return byok_font8x8_K;
    case 'L': return byok_font8x8_L;
    case 'M': return byok_font8x8_M;
    case 'N': return byok_font8x8_N;
    case 'O': return byok_font8x8_O;
    case 'P': return byok_font8x8_P;
    case 'Q': return byok_font8x8_Q;
    case 'R': return byok_font8x8_R;
    case 'S': return byok_font8x8_S;
    case 'T': return byok_font8x8_T;
    case 'U': return byok_font8x8_U;
    case 'V': return byok_font8x8_V;
    case 'W': return byok_font8x8_W;
    case 'X': return byok_font8x8_X;
    case 'Y': return byok_font8x8_Y;
    case 'Z': return byok_font8x8_Z;
    case '0': return byok_font8x8_0;
    case '1': return byok_font8x8_1;
    case '2': return byok_font8x8_2;
    case '3': return byok_font8x8_3;
    case '4': return byok_font8x8_4;
    case '5': return byok_font8x8_5;
    case '6': return byok_font8x8_6;
    case '7': return byok_font8x8_7;
    case '8': return byok_font8x8_8;
    case '9': return byok_font8x8_9;
    case '.': return byok_font8x8_dot;
    case ',': return byok_font8x8_comma;
    case ':': return byok_font8x8_colon;
    case '-': return byok_font8x8_dash;
    case '_': return byok_font8x8_underscore;
    case '!': return byok_font8x8_bang;
    case '?': return byok_font8x8_quest;
    case '/': return byok_font8x8_slash;
    case '\'': return byok_font8x8_apos;
    default: return byok_font8x8_fallback;
    }
}

#endif /* BYOK_FONT8X8_H */
