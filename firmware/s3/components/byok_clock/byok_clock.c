/* SPDX-License-Identifier: MIT */
#include "byok_clock.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include "byok_battery.h"
#include "byok_display.h"
#include "byok_font8x8.h" /* byok_font8x8_lookup() -- public, include/ dir of byok_display */
#include "byok_hw_shim.h" /* BYOK_CHARGE_STATUS_* -- hw_config.h Sec.9 */
#include "byok_idle.h" /* 0.1.13: byok_idle_enter_selected_submode()/_restore_backlight() --
                         * see this file's own updated comments below for where/why */
#include "byok_modes.h"
#include "byok_rtc.h"

static const char *TAG = "byok_clock";

/* ==========================================================================
 * Layout -- 240x80 panel (BYOK_DISPLAY_W/H, byok_display.h). All three
 * lines are horizontally centred at their own scale; the three row bands
 * (time/date/battery) were picked to fit with a few px of margin all round,
 * not derived from any spec -- this is this component's own UI, there is
 * nothing to reverse-engineer here (unlike the panel geometry itself).
 * ========================================================================== */
#define TIME_SCALE   5u                                  /* glyph cell 40x40 px */
#define TIME_CHARS   5u                                  /* "HH:MM"             */
#define TIME_CELL_PX ((uint16_t)(BYOK_FONT8X8_GLYPH_W * TIME_SCALE)) /* 40 */
#define TIME_Y       4u
#define TIME_X0      ((uint16_t)((BYOK_DISPLAY_W - TIME_CHARS * TIME_CELL_PX) / 2u)) /* 20 */

#define DATE_SCALE   2u                                  /* glyph cell 16x16 px */
#define DATE_CHARS   10u                                 /* "YYYY-MM-DD"        */
#define DATE_CELL_PX ((uint16_t)(BYOK_FONT8X8_GLYPH_W * DATE_SCALE)) /* 16 */
#define DATE_Y       48u
#define DATE_X0      ((uint16_t)((BYOK_DISPLAY_W - DATE_CHARS * DATE_CELL_PX) / 2u)) /* 40 */

#define BATT_SCALE   1u                                  /* native 8x8 glyphs */
#define BATT_Y       68u
#define BATT_BUF_LEN 24u /* "BATT 087PCT CHRG" + slack, well under this */

/* Tick period for the background task. 1 Hz is cheap (one RTC I2C read +
 * a handful of string compares while in CLOCK mode; nothing at all while
 * in DASHBOARD_USB beyond one esp_timer_get_time() comparison) and is fine
 * granularity for both the idle-timeout check (30 s default, see this
 * component's Kconfig) and noticing a minute boundary promptly. */
#define TICK_MS 1000u

/* Hourly full refresh, expressed as "how many minute-ticks since the last
 * full refresh" rather than a wall-clock hour boundary -- simpler (no
 * calendar math needed here) and close enough to an hourly cadence that
 * its purpose (bound partial-refresh drift, re-baseline the diff state) is
 * met either way. Reset to 0 by every full render, including the one that
 * happens immediately on entering CLOCK. */
#define MINUTES_PER_FULL_REFRESH 60u

static bool     s_inited;
static uint32_t s_last_host_frame_ms; /* esp_timer ms, boot-relative */

/* Diff state for the per-minute partial-refresh path -- see
 * clock_render_minute_update()'s own comment. Empty strings are never a
 * real HH:MM/date, so they double as "nothing drawn yet" sentinels without
 * a separate bool. */
static char    s_last_time_str[TIME_CHARS + 1];
static char    s_last_date_str[DATE_CHARS + 1];
static uint8_t s_last_batt_pct = 0xFFu; /* byok_battery_get_pct()'s own range is 0-100 */
static uint8_t s_last_batt_chg = 0xFFu; /* BYOK_CHARGE_STATUS_* is 0-2 -- 0xFF is "never drawn" */
static unsigned s_minutes_since_full_refresh;
static bool    s_rtc_read_fail_logged; /* rate-limit the "can't read RTC" warning to once per outage */

/* ==========================================================================
 * Scaled glyph drawing -- reuses byok_display's existing public API
 * (byok_display_fill_rect(), BYOK_RECT_OP_FILLED) rather than adding a new
 * scaled-draw primitive to byok_display.c itself: nearest-neighbour scaling
 * is exactly "for every glyph bit that's on, fill a scale x scale block",
 * which is one fill_rect() call per set bit -- at most 8*5=40 calls for the
 * biggest (TIME_SCALE) cell, once a minute at most, on a component (I2C,
 * ~1 kHz-ish per byte at 400 kHz) that is not remotely latency-sensitive.
 * Erases its own cell to light FIRST (a second FILLED call over the full
 * cell) so this is idempotent and safe to call again over stale content --
 * both the full render and the per-minute partial-cell redraw use it. */
static void draw_scaled_char(uint16_t x, uint16_t y, uint8_t scale, char ch)
{
    uint16_t cell = (uint16_t)(BYOK_FONT8X8_GLYPH_W * scale); /* glyph is 8x8, so this is both w and h */
    byok_display_fill_rect(x, y, cell, cell, BYOK_RECT_OP_FILLED, false);

    const uint8_t *glyph = byok_font8x8_lookup((uint32_t)(unsigned char)ch);
    for (uint8_t row = 0; row < BYOK_FONT8X8_GLYPH_H; row++) {
        uint8_t bits = glyph[row];
        if (bits == 0) {
            continue;
        }
        for (uint8_t col = 0; col < BYOK_FONT8X8_GLYPH_W; col++) {
            if (((bits >> (7 - col)) & 1u) != 0) {
                byok_display_fill_rect((uint16_t)(x + col * scale), (uint16_t)(y + row * scale),
                                        scale, scale, BYOK_RECT_OP_FILLED, true);
            }
        }
    }
}

static void draw_scaled_string(uint16_t x, uint16_t y, uint8_t scale, const char *s, size_t len)
{
    uint16_t cx = x;
    uint16_t cell = (uint16_t)(BYOK_FONT8X8_GLYPH_W * scale);
    for (size_t i = 0; i < len; i++) {
        draw_scaled_char(cx, y, scale, s[i]);
        cx = (uint16_t)(cx + cell);
    }
}

/* ==========================================================================
 * Formatting. byok_font8x8 has no '%' glyph (see its own coverage comment)
 * -- "PCT"/"CHRG"/"FULL" spell it out with letters the font does have,
 * rather than falling back to the font's filled-box glyph for an
 * unmappable '%'.
 * ========================================================================== */
/* The `% 100u`/`% 10000u` on every field below are a real safety clamp, not
 * just a way to satisfy -Wformat-truncation (which they also do -- GCC's
 * range propagation can prove a modulo result fits its field width, where
 * it can't for a bare uint8_t/uint16_t struct field with the compiler's own
 * full nominal range): byok_rtc_read()'s BCD decode has no way to detect a
 * corrupted I2C read whose nibbles are individually valid hex but not valid
 * BCD digits (e.g. a nibble of 0xF is a legal 4 bits but not a BCD digit
 * 0-9), which can produce an hour/day up to 45 or a 2-digit year offset up
 * to 165 -- see byok_rtc_read()'s own register-width comments. Clamping
 * here guarantees format_time()/format_date() always write exactly
 * TIME_CHARS/DATE_CHARS bytes into a caller-owned fixed buffer, regardless
 * of what a glitched read produced upstream. */
static void format_time(const byok_rtc_time_t *t, char *out /* TIME_CHARS+1 */)
{
    snprintf(out, TIME_CHARS + 1, "%02u:%02u", (unsigned)(t->hour % 100u),
             (unsigned)(t->minute % 100u));
}

static void format_date(const byok_rtc_time_t *t, char *out /* DATE_CHARS+1 */)
{
    snprintf(out, DATE_CHARS + 1, "%04u-%02u-%02u", (unsigned)(t->year % 10000u),
             (unsigned)(t->month % 100u), (unsigned)(t->day % 100u));
}

static size_t format_battery(char *out, size_t out_len, uint8_t pct, uint8_t charging)
{
    int n;
    switch (charging) {
    case BYOK_CHARGE_STATUS_CHARGING:
        n = snprintf(out, out_len, "BATT %03uPCT CHRG", (unsigned)pct);
        break;
    case BYOK_CHARGE_STATUS_COMPLETE:
        n = snprintf(out, out_len, "BATT %03uPCT FULL", (unsigned)pct);
        break;
    case BYOK_CHARGE_STATUS_NONE:
    default:
        n = snprintf(out, out_len, "BATT %03uPCT", (unsigned)pct);
        break;
    }
    if (n <= 0) {
        return 0;
    }
    /* snprintf()'s return value is how many bytes it WOULD have written,
     * which can exceed out_len on truncation -- clamp so a caller passing
     * this straight to byok_display_draw_text()'s `len` never reads past
     * what was actually written into `out` (defensive; the fixed formats
     * above never actually reach this with BATT_BUF_LEN=24, but a future
     * edit to one of them growing past that should not become an
     * out-of-bounds read here). */
    return ((size_t)n < out_len) ? (size_t)n : out_len - 1;
}

/* ==========================================================================
 * Rendering
 * ========================================================================== */

/* Bounding-box accumulator for the partial-refresh path: every cell this
 * pass actually redrew widens the box; byok_display_partial_refresh() is
 * called once at the end over the union, rather than once per cell -- one
 * I2C window-program per tick instead of up to seven. */
typedef struct {
    bool     any;
    uint16_t x0, y0, x1, y1; /* inclusive */
} dirty_box_t;

static void dirty_box_add(dirty_box_t *b, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x1 = (uint16_t)(x + w - 1);
    uint16_t y1 = (uint16_t)(y + h - 1);
    if (!b->any) {
        b->x0 = x; b->y0 = y; b->x1 = x1; b->y1 = y1;
        b->any = true;
        return;
    }
    if (x  < b->x0) b->x0 = x;
    if (y  < b->y0) b->y0 = y;
    if (x1 > b->x1) b->x1 = x1;
    if (y1 > b->y1) b->y1 = y1;
}

static void draw_error_screen(const char *line)
{
    byok_display_clear(false);
    size_t len = strlen(line);
    uint16_t w = (uint16_t)(len * BYOK_FONT8X8_GLYPH_W);
    uint16_t x = (BYOK_DISPLAY_W > w) ? (uint16_t)((BYOK_DISPLAY_W - w) / 2) : 0;
    uint16_t y = (uint16_t)((BYOK_DISPLAY_H - BYOK_FONT8X8_GLYPH_H) / 2);
    byok_display_draw_text(x, y, 0, line, len);
    byok_display_full_refresh(false);
    s_last_time_str[0] = '\0';
    s_last_date_str[0] = '\0';
    s_last_batt_pct = 0xFFu;
    s_last_batt_chg = 0xFFu;
}

static void clock_render_full(const byok_rtc_time_t *t)
{
    char time_str[TIME_CHARS + 1];
    char date_str[DATE_CHARS + 1];
    char batt_str[BATT_BUF_LEN];
    format_time(t, time_str);
    format_date(t, date_str);
    uint8_t pct = byok_battery_get_pct();
    uint8_t chg = byok_battery_get_charging();
    size_t batt_len = format_battery(batt_str, sizeof(batt_str), pct, chg);
    uint16_t batt_w = (uint16_t)(batt_len * BYOK_FONT8X8_GLYPH_W);
    uint16_t batt_x = (BYOK_DISPLAY_W > batt_w) ? (uint16_t)((BYOK_DISPLAY_W - batt_w) / 2) : 0;

    byok_display_clear(false);
    draw_scaled_string(TIME_X0, TIME_Y, TIME_SCALE, time_str, TIME_CHARS);
    draw_scaled_string(DATE_X0, DATE_Y, DATE_SCALE, date_str, DATE_CHARS);
    byok_display_draw_text(batt_x, BATT_Y, 0, batt_str, batt_len);
    byok_display_full_refresh(false);

    memcpy(s_last_time_str, time_str, sizeof(time_str));
    memcpy(s_last_date_str, date_str, sizeof(date_str));
    s_last_batt_pct = pct;
    s_last_batt_chg = chg;
    s_minutes_since_full_refresh = 0;
}

/* Redraws only what changed since the last render (time digit cells, the
 * date line, the battery line) and issues ONE byok_display_partial_refresh()
 * over the union of their bounding boxes -- see this file's own header
 * comment ("Refresh cadence") for why this is once-a-minute-at-most, not a
 * per-second redraw. A no-op (no I2C at all) on a tick where literally
 * nothing changed, which cannot happen from the minute digits alone (the
 * minute always changes between two calls a tick period apart while the
 * caller only invokes this on an actual minute-boundary tick) but is a
 * real possibility for the date/battery portions and is handled correctly
 * (they simply do not widen the dirty box). */
static void clock_render_minute_update(const byok_rtc_time_t *t)
{
    char time_str[TIME_CHARS + 1];
    char date_str[DATE_CHARS + 1];
    format_time(t, time_str);
    format_date(t, date_str);

    dirty_box_t box = { 0 };

    for (unsigned i = 0; i < TIME_CHARS; i++) {
        if (time_str[i] != s_last_time_str[i]) {
            uint16_t x = (uint16_t)(TIME_X0 + i * TIME_CELL_PX);
            draw_scaled_char(x, TIME_Y, TIME_SCALE, time_str[i]);
            dirty_box_add(&box, x, TIME_Y, TIME_CELL_PX, TIME_CELL_PX);
        }
    }

    if (strcmp(date_str, s_last_date_str) != 0) {
        draw_scaled_string(DATE_X0, DATE_Y, DATE_SCALE, date_str, DATE_CHARS);
        dirty_box_add(&box, DATE_X0, DATE_Y, (uint16_t)(DATE_CHARS * DATE_CELL_PX), DATE_CELL_PX);
    }

    uint8_t pct = byok_battery_get_pct();
    uint8_t chg = byok_battery_get_charging();
    if (pct != s_last_batt_pct || chg != s_last_batt_chg) {
        char batt_str[BATT_BUF_LEN];
        size_t batt_len = format_battery(batt_str, sizeof(batt_str), pct, chg);
        uint16_t batt_w = (uint16_t)(batt_len * BYOK_FONT8X8_GLYPH_W);
        uint16_t batt_x = (BYOK_DISPLAY_W > batt_w) ? (uint16_t)((BYOK_DISPLAY_W - batt_w) / 2) : 0;
        /* Full battery-line width (not just batt_w) is cleared+redrawn so a
         * shorter new string (e.g. losing " CHRG") doesn't leave stale
         * glyphs from the longer previous string sitting to its right. */
        byok_display_fill_rect(0, BATT_Y, BYOK_DISPLAY_W, BYOK_FONT8X8_GLYPH_H,
                                BYOK_RECT_OP_FILLED, false);
        byok_display_draw_text(batt_x, BATT_Y, 0, batt_str, batt_len);
        dirty_box_add(&box, 0, BATT_Y, BYOK_DISPLAY_W, BYOK_FONT8X8_GLYPH_H);
        s_last_batt_pct = pct;
        s_last_batt_chg = chg;
    }

    if (box.any) {
        byok_display_partial_refresh(box.x0, box.y0,
                                      (uint16_t)(box.x1 - box.x0 + 1),
                                      (uint16_t)(box.y1 - box.y0 + 1));
    }

    memcpy(s_last_time_str, time_str, sizeof(time_str));
    memcpy(s_last_date_str, date_str, sizeof(date_str));
    s_minutes_since_full_refresh++;
}

/* ==========================================================================
 * byok_modes change callback + background task
 * ========================================================================== */

/* Known, accepted, benign race (not solved here -- same "accepted, not
 * solved" spirit as byok_power.h's own note on its display-call
 * interleaving): byok_modes_set() (byok_modes.c) has no lock of its own
 * around s_mode, so if a host frame arrives on dispatch_task RIGHT as this
 * callback starts rendering on clock_task (a ~30 s-idle-then-immediately-a-
 * frame window, narrow in practice), byok_clock_notify_host_frame() can
 * flip s_mode back to DASHBOARD_USB (and this function re-enters with
 * new_mode=DASHBOARD_USB, returns immediately via the guard below) WHILE
 * this call's own render is still in flight against its own already-
 * captured `t`/mode arguments -- the result is one stale CLOCK-screen
 * render landing on the glass after the mode already flipped back, until
 * the next host DRAW_* command (or clock_task's own next tick, which will
 * see the corrected mode and stop) overwrites it. Cosmetic only -- no data
 * race on the framebuffer itself (byok_display's own mutex still
 * serialises the actual I2C calls), just a one-frame-late UI update. */
static void clock_mode_changed(byok_app_mode_t new_mode, byok_app_mode_t old_mode, void *ctx)
{
    (void)ctx;
    if (new_mode != BYOK_MODE_CLOCK || old_mode == BYOK_MODE_CLOCK) {
        /* Leaving CLOCK (or a transition that doesn't involve it at all):
         * nothing to draw here. DASHBOARD_USB's own content is whatever the
         * host draws on its next frame (dispatch_task, app_main.c); this
         * component does not own or clear the screen on the way out. */
        return;
    }

    if (!byok_display_is_ready()) {
        ESP_LOGW(TAG, "entering CLOCK mode but the display never came up -- nothing to render");
        return;
    }

    ESP_LOGI(TAG, "entering CLOCK mode -- rendering now");
    byok_rtc_time_t t;
    esp_err_t err = byok_rtc_read(&t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_rtc_read failed on CLOCK entry: %s", esp_err_to_name(err));
        s_rtc_read_fail_logged = true; /* the periodic task's own read will re-log only on recovery */
        draw_error_screen("RTC READ FAIL");
        return;
    }
    s_rtc_read_fail_logged = false;
    if (t.voltage_low) {
        ESP_LOGW(TAG, "PCF8563 VL (voltage-low) flag set -- displayed time may be wrong "
                      "(RTC lost power since it was last set)");
    }
    clock_render_full(&t);
}

static void clock_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        byok_app_mode_t mode = byok_modes_get();
        if (mode == BYOK_MODE_DASHBOARD_USB) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if ((now_ms - s_last_host_frame_ms) >= (uint32_t)CONFIG_BYOK_CLOCK_IDLE_TIMEOUT_MS) {
                /* 0.1.13: whichever idle submode (CLOCK/STATIC_NOTE/BLANK)
                 * the owner last selected via the back buttons or an
                 * explicit SET_MODE 4/5/6 -- byok_idle.h -- not
                 * unconditionally CLOCK any more (docs/protocol.md Sec.6.4).
                 * If that lands on CLOCK, the effect is identical to the
                 * pre-0.1.13 byok_modes_set(BYOK_MODE_CLOCK) this replaced:
                 * clock_mode_changed() below still renders immediately via
                 * the byok_modes change-callback registry. */
                byok_idle_enter_selected_submode();
            }
            continue;
        }
        if (mode != BYOK_MODE_CLOCK) {
            continue; /* STATIC_NOTE/ORIGINAL: not this component's concern */
        }
        if (!byok_display_is_ready()) {
            continue; /* clock_mode_changed() already logged this once on entry */
        }

        byok_rtc_time_t t;
        esp_err_t err = byok_rtc_read(&t);
        if (err != ESP_OK) {
            if (!s_rtc_read_fail_logged) {
                ESP_LOGW(TAG, "byok_rtc_read failed: %s (keeping last screen up)",
                         esp_err_to_name(err));
                s_rtc_read_fail_logged = true;
            }
            continue; /* keep showing the last good render rather than blanking on one bad read */
        }
        s_rtc_read_fail_logged = false;

        char time_str[TIME_CHARS + 1];
        format_time(&t, time_str);
        if (strcmp(time_str, s_last_time_str) == 0) {
            continue; /* same minute as last render -- nothing to do */
        }

        if (s_minutes_since_full_refresh >= MINUTES_PER_FULL_REFRESH) {
            clock_render_full(&t);
        } else {
            clock_render_minute_update(&t);
        }
    }
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

void byok_clock_init(void)
{
    if (s_inited) {
        return;
    }

    esp_err_t rtc_err = byok_rtc_init();
    if (rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "byok_rtc_init failed: %s -- CLOCK mode will show an error screen "
                      "instead of the time until the RTC becomes readable", esp_err_to_name(rtc_err));
        /* Not fatal -- still start the task and register the callback below.
         * clock_mode_changed()/clock_task() both already handle a failing
         * byok_rtc_read() gracefully (draw_error_screen() / "keep last
         * screen up"), which covers "never came up at all" the same way as
         * "came up, then one read failed". */
    }

    s_last_host_frame_ms = (uint32_t)(esp_timer_get_time() / 1000);
    esp_err_t cb_err = byok_modes_register_change_cb(clock_mode_changed, NULL);
    if (cb_err != ESP_OK) {
        ESP_LOGE(TAG, "byok_modes_register_change_cb: %s -- CLOCK will never render on mode entry",
                 esp_err_to_name(cb_err));
    }

    /* Stack estimate follows byok_power_btn's own precedent (4096 B) for a
     * task whose deepest call chain is the same shape: this file's own
     * locals (a handful of small char[] buffers, all << the 1024 B static-
     * check-budget cap) -> byok_rtc_read()/byok_display_* -> the I2C master
     * driver, one bus transaction deep either way -- see byok_power.c's own
     * "Stack audit" comment on byok_power_btn for the reasoning this
     * mirrors. */
    BaseType_t created = xTaskCreate(clock_task, "byok_clock", 4096, NULL, 2, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(byok_clock) failed -- CLOCK mode disabled "
                      "(idle timeout will never fire, SET_MODE(IDLE) will still switch the "
                      "byok_app_mode_t but nothing will render or tick)");
        return;
    }
    s_inited = true;
}

void byok_clock_notify_host_frame(void)
{
    s_last_host_frame_ms = (uint32_t)(esp_timer_get_time() / 1000);
    /* 0.1.13: "any host frame switches back to DASHBOARD_USB" now applies to
     * all three device-autonomous idle submodes (CLOCK/STATIC_NOTE/BLANK),
     * not only CLOCK -- byok_idle.h introduced the other two, and
     * docs/protocol.md Sec.6.4 states the rule for all of them: the device
     * switches straight back to HOST "the instant any frame arrives (from
     * any of `CLOCK`/`STATIC_NOTE`/`BLANK`)". Leaving BLANK specifically also needs
     * its backlight restored first (BLANK forced it to 0 on entry,
     * byok_idle.c) -- CLOCK/STATIC_NOTE never zero it, so no restore is
     * needed for those two. */
    byok_app_mode_t mode = byok_modes_get();
    if (mode == BYOK_MODE_CLOCK || mode == BYOK_MODE_STATIC_NOTE || mode == BYOK_MODE_BLANK) {
        if (mode == BYOK_MODE_BLANK) {
            byok_idle_restore_backlight();
        }
        byok_modes_set(BYOK_MODE_DASHBOARD_USB);
    }
}
