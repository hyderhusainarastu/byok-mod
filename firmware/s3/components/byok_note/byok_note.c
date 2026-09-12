/* SPDX-License-Identifier: MIT */
#include "byok_note.h"

#include <string.h>

#include "esp_log.h"

#include "byok_display.h"
#include "byok_font8x8.h" /* BYOK_FONT8X8_GLYPH_W/H */
#include "byok_modes.h"
#include "byok_nvs.h"

static const char *TAG = "byok_note";

/* Real panel/font geometry, not a borrowed/hard-coded figure -- see
 * byok_note.h's own header comment for why (BYOK_DISPLAY_W/H from
 * byok_display.h are themselves compile-time constants re-exported from
 * hw_config.h's confirmed measurements, same source HELLO_ACK's own
 * geometry fields use). */
#define NOTE_COLS ((uint16_t)(BYOK_DISPLAY_W / BYOK_FONT8X8_GLYPH_W)) /* 240/8 = 30 */
#define NOTE_ROWS ((uint16_t)(BYOK_DISPLAY_H / BYOK_FONT8X8_GLYPH_H)) /* 80/8  = 10 */

static const char k_default_note[] = "NO NOTE SET";

static char   s_note[BYOK_NVS_NOTE_MAX_LEN]; /* NOT NUL-terminated -- see s_note_len */
static size_t s_note_len;
static bool   s_inited;

void byok_note_init(void)
{
    if (s_inited) {
        return;
    }

    size_t len = 0;
    if (byok_nvs_is_armed()) {
        esp_err_t err = byok_nvs_get_note(s_note, sizeof(s_note), &len);
        if (err == ESP_OK) {
            s_note_len = len;
            ESP_LOGI(TAG, "loaded persisted note (%u bytes)", (unsigned)s_note_len);
            s_inited = true;
            return;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "byok_nvs_get_note: %s -- falling back to the default note",
                     esp_err_to_name(err));
        }
    }

    memcpy(s_note, k_default_note, sizeof(k_default_note) - 1);
    s_note_len = sizeof(k_default_note) - 1;
    s_inited = true;
}

esp_err_t byok_note_set(const char *utf8, size_t len)
{
    if (len > BYOK_NVS_NOTE_MAX_LEN || (utf8 == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len > 0) {
        memcpy(s_note, utf8, len);
    }
    s_note_len = len;

    if (byok_nvs_is_armed()) {
        esp_err_t err = byok_nvs_set_note(s_note, s_note_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "byok_nvs_set_note: %s (kept in RAM regardless)",
                     esp_err_to_name(err));
        }
    }

    if (byok_modes_get() == BYOK_MODE_STATIC_NOTE) {
        byok_note_render();
    }
    return ESP_OK;
}

size_t byok_note_get(char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return 0;
    }
    size_t n = (s_note_len < out_cap) ? s_note_len : out_cap;
    memcpy(out, s_note, n);
    return n;
}

/* Finds the end (exclusive) of the next line starting at s_note[start],
 * consuming at most NOTE_COLS bytes. `*resume_at` is set to the index to
 * resume scanning from for the FOLLOWING line -- always past exactly one
 * consumed delimiter byte when the line ended on one, so the caller never
 * needs its own "was that a delimiter" logic (a previous version of this
 * function pushed that decision to the caller and double-skipped a space
 * immediately following a '\n'-forced break; folding it in here removes
 * that whole class of off-by-one):
 *
 *   - A '\n' within the window ends the line there and is itself consumed
 *     (not drawn -- the font has no glyph for it); *resume_at = that index + 1.
 *   - Otherwise, if the window's end lands strictly inside a run of
 *     non-space bytes (i.e. the byte right after the window, if any, is
 *     not itself a space/newline -- meaning a word is being split), back
 *     up to the last space inside the window so whole words wrap, and
 *     consume that one space (not drawn); *resume_at = that space's index + 1.
 *     If no space exists inside the window at all (one word longer than
 *     NOTE_COLS), hard-break at the column edge instead (*resume_at ==
 *     the returned line end, nothing consumed) -- matching DRAW_TEXT's own
 *     "never fails, just clips/drops" posture rather than refusing to
 *     render an over-long token.
 *   - If the window's end already lands exactly at a word boundary (or at
 *     the text's own end), *resume_at == the returned line end, nothing
 *     consumed. */
static size_t note_line_end(size_t start, size_t *resume_at)
{
    size_t limit = start + NOTE_COLS;
    if (limit > s_note_len) {
        limit = s_note_len;
    }

    for (size_t i = start; i < limit; i++) {
        if (s_note[i] == '\n') {
            *resume_at = i + 1;
            return i;
        }
    }

    if (limit == s_note_len || s_note[limit] == ' ' || s_note[limit] == '\n') {
        *resume_at = limit;
        return limit;
    }

    size_t brk = limit;
    while (brk > start && s_note[brk - 1] != ' ') {
        brk--;
    }
    if (brk == start) {
        /* One "word" is longer than a whole line -- hard-break here. */
        *resume_at = limit;
        return limit;
    }
    *resume_at = brk; /* the space itself (at brk-1) is the one consumed */
    return brk - 1;
}

void byok_note_render(void)
{
    if (!byok_display_is_ready()) {
        ESP_LOGD(TAG, "byok_note_render: display not ready, skipping");
        return;
    }

    byok_display_clear(false);

    size_t   i = 0;
    uint16_t row = 0;
    bool     dropped = false;
    while (i < s_note_len) {
        if (row >= NOTE_ROWS) {
            dropped = true;
            break;
        }
        size_t resume_at = i;
        size_t line_end = note_line_end(i, &resume_at);
        size_t line_len = line_end - i;
        if (line_len > 0) {
            byok_display_draw_text(0, (uint16_t)(row * BYOK_FONT8X8_GLYPH_H), 0,
                                    s_note + i, line_len);
        }
        row++;
        i = resume_at; /* note_line_end() already accounts for exactly one
                         * consumed delimiter byte, if any -- see its own
                         * comment. */
    }
    if (dropped) {
        ESP_LOGW(TAG, "note text longer than %u rows at %u cols -- remainder dropped",
                 (unsigned)NOTE_ROWS, (unsigned)NOTE_COLS);
    }

    byok_display_full_refresh(false);
}
