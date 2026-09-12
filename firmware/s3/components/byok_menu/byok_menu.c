/* SPDX-License-Identifier: MIT */
#include "byok_menu.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "byok_display.h"
#include "byok_font8x8.h" /* BYOK_FONT8X8_GLYPH_H */
#include "byok_modes.h"
#include "byok_nvs.h"      /* BYOK_PRESET_MAX_COUNT/_NAME_LEN, byok_nvs_get/set_presets/_preset_idx */

static const char *TAG = "byok_menu";

static char    s_names[BYOK_PRESET_MAX_COUNT][BYOK_PRESET_NAME_LEN];
static uint8_t s_count;
static uint8_t s_selected;   /* persisted/committed index -- STATUS reserved bits read this */
static uint8_t s_highlight;  /* cursor row while s_open, may differ from s_selected until _select() */

static bool             s_open;
static byok_app_mode_t  s_prev_mode;
static int64_t          s_deadline_us; /* only meaningful while s_open */

static bool s_inited;

static byok_menu_preset_changed_cb_t s_changed_cb;

/* ==========================================================================
 * Load / persist
 * ========================================================================== */

static void load_from_nvs(void)
{
    memset(s_names, 0, sizeof(s_names));
    s_count = 0;
    s_selected = 0;

    if (!byok_nvs_is_armed()) {
        return;
    }

    uint8_t blob[BYOK_NVS_PRESETS_BLOB_LEN];
    esp_err_t err = byok_nvs_get_presets(blob, sizeof(blob));
    if (err == ESP_OK) {
        uint8_t count = blob[0];
        if (count > BYOK_PRESET_MAX_COUNT) {
            ESP_LOGW(TAG, "persisted preset count %u exceeds %u -- clamping (stale/corrupt blob?)",
                     (unsigned)count, (unsigned)BYOK_PRESET_MAX_COUNT);
            count = BYOK_PRESET_MAX_COUNT;
        }
        s_count = count;
        memcpy(s_names, blob + 1, (size_t)s_count * BYOK_PRESET_NAME_LEN);
        ESP_LOGI(TAG, "loaded %u persisted preset(s)", (unsigned)s_count);
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "byok_nvs_get_presets: %s -- starting with an empty list", esp_err_to_name(err));
    }

    uint8_t idx;
    if (s_count > 0 && byok_nvs_get_preset_idx(&idx) == ESP_OK && idx < s_count) {
        s_selected = idx;
    }
}

static void persist_presets(void)
{
    if (!byok_nvs_is_armed()) {
        return;
    }
    uint8_t blob[BYOK_NVS_PRESETS_BLOB_LEN];
    blob[0] = s_count;
    memcpy(blob + 1, s_names, sizeof(s_names));
    esp_err_t err = byok_nvs_set_presets(blob);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_nvs_set_presets: %s (kept in RAM regardless)", esp_err_to_name(err));
    }
}

static void persist_selected(void)
{
    if (!byok_nvs_is_armed()) {
        return;
    }
    esp_err_t err = byok_nvs_set_preset_idx(s_selected);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_nvs_set_preset_idx: %s (kept in RAM regardless)", esp_err_to_name(err));
    }
}

/* ==========================================================================
 * Rendering
 * ========================================================================== */

static size_t name_draw_len(const char *name)
{
    size_t n = 0;
    while (n < BYOK_PRESET_NAME_LEN && name[n] != '\0') {
        n++;
    }
    return n;
}

static void render(void)
{
    if (!byok_display_is_ready()) {
        return;
    }
    byok_display_clear(false);
    for (uint8_t i = 0; i < s_count; i++) {
        uint16_t y = (uint16_t)(i * BYOK_FONT8X8_GLYPH_H);
        uint8_t style = 0;
        if (i == s_highlight) {
            /* Full-width filled bar, then the name drawn INVERTED on top of
             * it (style bit0, docs/protocol.md Sec.6.2 DRAW_TEXT) -- the
             * glyph's own "on" pixels come out light against the bar's
             * dark fill, giving a genuine inverted highlight row rather
             * than just inverted glyph cells on an otherwise light row. */
            byok_display_fill_rect(0, y, BYOK_DISPLAY_W, BYOK_FONT8X8_GLYPH_H,
                                    BYOK_RECT_OP_FILLED, true);
            style = 0x01;
        }
        size_t len = name_draw_len(s_names[i]);
        if (len > 0) {
            byok_display_draw_text(0, y, style, s_names[i], len);
        }
    }
    byok_display_full_refresh(false);
}

/* ==========================================================================
 * Open / close
 * ========================================================================== */

static void open_with_timeout(uint32_t timeout_ms)
{
    if (s_open || s_count == 0) {
        return; /* skipped when no presets are stored (docs/protocol.md
                 * Sec.6.4b); idempotent if already open */
    }
    s_prev_mode = byok_modes_get();
    s_highlight = s_selected;
    s_open = true;
    s_deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t err = byok_modes_set(BYOK_MODE_MENU);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_modes_set(MENU): %s", esp_err_to_name(err));
    }
    render();
    ESP_LOGI(TAG, "menu open (%u ms timeout, restoring %s on close)", (unsigned)timeout_ms,
             byok_modes_name(s_prev_mode));
}

void byok_menu_open_boot(void)
{
    open_with_timeout(BYOK_MENU_BOOT_TIMEOUT_MS);
}

void byok_menu_open_default(void)
{
    open_with_timeout(BYOK_MENU_TIMEOUT_MS);
}

static void close_menu(bool apply)
{
    if (!s_open) {
        return;
    }
    s_open = false;

    /* apply == true only from byok_menu_select() (an explicit EXECUTE
     * confirm) -- fires EVT_PRESET_CHANGED (docs/protocol.md Sec.6.5) on
     * every explicit selection, even a re-confirm of the already-selected
     * row (the host asked to be told "on selection", not "on change"); a
     * timeout close (apply == false, "10 s timeout keeps the current")
     * never persists or fires the callback. */
    if (apply) {
        bool changed = (s_highlight != s_selected);
        s_selected = s_highlight;
        if (changed) {
            persist_selected();
        }
        if (s_changed_cb != NULL) {
            s_changed_cb(s_selected);
        }
    }

    esp_err_t err = byok_modes_set(s_prev_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_modes_set(%s) on menu close: %s", byok_modes_name(s_prev_mode),
                 esp_err_to_name(err));
    }
    /* Reverting to BYOK_MODE_DASHBOARD_USB does not itself redraw anything
     * -- see this component's own header comment ("Display ownership while
     * open"): the host's own next DRAW_x/FRAME_x call + refresh is what actually
     * clears the menu's last frame off the glass. CLOCK/STATIC_NOTE/BLANK
     * all self-heal immediately instead, via byok_modes_set()'s own
     * change-callback firing (byok_clock.c/byok_idle.c's registered
     * callbacks re-render on entry). */
}

void byok_menu_abort(void)
{
    s_open = false;
}

void byok_menu_select(void)
{
    close_menu(true);
}

void byok_menu_move(int delta)
{
    if (!s_open || s_count == 0) {
        return;
    }
    int n = (int)s_count;
    /* 0.1.15 fix (CHANGELOG.md 0.1.14 Known Issue, commit b51c65c):
     * render() draws row i at y = i * glyph height, i.e. index 0 is the
     * TOP row and increasing index walks DOWN the panel. byok_idle.c's
     * up_short()/down_short() are unchanged -- UP still passes +1, DOWN
     * still passes -1 (that GPIO->UP/DOWN wiring is CONFIRMED correct and
     * shared by the media-transport table and the idle CLOCK/STATIC_NOTE/
     * BLANK cycling, neither of which is affected). Subtracting `delta`
     * (instead of adding it) reconciles the two: UP's +1 now walks the
     * highlight UP the list (index-1) and DOWN's -1 walks it DOWN
     * (index+1), matching the row order render() actually draws. */
    s_highlight = (uint8_t)((((int)s_highlight - delta) % n + n) % n);
    render();
}

bool byok_menu_is_open(void)
{
    return s_open;
}

bool byok_menu_has_presets(void)
{
    return s_count > 0;
}

uint8_t byok_menu_get_selected_index(void)
{
    return s_selected;
}

void byok_menu_set_preset_changed_cb(byok_menu_preset_changed_cb_t cb)
{
    s_changed_cb = cb;
}

esp_err_t byok_menu_set_presets(uint8_t count, const uint8_t *names)
{
    if (count > BYOK_PRESET_MAX_COUNT || (names == NULL && count > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_names, 0, sizeof(s_names));
    if (count > 0) {
        memcpy(s_names, names, (size_t)count * BYOK_PRESET_NAME_LEN);
    }
    s_count = count;

    if (s_selected >= s_count) {
        s_selected = 0;
    }
    if (s_highlight >= s_count) {
        s_highlight = s_selected;
    }

    persist_presets();

    if (s_open) {
        render();
    }
    ESP_LOGI(TAG, "SET_PRESETS: %u preset(s) loaded", (unsigned)s_count);
    return ESP_OK;
}

/* ==========================================================================
 * Timeout-watch task
 * ========================================================================== */

#define TICK_MS 200u

static void menu_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        if (s_open && esp_timer_get_time() >= s_deadline_us) {
            ESP_LOGI(TAG, "menu timeout -- keeping current selection (index %u)", (unsigned)s_selected);
            close_menu(false);
        }
    }
}

void byok_menu_init(void)
{
    if (s_inited) {
        return;
    }

    load_from_nvs();
    s_highlight = s_selected;

    BaseType_t created = xTaskCreate(menu_task, "byok_menu", 3072, NULL, 3, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(byok_menu) failed -- the menu's own auto-close timeout will "
                      "never fire (byok_menu_select() and an explicit SET_MODE away from it still "
                      "work; only the 5s/10s idle timeout is affected)");
        return;
    }
    s_inited = true;
}
