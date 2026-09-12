/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_note.h — device-local STATIC NOTE storage + rendering
 *               (byok_modes BYOK_MODE_STATIC_NOTE)
 * ============================================================================
 *
 * Owns the text behind the protocol's SET_NOTE (docs/protocol.md Sec.6.4,
 * v1.1) and byok_modes.h's BYOK_MODE_STATIC_NOTE: an in-RAM copy of up to
 * BYOK_NVS_NOTE_MAX_LEN (200) bytes of UTF-8/ASCII, optionally mirrored to
 * this firmware's own NVS namespace ("byokmod", see byok_nvs.h) when
 * CONFIG_BYOK_NVS_PERSIST_ARMED is on, plus the word-wrap/draw logic that
 * puts it on the panel.
 *
 * Rendering geometry is derived from the ACTUAL panel/font this firmware
 * has -- BYOK_DISPLAY_W/H (byok_display.h) and BYOK_FONT8X8_GLYPH_W/H
 * (byok_font8x8.h) -- giving 240/8 = 30 columns x 80/8 = 10 rows, not a
 * hard-coded figure. (An early spec for this feature said
 * "40 cols x up to 9 rows"; that number is this project's OWN vendor-
 * evidence text-mode figure for the vendor's *6-pixel-wide* default font,
 * hw_config.h Sec.3 BYOK_LCD_TEXT_COLS_DEFAULT/_ROWS_DEFAULT -- correct for
 * that different font, but 40 glyphs at THIS firmware's actual 8 px advance
 * would be 320 px wide against a 240 px panel. Deriving columns/rows from
 * this driver's own real constants instead follows the same rule
 * HELLO_ACK's own doc comment states for the protocol generally -- "the
 * host must take geometry from here and never hard-code it" -- applied
 * on-device to our own font instead of a borrowed, mismatched one.)
 *
 * Text coverage is whatever byok_font8x8.h covers (space, A-Z, 0-9, and a
 * handful of punctuation -- no lowercase) -- anything else renders as the
 * font's existing filled-box fallback glyph, exactly DRAW_TEXT's own
 * documented behaviour (docs/protocol.md Sec.6.2); this component does not
 * uppercase or otherwise rewrite the stored text, it only decides where
 * each byte lands on the grid. One extra convenience beyond the strict
 * spec: a literal '\n' (0x0A) byte in the stored text forces a line break
 * at that point (consuming the byte, never drawn -- the font has no glyph
 * for it and it would otherwise show as a fallback box) in addition to the
 * greedy word-wrap-at-the-column-edge this component does for everything
 * else. Text beyond BYOK_NOTE_MAX_ROWS is silently dropped (logged once at
 * render time), matching DRAW_TEXT's own "never fails a frame" posture.
 * ============================================================================
 */
#ifndef BYOK_NOTE_H
#define BYOK_NOTE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Loads the persisted note (if CONFIG_BYOK_NVS_PERSIST_ARMED and one was
 * ever saved) into the in-RAM buffer, or a default placeholder string
 * otherwise ("NO NOTE SET"). Does NOT draw anything and does NOT register a
 * byok_modes callback itself -- byok_idle.c owns the single BYOK_MODE_
 * STATIC_NOTE-entry callback registration and calls byok_note_render() from
 * it (see byok_idle.h's own header comment for why one component owns the
 * callback for all three idle submodes). Call once from app_main, after
 * byok_nvs_init() and before byok_idle_init(). Safe to call more than once
 * (a second call is a no-op). */
void byok_note_init(void);

/** SET_NOTE handler body: validates `len` (<= BYOK_NVS_NOTE_MAX_LEN, from
 * byok_nvs.h), replaces the in-RAM copy, persists it if armed, and -- if
 * BYOK_MODE_STATIC_NOTE is the CURRENT byok_modes state -- re-renders
 * immediately so a note pushed while it's already on-glass updates live
 * rather than waiting for the next mode re-entry. Returns ESP_ERR_INVALID_
 * ARG if `len` is out of range (the caller, app_main.c's dispatch(), should
 * NACK/E_BAD_LENGTH on that, exactly like every other SET_* handler's own
 * length check) or `utf8` is NULL with a nonzero `len`. `len == 0` is
 * legal (an empty note; byok_note_render() then shows a screen with no
 * text and nothing else changes on it). */
esp_err_t byok_note_set(const char *utf8, size_t len);

/** Copies the current in-RAM note into `out` (capacity `out_cap`), returning
 * the number of bytes copied (0 if `out_cap` is 0). Truncates rather than
 * failing if `out_cap` is smaller than the stored length -- there is no
 * protocol path that reads SET_NOTE's text back today, this getter exists
 * for logging/tests. */
size_t byok_note_get(char *out, size_t out_cap);

/** Draws the current in-RAM note (word-wrapped, see this header's own top
 * comment) into the back buffer and issues one byok_display_full_refresh().
 * A no-op (logged at DEBUG) if byok_display_is_ready() is false. Called by
 * byok_idle.c's mode-changed callback on BYOK_MODE_STATIC_NOTE entry, and
 * by byok_note_set() above for a live update while already showing. */
void byok_note_render(void);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_NOTE_H */
