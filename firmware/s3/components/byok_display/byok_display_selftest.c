/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_display_selftest.c — boot self-test pattern sequence
 * ============================================================================
 * Built entirely on byok_display's own public API (byok_display.h) -- no
 * access to the driver's internal state -- so this file could equally live
 * outside the component; it stays here because it is display-specific and
 * nothing else needs it. See byok_display_run_boot_selftest()'s own doc
 * comment in byok_display.h for the phase list and what gets logged.
 */
#include "byok_display.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include "byok_battery.h" /* battery_mv/pct/charging -- resting-screen "BAT" line only, see draw_border_and_banner() */
#include "byok_hw_shim.h" /* BYOK_LCD_I2C_ADDR_CMD/_DATA -- polarity experiment logging only, and
                            * BYOK_CHARGE_STATUS_* for the resting-screen "BAT" line below */

static const char *TAG = "byok_display_selftest";

#define PHASE_HOLD_MS 2000u

/* ==========================================================================
 * Phase drawing
 * ========================================================================== */

static void draw_checkerboard(void)
{
    byok_display_clear(false); /* false = light, CLEAR's own convention */
    const uint16_t cell = 8;
    for (uint16_t y = 0; y < BYOK_DISPLAY_H; y = (uint16_t)(y + cell)) {
        for (uint16_t x = 0; x < BYOK_DISPLAY_W; x = (uint16_t)(x + cell)) {
            bool dark = (((x / cell) + (y / cell)) % 2) == 0;
            byok_display_fill_rect(x, y, cell, cell, BYOK_RECT_OP_FILLED, dark);
        }
    }
}

/* byok_font8x8 (see byok_font8x8.h) only covers A-Z / 0-9 / a handful of
 * punctuation -- no lowercase letters -- so every string this self-test
 * draws is upper-cased first; a lowercase `fw_version` would otherwise
 * render as a row of fallback-glyph boxes instead of readable text. */
static void upper_copy(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (char)((c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : (char)c);
    }
    dst[i] = '\0';
}

/* "BAT 3.95V 78% CHG" -- own row (y=68), 10px below "PANEL NO ACK"'s row
 * (y=58; that row itself is only conditionally drawn, but this row's
 * position does not depend on whether it was, same as every other row on
 * this screen being at a fixed offset from the last). Added 0.1.11
 * alongside byok_battery (hw_config.h Sec.9): reads
 * the live EMA-filtered voltage/percent/charger-status getters at DRAW
 * time (not a value threaded in from app_main -- see this file's own
 * CMakeLists.txt comment on the byok_battery dependency), so this always
 * shows whatever byok_battery currently has, including its early-boot
 * synchronous fallback reading if the monitor task's first 2 s pass has
 * not completed yet (byok_battery_get_mv()'s own doc comment). Only drawn
 * when `show_battery` is true -- the EARLIER call to this same function
 * (self-test phase 2, ~6 s before the resting screen) intentionally omits
 * it: the battery line belongs on the resting screen only, not on every
 * border+banner draw in the sequence. */
static void draw_battery_line(void)
{
    uint16_t mv = byok_battery_get_mv();
    uint8_t pct = byok_battery_get_pct();
    uint8_t charging = byok_battery_get_charging();
    unsigned whole = mv / 1000u;
    unsigned frac = ((mv % 1000u) + 5u) / 10u; /* round to the nearest 0.01 V */
    if (frac >= 100u) {
        frac -= 100u;
        whole += 1u;
    }
    const char *suffix = "";
    if (charging == BYOK_CHARGE_STATUS_CHARGING) {
        suffix = " CHG";
    } else if (charging == BYOK_CHARGE_STATUS_COMPLETE) {
        suffix = " FULL";
    }
    char line[24];
    int n = snprintf(line, sizeof(line), "BAT %u.%02uV %u%%%s", whole, frac, (unsigned)pct, suffix);
    if (n < 0) {
        n = 0;
    } else if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    byok_display_draw_text(4, 68, 0, line, (size_t)n);
}

static void draw_border_and_banner(const char *fw_version, const char *reset_reason_str, bool show_battery)
{
    byok_display_clear(false);
    byok_display_fill_rect(0, 0, BYOK_DISPLAY_W, BYOK_DISPLAY_H, BYOK_RECT_OP_OUTLINE, true);

    static const char line1[] = "BYOK LAB";
    static const char line2[] = "240X80 1BPP";
    byok_display_draw_text(4, 8, 0, line1, strlen(line1));
    byok_display_draw_text(4, 18, 0, line2, strlen(line2));

    char ver_upper[16];
    upper_copy(ver_upper, sizeof(ver_upper), (fw_version != NULL) ? fw_version : "?");
    char ver_line[24];
    int n = snprintf(ver_line, sizeof(ver_line), "V:%s", ver_upper);
    if (n < 0) {
        n = 0;
    } else if ((size_t)n >= sizeof(ver_line)) {
        n = (int)sizeof(ver_line) - 1;
    }
    byok_display_draw_text(4, 28, 0, ver_line, (size_t)n);

    /* "RST:<reason>" -- docs/recovery.md Sec.4/Sec.8 item 2: the
     * on-glass half of the esp_reset_reason() diagnostic, the only channel
     * that still works when the USB-Serial-JTAG console itself is dead
     * (which is precisely the failure this line exists to help diagnose).
     * Own row (y=38), 10px below "V:...", so it never collides with that
     * line or with the 1px border. */
    char rst_upper[20];
    upper_copy(rst_upper, sizeof(rst_upper), (reset_reason_str != NULL) ? reset_reason_str : "?");
    char rst_line[24];
    int rn = snprintf(rst_line, sizeof(rst_line), "RST:%s", rst_upper);
    if (rn < 0) {
        rn = 0;
    } else if ((size_t)rn >= sizeof(rst_line)) {
        rn = (int)sizeof(rst_line) - 1;
    }
    byok_display_draw_text(4, 38, 0, rst_line, (size_t)rn);

    /* "USB: WAITING FOR HOST" -- own row (y=48), 10px below "RST:...",
     * consistent with every other row's spacing on this screen. Added
     * 2026-09-03 (device observation: the owner had no on-glass indication
     * that the resting screen was, in fact, still waiting for a host and
     * not just stuck). This line is static, not a live connection
     * indicator: nothing in this firmware re-draws it once a host actually
     * connects (see app_main.c's dispatch() -- CLEAR / DRAW_x / FRAME_x are
     * the only things that ever touch the display again after boot, and this
     * screen simply gets overdrawn by whatever the host sends first, same
     * as before this line existed). It is deliberately NOT redrawn/cleared
     * by HELLO -- see app_main.c's BYOK_TYPE_HELLO case. */
    static const char usb_line[] = "USB: WAITING FOR HOST";
    byok_display_draw_text(4, 48, 0, usb_line, strlen(usb_line));

    /* "PANEL NO ACK" -- 0.1.6, own row (y=58), 10px below "USB: WAITING FOR
     * HOST", same spacing as every other row on this screen. Diagnostic
     * only (docs/troubleshooting.md, byok_display.c's
     * lcd_ack_probe()): drawn when EITHER the pre-init or post-init ACK
     * probe found the panel not answering at 0x38 with ACK checking
     * actually enabled -- which is a real possibility this screen itself
     * cannot otherwise show, since every draw call on this screen goes out
     * over the same vendor-parity, ACK-check-DISABLED handles that report
     * ESP_OK whether or not the panel is listening. If this line is
     * visible at all, drawing worked (the glass answered enough I2C
     * traffic to render text) even though the probe reported no ACK --
     * that combination is itself useful diagnostic information, not a
     * contradiction; see lcd_ack_probe()'s own comment. Silently omitted
     * (blank row, nothing drawn) when both probes ACKed or neither ran. */
    if (byok_display_get_ack_probe_pre_init() == BYOK_ACK_PROBE_NO_ACK ||
        byok_display_get_ack_probe_post_init() == BYOK_ACK_PROBE_NO_ACK) {
        static const char ack_line[] = "PANEL NO ACK";
        byok_display_draw_text(4, 58, 0, ack_line, strlen(ack_line));
    }

    if (show_battery) {
        draw_battery_line();
    }
}

static void draw_line_test(void)
{
    byok_display_clear(false);
    /* Horizontal: top, mid, bottom -- exercises row addressing across the
     * full page range (row 0's page, a middle page, the last page). */
    byok_display_fill_rect(0, 0, BYOK_DISPLAY_W, 1, BYOK_RECT_OP_FILLED, true);
    byok_display_fill_rect(0, BYOK_DISPLAY_H / 2, BYOK_DISPLAY_W, 1, BYOK_RECT_OP_FILLED, true);
    byok_display_fill_rect(0, BYOK_DISPLAY_H - 1, BYOK_DISPLAY_W, 1, BYOK_RECT_OP_FILLED, true);
    /* Vertical: left, mid, right -- exercises column addressing across the
     * full window width (column 0, a middle column, the last column). */
    byok_display_fill_rect(0, 0, 1, BYOK_DISPLAY_H, BYOK_RECT_OP_FILLED, true);
    byok_display_fill_rect(BYOK_DISPLAY_W / 2, 0, 1, BYOK_DISPLAY_H, BYOK_RECT_OP_FILLED, true);
    byok_display_fill_rect(BYOK_DISPLAY_W - 1, 0, 1, BYOK_DISPLAY_H, BYOK_RECT_OP_FILLED, true);
}

static void draw_gate_message(bool gate_open)
{
    byok_display_clear(false);
    static const char header[] = "EXECUTE 3S HOLD:";
    byok_display_draw_text(4, 4, 0, header, strlen(header));

    /* style 0x02 = wrap at panel edge (byok_display.h) -- the message is
     * longer than one 30-char row at 240px/8px-per-glyph, so it wraps
     * across the rest of the panel. This mirrors check_boot_button()'s own
     * gating exactly: with the flag off (the PoC default) EXECUTE-hold
     * only LOGS what it would do (main/app_main.c); ON, it actually calls
     * esp_ota_set_boot_partition()+esp_restart(). Putting the live gate
     * state on the glass means the owner does not have to be watching the
     * serial log to know which behaviour a 3 s hold will produce right
     * now. */
    if (gate_open) {
        static const char msg[] = "RETURN-TO-STOCK IS ENABLED IN THIS BUILD";
        byok_display_draw_text(4, 16, 0x02, msg, strlen(msg));
    } else {
        static const char msg[] = "RETURN-TO-STOCK IS DISABLED IN THIS BUILD";
        byok_display_draw_text(4, 16, 0x02, msg, strlen(msg));
    }
}

/* 0.1.7 polarity experiment banner -- components/byok_display/Kconfig
 * BYOK_DISPLAY_POLARITY_EXPERIMENT, docs/display.md Sec.6. `label`
 * identifies which pass's mapping is live when this is drawn (pass C always
 * draws "A", per that section's verdict). */
static void draw_polarity_banner(const char *fw_version, const char *label)
{
    byok_display_clear(false);
    byok_display_fill_rect(0, 0, BYOK_DISPLAY_W, BYOK_DISPLAY_H, BYOK_RECT_OP_OUTLINE, true);

    char line1[24];
    int n = snprintf(line1, sizeof(line1), "POLARITY %s OK?", label);
    if (n < 0) {
        n = 0;
    } else if ((size_t)n >= sizeof(line1)) {
        n = (int)sizeof(line1) - 1;
    }
    byok_display_draw_text(4, 8, 0, line1, (size_t)n);

    char ver_upper[16];
    upper_copy(ver_upper, sizeof(ver_upper), (fw_version != NULL) ? fw_version : "?");
    char ver_line[24];
    int vn = snprintf(ver_line, sizeof(ver_line), "V:%s", ver_upper);
    if (vn < 0) {
        vn = 0;
    } else if ((size_t)vn >= sizeof(ver_line)) {
        vn = (int)sizeof(ver_line) - 1;
    }
    byok_display_draw_text(4, 20, 0, ver_line, (size_t)vn);

    static const char line3[] = "SEE docs/display.md SEC 6";
    byok_display_draw_text(4, 32, 0x02, line3, strlen(line3));
}

/* ==========================================================================
 * Phase runner — draw, refresh, time it, log it (including any I2C error
 * delta), hold.
 * ========================================================================== */

static void finish_phase(const char *phase_name, int64_t t0, uint32_t err_before,
                          uint32_t txn_before, uint32_t hold_ms)
{
    /* 0.1.8: byte count sampled around the same refresh as the txn count,
     * so txn_delta/byte_delta is this phase's OWN measured
     * transactions-per-byte ratio -- see byok_display_get_i2c_byte_count()'s
     * doc comment and docs/display.md Sec.3.1's transaction model.
     * Expected 1.000 on this release's per-byte
     * (CONFIG_BYOK_DISPLAY_BULK_WRITES=n) path. */
    uint32_t byte_before = byok_display_get_i2c_byte_count();
    esp_err_t rerr = byok_display_full_refresh(false);
    int64_t total_us = esp_timer_get_time() - t0;
    uint32_t err_after = byok_display_get_i2c_error_count();
    uint32_t txn_after = byok_display_get_i2c_txn_count();
    uint32_t byte_after = byok_display_get_i2c_byte_count();
    uint32_t err_delta = err_after - err_before;
    uint32_t txn_delta = txn_after - txn_before;
    uint32_t byte_delta = byte_after - byte_before;
    /* x1000 fixed-point so this build's log has no float formatting
     * dependency; byte_delta is 0 only if the phase issued zero traffic,
     * which never happens here (every phase runs a full refresh). */
    uint32_t txn_per_byte_x1000 = (byte_delta > 0) ? (txn_delta * 1000u) / byte_delta : 0u;
    ESP_LOGI(TAG,
             "phase \"%s\": %" PRId64 " us draw+refresh (refresh itself %" PRIu32 " us), "
             "i2c txns +%" PRIu32 " (total %" PRIu32 "), bytes +%" PRIu32 " (total %" PRIu32 "), "
             "txns/byte=%" PRIu32 ".%03" PRIu32 ", any_non_ok=%s, "
             "i2c errors +%" PRIu32 " (total %" PRIu32 "), refresh_result=%s",
             phase_name, total_us, byok_display_get_last_full_refresh_us(),
             txn_delta, txn_after, byte_delta, byte_after,
             txn_per_byte_x1000 / 1000u, txn_per_byte_x1000 % 1000u,
             (err_delta > 0) ? "yes" : "no",
             err_delta, err_after, esp_err_to_name(rerr));
    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
}

void byok_display_run_boot_selftest(const char *fw_version, bool boot_original_gate_open,
                                     const char *reset_reason_str)
{
    /* 0.1.14: BYOK_DISPLAY_BULK_WRITES is now a runtime setting
     * (byok_display_get_bulk_writes(), docs/protocol.md Sec.6.4b
     * DISPLAY_CFG) -- this reports its boot-time value (from
     * CONFIG_BYOK_DISPLAY_BULK_WRITES, see byok_display.c's own
     * s_bulk_writes_enabled initialiser), not a compile-time constant. */
    ESP_LOGI(TAG, "boot self-test starting (BYOK_DISPLAY_BULK_WRITES=%d)",
             (int)byok_display_get_bulk_writes());

    int64_t t0;
    uint32_t err0;
    uint32_t txn0;

    t0 = esp_timer_get_time();
    err0 = byok_display_get_i2c_error_count();
    txn0 = byok_display_get_i2c_txn_count();
    draw_checkerboard();
    finish_phase("checkerboard-8px", t0, err0, txn0, PHASE_HOLD_MS);

    t0 = esp_timer_get_time();
    err0 = byok_display_get_i2c_error_count();
    txn0 = byok_display_get_i2c_txn_count();
    draw_border_and_banner(fw_version, reset_reason_str, /*show_battery=*/false);
    finish_phase("border+banner+version", t0, err0, txn0, PHASE_HOLD_MS);

    t0 = esp_timer_get_time();
    err0 = byok_display_get_i2c_error_count();
    txn0 = byok_display_get_i2c_txn_count();
    draw_line_test();
    finish_phase("hv-line-test", t0, err0, txn0, PHASE_HOLD_MS);

    t0 = esp_timer_get_time();
    err0 = byok_display_get_i2c_error_count();
    txn0 = byok_display_get_i2c_txn_count();
    draw_gate_message(boot_original_gate_open);
    finish_phase("boot-original-gate", t0, err0, txn0, PHASE_HOLD_MS);

    /* Leave the border+banner screen up as the boot-complete resting
     * state -- callers do not need to draw anything else once this
     * returns (see byok_display_run_boot_selftest()'s doc comment). */
    t0 = esp_timer_get_time();
    err0 = byok_display_get_i2c_error_count();
    txn0 = byok_display_get_i2c_txn_count();
    draw_border_and_banner(fw_version, reset_reason_str, /*show_battery=*/true);
    finish_phase("resting-screen", t0, err0, txn0, 0);

    uint32_t total_txn = byok_display_get_i2c_txn_count();
    uint32_t total_byte = byok_display_get_i2c_byte_count();
    uint32_t total_x1000 = (total_byte > 0) ? (total_txn * 1000u) / total_byte : 0u;
    ESP_LOGI(TAG,
             "boot self-test complete: %" PRIu32 " cumulative I2C transaction(s), "
             "%" PRIu32 " cumulative I2C byte(s) (%" PRIu32 ".%03" PRIu32 " txns/byte), "
             "%" PRIu32 " cumulative I2C error(s) since boot",
             total_txn, total_byte, total_x1000 / 1000u, total_x1000 % 1000u,
             byok_display_get_i2c_error_count());
}

/* ==========================================================================
 * 0.1.7 command/data polarity A/B/C experiment -- components/byok_display/
 * Kconfig BYOK_DISPLAY_POLARITY_EXPERIMENT, docs/display.md Sec.6 (the
 * disassembly + real UC1611 datasheet re-derivation that found NO inversion
 * -- pass A's mapping is CONFIRMED correct there). Replaces
 * byok_display_run_boot_selftest() at boot when the option is on. Each pass
 * forces a full reset+init+window-program replay (byok_display_full_
 * refresh(true)) under whatever mapping was just set, so pass B genuinely
 * re-initialises the controller under the swapped addresses rather than
 * reusing state programmed under pass A's mapping.
 * ========================================================================== */
#define POLARITY_HOLD_MS 3000u

static void run_polarity_pass(const char *pass_label, bool swapped, const char *fw_version,
                               bool draw_banner_instead_of_checkerboard, uint32_t hold_ms)
{
    char pre_tag[24];
    snprintf(pre_tag, sizeof(pre_tag), "pre-pass-%s", pass_label);
    byok_display_status_read(pre_tag);

    byok_display_set_polarity(swapped);

    int64_t t0 = esp_timer_get_time();
    uint32_t err0 = byok_display_get_i2c_error_count();
    uint32_t txn0 = byok_display_get_i2c_txn_count();

    /* Full reset + 21-entry init table + window program, under the mapping
     * just set above -- NOT a cached/partial refresh. */
    esp_err_t reinit_err = byok_display_full_refresh(true);

    ESP_LOGI(TAG, "POLARITY %s: cmd=0x%02X data=0x%02X (reinit=%s)", pass_label,
             swapped ? BYOK_LCD_I2C_ADDR_DATA : BYOK_LCD_I2C_ADDR_CMD,
             swapped ? BYOK_LCD_I2C_ADDR_CMD : BYOK_LCD_I2C_ADDR_DATA,
             esp_err_to_name(reinit_err));

    char post_init_tag[24];
    snprintf(post_init_tag, sizeof(post_init_tag), "post-pass-%s-init", pass_label);
    byok_display_status_read(post_init_tag);

    if (draw_banner_instead_of_checkerboard) {
        draw_polarity_banner(fw_version, "A"); /* pass C's mapping == pass A's, docs/display.md Sec.6 */
    } else {
        draw_checkerboard();
    }
    char phase_name[32];
    snprintf(phase_name, sizeof(phase_name), "polarity-%s-%s", pass_label,
              draw_banner_instead_of_checkerboard ? "banner" : "checkerboard");
    finish_phase(phase_name, t0, err0, txn0, hold_ms);

    char post_tag[24];
    snprintf(post_tag, sizeof(post_tag), "post-pass-%s", pass_label);
    byok_display_status_read(post_tag);
}

void byok_display_run_polarity_experiment(const char *fw_version)
{
    ESP_LOGI(TAG, "polarity experiment starting (docs/display.md Sec.6: cmd=0x38/data=0x39 "
                  "CONFIRMED correct; running A/B/C for independent on-device evidence)");

    /* Pass A: current/vendor-parity mapping, cmd=0x38 / data=0x39. */
    run_polarity_pass("A", /*swapped=*/false, fw_version, /*banner=*/false, POLARITY_HOLD_MS);

    /* Pass B: SWAPPED mapping, cmd=0x39 / data=0x38. */
    run_polarity_pass("B", /*swapped=*/true, fw_version, /*banner=*/false, POLARITY_HOLD_MS);

    /* Pass C: whichever docs/display.md Sec.6 says is correct -- that
     * section's verdict is pass A's mapping (unswapped). Left as its own
     * labelled pass (not merged with A) since it exercises a fresh
     * reset+init under a clean state after pass B, and because "the
     * disassembly-derived pass" is a distinct claim from "the first pass we
     * happened to try" even though they share a mapping this release. Its
     * banner + resting screen are what gets left up when this function
     * returns. */
    run_polarity_pass("C", /*swapped=*/false, fw_version, /*banner=*/true, 0);

    uint32_t total_txn = byok_display_get_i2c_txn_count();
    uint32_t total_byte = byok_display_get_i2c_byte_count();
    uint32_t total_x1000 = (total_byte > 0) ? (total_txn * 1000u) / total_byte : 0u;
    ESP_LOGI(TAG,
             "polarity experiment complete: %" PRIu32 " cumulative I2C transaction(s), "
             "%" PRIu32 " cumulative I2C byte(s) (%" PRIu32 ".%03" PRIu32 " txns/byte), "
             "%" PRIu32 " cumulative I2C error(s) since boot",
             total_txn, total_byte, total_x1000 / 1000u, total_x1000 % 1000u,
             byok_display_get_i2c_error_count());
}
