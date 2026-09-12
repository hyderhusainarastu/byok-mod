/* SPDX-License-Identifier: MIT */
#include "byok_display.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "sdkconfig.h"

#include "byok_font8x8.h"
#include "byok_hw_shim.h" /* pulls in hw_config.h with Sec.13 unknowns resolved */

static const char *TAG = "byok_display";

/* -- state -- */
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_cmd_dev;
static i2c_master_dev_handle_t s_data_dev;
static uint8_t  s_fb[BYOK_DISPLAY_FB_BYTES];
static bool     s_inited;
static uint8_t  s_backlight_level;
static uint8_t  s_contrast_value;

/* Refresh timing (esp_timer) + cumulative I2C error count -- see
 * byok_display.h's "refresh timing / I2C health" block for why these are
 * DEBUG-log/getter-only rather than wire protocol fields. */
static uint32_t s_last_full_refresh_us;
static uint32_t s_last_partial_refresh_us;
static uint32_t s_i2c_error_count;
/* 0.1.5: cumulative count of i2c_master_transmit() CALLS (command or data,
 * bulk or per-byte), independent of s_i2c_error_count -- added so the boot
 * self-test (byok_display_selftest.c) can log "how many transactions did
 * this phase actually issue", not just "how many failed", per
 * docs/troubleshooting.md's follow-up device evidence. */
static uint32_t s_i2c_txn_count;
/* 0.1.8: cumulative count of PAYLOAD BYTES written to the panel across all
 * transactions in s_i2c_txn_count (command bytes + data bytes, bulk or
 * per-byte) -- independent of both counters above, added so the boot
 * self-test can log a measured transactions-per-byte ratio
 * (s_i2c_txn_count / s_i2c_byte_count) rather than assert it from the
 * Kconfig setting. With CONFIG_BYOK_DISPLAY_BULK_WRITES=n (this release's
 * default, and the only vendor-parity path -- docs/display.md Sec.3.1,
 * hw_config.h Sec.1 BYOK_LCD_I2C_BYTES_PER_XFER=1) this ratio measures exactly 1.0; it would
 * read below 1.0 if BULK_WRITES were ever turned back on, making the two
 * paths' actual on-wire transaction counts visible in the same log line
 * instead of only in source comments. */
static uint32_t s_i2c_byte_count;

/* 0.1.6: panel ACK probe results -- see lcd_ack_probe() below. */
static byok_ack_probe_result_t s_ack_probe_pre_init  = BYOK_ACK_PROBE_NOT_RUN;
static byok_ack_probe_result_t s_ack_probe_post_init = BYOK_ACK_PROBE_NOT_RUN;

/* 0.1.7: runtime command/data polarity mapping -- docs/display.md Sec.6,
 * components/byok_display/Kconfig BYOK_DISPLAY_POLARITY_EXPERIMENT. false
 * (default) = vendor-parity mapping (cmd 0x38 / data 0x39, s_cmd_dev /
 * s_data_dev used as their names say). true = SWAPPED for pass B of the
 * polarity experiment. Both s_cmd_dev/s_data_dev always exist at their
 * fixed addresses (0x38/0x39 respectively, set up once in
 * byok_display_init()) -- this flag only changes which handle lcd_send()/
 * lcd_send_bulk_data() select for a given is_data value, it never re-adds
 * or re-addresses either device. */
static bool s_polarity_swapped;

/* 0.1.14: runtime write-path selector (docs/protocol.md Sec.6.4b
 * DISPLAY_CFG, byok_display.h's own updated header comment) -- replaces
 * the old build-time-only #if CONFIG_BYOK_DISPLAY_BULK_WRITES branch in
 * flush_rect() below. Initialised from that same Kconfig value (still the
 * boot-time default; sdkconfig.defaults currently pins it to n, i.e.
 * per-byte) so a build with no host ever sending DISPLAY_CFG behaves
 * exactly as before this release. Resolved via #if, not a bare reference to
 * the CONFIG_ symbol in a C expression -- see the existing 0.1.5-fix
 * comment a few lines below (display_path_str()) for why: IDF Kconfig
 * only #defines a bool symbol (as 1) when it is set to y; off, the macro
 * does not exist at all, so `CONFIG_BYOK_DISPLAY_BULK_WRITES ? ... : ...`
 * fails to compile the moment the default (or this build's override) is n,
 * exactly the build this project ships. */
#if CONFIG_BYOK_DISPLAY_BULK_WRITES
static bool s_bulk_writes_enabled = true;
#else
static bool s_bulk_writes_enabled = false;
#endif

/* 0.1.10 (docs/troubleshooting.md §5's recommended
 * follow-up): this header's own "Thread-safety: none" note used to be true
 * -- byok_power's WAKE-hold shutdown path calls straight into this driver
 * from a task that does not own the display (byok_power_btn), racing
 * app_main's dispatch_task. That used to be papered over by wrapping the
 * CALLER's three display calls in vTaskSuspendAll()/xTaskResumeAll(), which
 * turned out to be a worse bug (a scheduler-suspended blocking I2C call is a
 * guaranteed configASSERT/abort -- docs/troubleshooting.md Sec.5). The actual fix
 * belongs here instead: one plain (non-recursive) FreeRTOS mutex, taken at
 * the top and given at the bottom of every public entry point that touches
 * s_fb or issues an I2C transaction to the panel, so at most one such call
 * is ever in flight system-wide -- dispatch_task and byok_power_btn now
 * serialize on this lock instead of interleaving mid-transaction.
 *
 * Non-recursive is deliberate and safe: byok_display_init() is the only
 * place a locked public entry point (byok_display_full_refresh()) is called
 * from INSIDE another one of this file's own functions, and init() itself
 * does not take the lock -- there is no nested-acquire path anywhere in
 * this file, so a plain mutex cannot self-deadlock. portMAX_DELAY (block
 * forever rather than a short timeout) is likewise safe for the same
 * reason: nothing this lock guards ever waits on a second thing that could
 * itself be waiting on this lock.
 *
 * Statically allocated (xSemaphoreCreateMutexStatic) so there is no heap
 * allocation failure path to handle at init time. Created lazily on
 * byok_display_init()'s first call (idempotent -- a second init() call
 * finds it already made) rather than at file-load time, since FreeRTOS
 * object creation is a runtime call, not something that can happen at
 * static-initializer time.
 *
 * LEDC backlight calls (byok_display_set_backlight()) are a separate
 * hardware peripheral from the I2C panel bus and are deliberately NOT
 * gated by this lock: a concurrent PWM duty/fade change cannot tear an
 * in-flight I2C draw, and byok_power's shutdown sequence relies on being
 * able to start the 500 ms backlight fade without waiting on this lock. */
static StaticSemaphore_t s_display_mutex_buf;
static SemaphoreHandle_t s_display_mutex;

static inline void display_lock(void)
{
    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
}

static inline void display_unlock(void)
{
    xSemaphoreGive(s_display_mutex);
}

#define CHK(expr) do { esp_err_t _e = (expr); if (_e != ESP_OK) return _e; } while (0)

/* 0.1.14: was a compile-time #if CONFIG_BYOK_DISPLAY_BULK_WRITES string
 * constant through 0.1.13 (see git history) -- now a runtime read of
 * s_bulk_writes_enabled, since the write path itself is runtime-selectable
 * (DISPLAY_CFG, docs/protocol.md Sec.6.4b) and the two DEBUG-log call
 * sites below need to report whichever path the NEXT flush_rect() will
 * actually take. */
static inline const char *display_path_str(void)
{
    return s_bulk_writes_enabled ? "bulk" : "per-byte";
}

/* ==========================================================================
 * Low-level I2C send
 * ========================================================================== */

/* Commands (and, on the CONFIG_BYOK_DISPLAY_BULK_WRITES=n fallback path,
 * data too) always go one byte per I2C transaction -- hw_config.h Sec.1:
 * BYOK_LCD_I2C_BYTES_PER_XFER == 1, the vendor driver's own behaviour,
 * CONFIRMED from the disassembly. Every failure is counted, never retried
 * or escalated here -- see byok_display_get_i2c_error_count(). */
static inline esp_err_t lcd_send(bool is_data, uint8_t byte)
{
    /* 0.1.7: s_polarity_swapped flips which physical handle "is_data"
     * selects -- see docs/display.md Sec.6 and byok_display_set_polarity()
     * in byok_display.h. Unswapped (default): is_data -> s_data_dev
     * (0x39), !is_data -> s_cmd_dev (0x38) -- unchanged from every prior
     * release. */
    bool use_data_handle = s_polarity_swapped ? !is_data : is_data;
    i2c_master_dev_handle_t dev = use_data_handle ? s_data_dev : s_cmd_dev;
    esp_err_t err = i2c_master_transmit(dev, &byte, 1, (int)BYOK_LCD_I2C_TIMEOUT_MS);
    s_i2c_txn_count++;
    s_i2c_byte_count++;
    if (err != ESP_OK) {
        s_i2c_error_count++;
    }
    return err;
}

/* Bulk data path (docs/protocol.md Sec.6.4b DISPLAY_CFG,
 * byok_display_set_bulk_writes()): the UC1611 auto-increments its column
 * address after each data byte written inside the window programmed by
 * window_program() (hw_config.h Sec.3.3), so `len` consecutive framebuffer
 * bytes -- a full 8-row page, or (when the window is full-width) the whole
 * window at once -- can be handed to the controller as ONE
 * i2c_master_transmit() call to the data address, instead of `len`
 * one-byte transactions. This is the only difference from the stock
 * behaviour; command traffic (reset, init table, window addressing,
 * contrast) is always one byte per transaction regardless of this path,
 * see lcd_send() above. 0.1.14: always compiled in (was previously behind
 * #if CONFIG_BYOK_DISPLAY_BULK_WRITES, one code path or the other) --
 * flush_rect() below now selects between this and the per-byte loop at
 * runtime via s_bulk_writes_enabled, so both must exist in every build. */
static inline esp_err_t lcd_send_bulk_data(const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    /* 0.1.7: same polarity flip as lcd_send() -- pixel data is always
     * "is_data=true" conceptually, so it goes to s_cmd_dev when swapped. */
    i2c_master_dev_handle_t dev = s_polarity_swapped ? s_cmd_dev : s_data_dev;
    esp_err_t err = i2c_master_transmit(dev, buf, len, (int)BYOK_LCD_I2C_TIMEOUT_MS);
    s_i2c_txn_count++;
    s_i2c_byte_count += (uint32_t)len;
    if (err != ESP_OK) {
        s_i2c_error_count++;
    }
    return err;
}

/* ==========================================================================
 * 0.1.6 panel ACK probe -- diagnostic only, see the blank-display entry in
 * docs/troubleshooting.md (0.1.5
 * device evidence: vendor-identical per-byte I2C stream, 0 errors, panel
 * still blank). hw_config.h Sec.1's BYOK_LCD_I2C_DISABLE_ACK_CHECK=1 makes
 * s_cmd_dev/s_data_dev vendor-parity-correct (the vendor's own dev_cfg sets
 * this flag too) but also means every normal transaction this driver issues
 * is BLIND to whether the panel is even present: i2c_master_transmit()
 * returns ESP_OK on a NACK'd address or data byte exactly the same as on a
 * real ACK. This function opens a THIRD, temporary device handle on the
 * SAME bus and the SAME command address (0x38) with ACK checking explicitly
 * ENABLED, sends one single-byte command, reports whether it ACKed, and
 * closes the handle again -- it never touches s_cmd_dev/s_data_dev.
 * ========================================================================== */

/* Probe byte: 0x00 ("Set Column Address LSB = 0"), sent to the COMMAND
 * address only. Deliberately NOT the 0xE3 "NOP" some UC1611-family variants
 * document: 0xE3 is never exercised anywhere in either vendor image (the
 * only unplaced opcodes on record for THIS panel are 0xC9/0x95/0xE1 --
 * hw_config.h Sec.13), so asserting it is a no-op on this specific silicon
 * would be exactly the kind of unverified guess SAFETY.md §2's evidence
 * standard exists to keep out of a shipped default. 0x00-range column-
 * address-LSB commands are a near-universal convention across the whole
 * ST75xx/UC16xx lineage this panel is STRONGLY INDICATED to belong to
 * (hw_config.h Sec.3), and -- decisively -- are safe REGARDLESS of which
 * single-byte command 0x00 actually decodes to on this controller: every
 * RAM write this driver ever issues is preceded by window_program()
 * explicitly setting the column/page address (below), so a stray
 * column-address write cannot survive to affect a real draw. Sent to 0x38
 * (COMMAND) only; 0x39 (DATA) is deliberately NOT probed -- a byte sent to
 * the data address is a RAM write, not obviously undoable without also
 * knowing the RAM pointer is about to be reset, which this probe (called
 * before any window has been programmed) cannot assume. */
#define BYOK_ACK_PROBE_CMD_BYTE 0x00

static void lcd_ack_probe(const char *when, byok_ack_probe_result_t *out)
{
    i2c_device_config_t probe_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BYOK_LCD_I2C_ADDR_CMD,
        .scl_speed_hz = BYOK_LCD_I2C_SPEED_HZ,
        .flags.disable_ack_check = 0, /* the one and only difference from s_cmd_dev -- the entire
                                        * point of this probe having its own handle */
    };
    i2c_master_dev_handle_t probe_dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &probe_cfg, &probe_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel ACK @0x38 (%s): could not add probe device (err=%s) -- probe skipped",
                 when, esp_err_to_name(err));
        *out = BYOK_ACK_PROBE_NOT_RUN;
        return;
    }

    uint8_t byte = BYOK_ACK_PROBE_CMD_BYTE;
    err = i2c_master_transmit(probe_dev, &byte, 1, (int)BYOK_LCD_I2C_TIMEOUT_MS);
    bool acked = (err == ESP_OK);
    ESP_LOGI(TAG, "panel ACK @0x38 (%s): %s (err=%s)", when, acked ? "yes" : "no",
             esp_err_to_name(err));
    *out = acked ? BYOK_ACK_PROBE_ACKED : BYOK_ACK_PROBE_NO_ACK;

    esp_err_t rm_err = i2c_master_bus_rm_device(probe_dev);
    if (rm_err != ESP_OK) {
        /* Never seen in practice (the handle was just added successfully on the
         * same bus, nothing else touches it), but this is diagnostic code --
         * log and move on rather than letting a cleanup failure propagate. */
        ESP_LOGW(TAG, "panel ACK @0x38 (%s): i2c_master_bus_rm_device failed (err=%s) -- leaked probe handle",
                 when, esp_err_to_name(rm_err));
    }
}

byok_ack_probe_result_t byok_display_get_ack_probe_pre_init(void)
{
    return s_ack_probe_pre_init;
}

byok_ack_probe_result_t byok_display_get_ack_probe_post_init(void)
{
    return s_ack_probe_post_init;
}

/* ==========================================================================
 * 0.1.7 command/data polarity mapping + STATUS READ probe -- docs/display.md
 * Sec.10, byok_display.h's own doc comments above each declaration.
 * ========================================================================== */

void byok_display_set_polarity(bool swapped)
{
    s_polarity_swapped = swapped;
    ESP_LOGI(TAG, "polarity mapping set: cmd=0x%02X data=0x%02X (swapped=%s)",
             swapped ? BYOK_LCD_I2C_ADDR_DATA : BYOK_LCD_I2C_ADDR_CMD,
             swapped ? BYOK_LCD_I2C_ADDR_CMD : BYOK_LCD_I2C_ADDR_DATA,
             swapped ? "yes" : "no");
}

bool byok_display_get_polarity_swapped(void)
{
    return s_polarity_swapped;
}

void byok_display_status_read(const char *when)
{
    uint8_t cmd_byte = 0, data_byte = 0;
    esp_err_t cmd_err = i2c_master_receive(s_cmd_dev, &cmd_byte, 1, (int)BYOK_LCD_I2C_TIMEOUT_MS);
    esp_err_t data_err = i2c_master_receive(s_data_dev, &data_byte, 1, (int)BYOK_LCD_I2C_TIMEOUT_MS);
    ESP_LOGI(TAG,
             "STATUS READ (%s): @0x%02X(cmd) err=%s byte=0x%02X | @0x%02X(data) err=%s byte=0x%02X",
             when, BYOK_LCD_I2C_ADDR_CMD, esp_err_to_name(cmd_err), cmd_byte,
             BYOK_LCD_I2C_ADDR_DATA, esp_err_to_name(data_err), data_byte);
}

/* ==========================================================================
 * Framebuffer bit convention -- see byok_display.h header comment. Internal
 * storage is hw convention (1 = light/off, CONFIRMED blank pattern under
 * "Inverse Display ON"); every public entry point deals in protocol
 * convention (dark = pixel on) and this pair does the one translation.
 * ========================================================================== */
static inline void fb_set_pixel(uint16_t x, uint16_t y, bool dark)
{
    if (x >= BYOK_DISPLAY_W || y >= BYOK_DISPLAY_H) {
        return;
    }
    size_t idx = BYOK_DISPLAY_FB_INDEX(x, y);
    uint8_t bit = BYOK_LCD_FB_BIT_ORDER ? (uint8_t)(1u << (y & 7u))
                                         : (uint8_t)(1u << (7u - (y & 7u)));
    if (dark) {
        s_fb[idx] &= (uint8_t)~bit;
    } else {
        s_fb[idx] |= bit;
    }
}

static inline bool fb_get_pixel(uint16_t x, uint16_t y)
{
    if (x >= BYOK_DISPLAY_W || y >= BYOK_DISPLAY_H) {
        return false;
    }
    size_t idx = BYOK_DISPLAY_FB_INDEX(x, y);
    uint8_t bit = BYOK_LCD_FB_BIT_ORDER ? (uint8_t)(1u << (y & 7u))
                                         : (uint8_t)(1u << (7u - (y & 7u)));
    return (s_fb[idx] & bit) == 0; /* 0 = dark under hw convention */
}

/* ==========================================================================
 * Reset preamble + 21-entry register configuration this panel requires --
 * see hw_config.h Sec.3.1/3.2 for the interoperability specification.
 * Replayed verbatim regardless of the Sec.13 "meaning unknown" markers on a
 * few of the opcodes (0xC9/0xAC, 0x95, 0xE1) -- see byok_hw_shim.h.
 * ========================================================================== */
static esp_err_t lcd_hw_reset_and_init(void)
{
    static const uint8_t table[BYOK_LCD_INIT_SEQ_LEN][2] = { BYOK_LCD_INIT_SEQ };

#if CONFIG_BYOK_DISPLAY_TRUE_SYSTEM_RESET
    /* byok-mod 0.1.2: a genuine UC1611 System Reset, sent as an actual
     * COMMAND (CD=0) to the command address -- NOT the vendor's 0xE1/0xE2
     * pair below, which never issues a real reset (docs/recovery.md §6).
     * Strictly additive: the vendor sequence
     * after this block is byte-identical to the CONFIG_BYOK_DISPLAY_
     * TRUE_SYSTEM_RESET=n path. Needed because the panel has no reset GPIO
     * and is not power-cycled by a warm S3 reset, so without this the
     * 21-entry init table below is written over whatever state the
     * previous run left (stale display RAM, contrast, scroll position,
     * inverse-display flag, charge-pump state). */
    vTaskDelay(pdMS_TO_TICKS(BYOK_LCD_TRUE_RESET_PRE_DELAY_MS));
    CHK(lcd_send(false, BYOK_LCD_TRUE_RESET_CMD_BYTE));
    vTaskDelay(pdMS_TO_TICKS(BYOK_LCD_TRUE_RESET_SETTLE_MS));
#endif

    vTaskDelay(pdMS_TO_TICKS(BYOK_LCD_RESET_DELAY_MS));
    CHK(lcd_send(false, BYOK_LCD_RESET_CMD_BYTE));
    CHK(lcd_send(true, BYOK_LCD_RESET_DATA_BYTE));
    vTaskDelay(pdMS_TO_TICKS(BYOK_LCD_RESET_DELAY_MS));

    for (unsigned i = 0; i < BYOK_LCD_INIT_SEQ_LEN; i++) {
        CHK(lcd_send(table[i][0] != 0, table[i][1]));
    }

#if CONFIG_BYOK_DISPLAY_FORCE_SCROLL_LINE_ZERO
    /* byok-mod 0.1.9: docs/troubleshooting.md, hw_config.h
     * Sec.3.2.2. NOT part of the vendor sequence -- the vendor never sends
     * any byte in 0x40-0x7F (exhaustive scan of all 20 sim_send call
     * sites) and so relies on this register's power-on value, which on
     * this unit is 15, producing a 15-row-too-high image.
     *
     * MSB FIRST, then LSB -- deliberate. On a UC1611 the pair is
     * 0x50|SL[7:4] then 0x40|SL[3:0] and either order gives SL=0. But if
     * this UC1611s variant instead decodes 0x40-0x7F as a single 6-bit
     * "Set Scroll Line" (the UC1610-style map), 0x50 alone means SL=16 --
     * so sending 0x50 first and 0x40 second lands on SL=0 under BOTH
     * decodes, while LSB-then-MSB would leave SL=16 under the 6-bit one.
     * Both bytes are COMMANDS (CD=0, address 0x38). */
    CHK(lcd_send(false, (uint8_t)(BYOK_LCD_CMD_SCROLL_LINE_MSB |
                                  ((BYOK_LCD_SCROLL_LINE_RESET >> 4) & 0x0Fu))));  /* 0x50 */
    CHK(lcd_send(false, (uint8_t)(BYOK_LCD_CMD_SCROLL_LINE_LSB |
                                  (BYOK_LCD_SCROLL_LINE_RESET & 0x0Fu))));         /* 0x40 */
#endif

    return ESP_OK;
}

/* ==========================================================================
 * 0.1.8 display-enable step -- hw_config.h Sec.3.2.1, docs/display.md Sec.5.
 * The one vendor
 * gdisp_init() action byok-mod omitted through 0.1.7: CMD 0xC9 / DATA 0xAD,
 * sent exactly once, immediately after the FIRST full frame is in display
 * RAM (byok_display_init()'s own initial refresh, or a force_reinit full
 * refresh's replay of that same bring-up shape below). Built on lcd_send()
 * like every other panel byte -- this is a missing protocol step, not a
 * transport change; the transaction it issues is identical in shape to
 * every other command+data pair this driver already sends. Two lcd_send()
 * failures both propagate via CHK() rather than being swallowed, matching
 * lcd_hw_reset_and_init()'s own error handling.
 * ========================================================================== */
static esp_err_t lcd_send_display_enable(void)
{
    CHK(lcd_send(false, BYOK_LCD_CMD_DISPLAY_ENABLE));
    CHK(lcd_send(true, BYOK_LCD_DATA_DISPLAY_ENABLE));
    return ESP_OK;
}

/* ==========================================================================
 * Partial-update window + flush -- hw_config.h Sec.3.3, CONFIRMED from
 * window_program()/gdisp_flush() disassembly.
 * ========================================================================== */
static esp_err_t window_program(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    CHK(lcd_send(false, BYOK_LCD_CMD_RAM_ADDR_CTRL));
    CHK(lcd_send(false, BYOK_LCD_CMD_WIN_MODE_OFF));
    CHK(lcd_send(false, BYOK_LCD_CMD_WIN_PAGE_START));
    CHK(lcd_send(true, (uint8_t)(y0 >> 3)));
    CHK(lcd_send(false, BYOK_LCD_CMD_WIN_PAGE_END));
    CHK(lcd_send(true, (uint8_t)(y1 >> 3)));
    CHK(lcd_send(false, BYOK_LCD_CMD_WIN_COL_START));
    CHK(lcd_send(true, (uint8_t)x0));
    CHK(lcd_send(false, BYOK_LCD_CMD_WIN_COL_END));
    CHK(lcd_send(true, (uint8_t)x1));
    CHK(lcd_send(false, BYOK_LCD_CMD_WIN_MODE_ON));
    CHK(lcd_send(false, BYOK_LCD_CMD_WIN_TRAILER));
    return ESP_OK;
}

static esp_err_t flush_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (y1 < y0 || x1 < x0) {
        return ESP_OK; /* mirrors gdisp_flush's own early return */
    }
    if (x1 > BYOK_DISPLAY_W - 1) {
        x1 = BYOK_DISPLAY_W - 1;
    }
    if (y1 > BYOK_DISPLAY_H - 1) {
        y1 = BYOK_DISPLAY_H - 1;
    }

    CHK(window_program(x0, y0, x1, y1));

    uint16_t y = (uint16_t)(y0 & ~7u); /* page-align the start, per hw_config.h */

    /* 0.1.14: runtime branch (s_bulk_writes_enabled, docs/protocol.md
     * Sec.6.4b DISPLAY_CFG) -- was a build-time #if CONFIG_BYOK_DISPLAY_
     * BULK_WRITES through 0.1.13; both paths are now always compiled in,
     * see lcd_send_bulk_data()'s own updated comment above. */
    if (s_bulk_writes_enabled) {
    if (x0 == 0 && x1 == BYOK_DISPLAY_W - 1) {
        /* Full-width window: every page row's bytes are contiguous in s_fb
         * (stride == W, hw_config.h's own FB_INDEX arithmetic), and so is
         * the run across MULTIPLE page rows -- send the whole span in one
         * transaction rather than one per page. */
        uint16_t pages = (uint16_t)(((y1 - y) >> 3) + 1);
        size_t len = (size_t)pages * BYOK_DISPLAY_W;
        CHK(lcd_send_bulk_data(&s_fb[BYOK_DISPLAY_FB_INDEX(0, y)], len));
    } else {
        /* Narrower partial window: each page row is still one contiguous
         * run of (x1-x0+1) bytes in s_fb -- one transaction per page
         * instead of one per byte. */
        for (; y <= y1; y = (uint16_t)(y + 8)) {
            size_t len = (size_t)(x1 - x0 + 1);
            CHK(lcd_send_bulk_data(&s_fb[BYOK_DISPLAY_FB_INDEX(x0, y)], len));
        }
    }
    } else {
        /* Fallback path: reproduces the stock per-byte behaviour exactly
         * (one i2c_master_transmit() per framebuffer byte). */
        for (; y <= y1; y = (uint16_t)(y + 8)) {
            for (uint16_t x = x0; x <= x1; x++) {
                CHK(lcd_send(true, s_fb[BYOK_DISPLAY_FB_INDEX(x, y)]));
            }
        }
    }
    return ESP_OK;
}

void byok_display_set_bulk_writes(bool enable)
{
    if (enable != s_bulk_writes_enabled) {
        ESP_LOGI(TAG, "display write path -> %s (docs/protocol.md Sec.6.4b DISPLAY_CFG)",
                 enable ? "bulk" : "per-byte");
    }
    s_bulk_writes_enabled = enable;
}

bool byok_display_get_bulk_writes(void)
{
    return s_bulk_writes_enabled;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

esp_err_t byok_display_init(void)
{
    /* Lazily create the display mutex on the first call (idempotent -- see
     * this file's own comment above s_display_mutex). Must happen before
     * the s_inited fast-return below so a second byok_display_init() call
     * (a no-op otherwise) still leaves s_display_mutex non-NULL exactly as
     * the first call did. */
    if (s_display_mutex == NULL) {
        s_display_mutex = xSemaphoreCreateMutexStatic(&s_display_mutex_buf);
    }

    if (s_inited) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BYOK_LCD_I2C_PORT,
        .sda_io_num = BYOK_LCD_I2C_SDA_GPIO,
        .scl_io_num = BYOK_LCD_I2C_SCL_GPIO,
        /* hw_config.h BYOK_LCD_I2C_CLK_SRC_XTAL: [CONFIRMED] from the vendor
         * disassembly the panel's I2C clock source is XTAL (which happens
         * to equal I2C_CLK_SRC_DEFAULT on the S3, but the RE'd value
         * should be what actually reaches the driver, not an IDF default
         * that's only coincidentally the same). */
        .clk_source = BYOK_LCD_I2C_CLK_SRC_XTAL ? I2C_CLK_SRC_XTAL : I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = BYOK_LCD_I2C_GLITCH_IGNORE_CNT,
        .flags.enable_internal_pullup = BYOK_LCD_I2C_INTERNAL_PULLUP,
    };
    CHK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t cmd_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BYOK_LCD_I2C_ADDR_CMD,
        .scl_speed_hz = BYOK_LCD_I2C_SPEED_HZ,
        .flags.disable_ack_check = BYOK_LCD_I2C_DISABLE_ACK_CHECK,
    };
    CHK(i2c_master_bus_add_device(s_bus, &cmd_cfg, &s_cmd_dev));

    i2c_device_config_t data_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BYOK_LCD_I2C_ADDR_DATA,
        .scl_speed_hz = BYOK_LCD_I2C_SPEED_HZ,
        .flags.disable_ack_check = BYOK_LCD_I2C_DISABLE_ACK_CHECK,
    };
    CHK(i2c_master_bus_add_device(s_bus, &data_cfg, &s_data_dev));

    /* 0.1.6: probe BEFORE the reset preamble -- whatever state the panel
     * powered up in, or didn't. See lcd_ack_probe()'s own block comment. */
    lcd_ack_probe("pre-init", &s_ack_probe_pre_init);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = (ledc_mode_t)BYOK_BACKLIGHT_LEDC_MODE,
        .duty_resolution = (ledc_timer_bit_t)BYOK_BACKLIGHT_DUTY_RES_BITS,
        .timer_num = (ledc_timer_t)BYOK_BACKLIGHT_LEDC_TIMER,
        .freq_hz = BYOK_BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    CHK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BYOK_BACKLIGHT_GPIO,
        .speed_mode = (ledc_mode_t)BYOK_BACKLIGHT_LEDC_MODE,
        .channel = (ledc_channel_t)BYOK_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)BYOK_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    CHK(ledc_channel_config(&ch_cfg));
    CHK(ledc_fade_func_install(0));

    memset(s_fb, BYOK_DISPLAY_FB_BLANK_BYTE, sizeof(s_fb));

    esp_err_t err = lcd_hw_reset_and_init();

    /* 0.1.6: probe AFTER the reset preamble + 21-entry init table, whether
     * or not that sequence itself reported ESP_OK -- diagnostic value in
     * both cases, and this never gates the early return below. */
    lcd_ack_probe("post-init", &s_ack_probe_post_init);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel reset/init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_backlight_level = 0;
    s_contrast_value = 128; /* nominal boot default; init table already
                              * programmed the panel's own default register
                              * (BYOK_LCD_CONTRAST_REG_DEFAULT) independently
                              * of this cached value. */
    s_inited = true;

    esp_err_t refresh_err = byok_display_full_refresh(false);
    if (refresh_err != ESP_OK) {
        return refresh_err;
    }

    /* 0.1.8: vendor gdisp_init() order (hw_config.h Sec.3.2.1) is reset+init
     * -> blank fb -> ONE full refresh -> THIS pair -> contrast reset ->
     * flush-if-dirty. The refresh above is that one full refresh; this is
     * the display-enable step right where the vendor puts it. Not folded
     * into byok_display_full_refresh(false) itself -- see that function's
     * own force_reinit branch below, which is the OTHER place this same
     * bring-up shape recurs. */
    esp_err_t enable_err = lcd_send_display_enable();
    if (enable_err != ESP_OK) {
        ESP_LOGE(TAG, "display-enable step (CMD 0x%02X/DATA 0x%02X) failed: %s",
                 BYOK_LCD_CMD_DISPLAY_ENABLE, BYOK_LCD_DATA_DISPLAY_ENABLE,
                 esp_err_to_name(enable_err));
        return enable_err;
    }
    return ESP_OK;
}

bool byok_display_is_ready(void)
{
    return s_inited;
}

/* 0.1.10: every function below down to byok_display_set_contrast() follows
 * the same "_impl does the real work, the public entry point just takes the
 * lock" shape -- see s_display_mutex's own comment above for why. */
static void clear_impl(bool dark)
{
    memset(s_fb, dark ? 0x00 : BYOK_DISPLAY_FB_BLANK_BYTE, sizeof(s_fb));
}

void byok_display_clear(bool dark)
{
    display_lock();
    clear_impl(dark);
    display_unlock();
}

static void draw_text_impl(uint16_t x, uint16_t y, uint8_t style,
                            const char *ascii, size_t len)
{
    const bool inverted = (style & 0x01) != 0;
    const bool wrap = (style & 0x02) != 0;
    const bool clip = (style & 0x04) != 0;

    uint16_t cx = x, cy = y;
    for (size_t i = 0; i < len; i++) {
        if ((uint32_t)cx + BYOK_FONT8X8_GLYPH_W > BYOK_DISPLAY_W) {
            if (wrap) {
                cx = 0;
                cy = (uint16_t)(cy + BYOK_FONT8X8_GLYPH_H);
                if (cy >= BYOK_DISPLAY_H) {
                    break;
                }
            } else if (clip) {
                break;
            }
            /* neither flag set: fall through and let fb_set_pixel's own
             * bounds check silently drop the off-panel columns. */
        }

        const uint8_t *glyph = byok_font8x8_lookup((uint32_t)(unsigned char)ascii[i]);
        for (uint8_t row = 0; row < BYOK_FONT8X8_GLYPH_H; row++) {
            uint8_t bits = glyph[row];
            for (uint8_t col = 0; col < BYOK_FONT8X8_GLYPH_W; col++) {
                bool on = ((bits >> (7 - col)) & 1) != 0;
                if (inverted) {
                    on = !on;
                }
                fb_set_pixel((uint16_t)(cx + col), (uint16_t)(cy + row), on);
            }
        }
        cx = (uint16_t)(cx + BYOK_FONT8X8_GLYPH_W);
    }
}

void byok_display_draw_text(uint16_t x, uint16_t y, uint8_t style,
                             const char *ascii, size_t len)
{
    display_lock();
    draw_text_impl(x, y, style, ascii, len);
    display_unlock();
}

static void fill_rect_impl(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            uint8_t op, bool value)
{
    if (w == 0 || h == 0) {
        return;
    }
    uint32_t x1_32 = (uint32_t)x + w - 1;
    uint32_t y1_32 = (uint32_t)y + h - 1;
    uint16_t x1 = (x1_32 > 0xFFFFu) ? 0xFFFFu : (uint16_t)x1_32;
    uint16_t y1 = (y1_32 > 0xFFFFu) ? 0xFFFFu : (uint16_t)y1_32;

    for (uint32_t yy = y; yy <= y1; yy++) {
        if (yy >= BYOK_DISPLAY_H) {
            break;
        }
        for (uint32_t xx = x; xx <= x1; xx++) {
            if (xx >= BYOK_DISPLAY_W) {
                break;
            }
            bool border = (yy == y || yy == y1 || xx == x || xx == x1);
            switch (op) {
            case BYOK_RECT_OP_OUTLINE:
                if (border) {
                    fb_set_pixel((uint16_t)xx, (uint16_t)yy, value);
                }
                break;
            case BYOK_RECT_OP_FILLED:
                fb_set_pixel((uint16_t)xx, (uint16_t)yy, value);
                break;
            case BYOK_RECT_OP_INVERT:
                fb_set_pixel((uint16_t)xx, (uint16_t)yy, !fb_get_pixel((uint16_t)xx, (uint16_t)yy));
                break;
            case BYOK_RECT_OP_CLEAR_REGION:
                fb_set_pixel((uint16_t)xx, (uint16_t)yy, false);
                break;
            default:
                break;
            }
        }
    }
}

void byok_display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             uint8_t op, bool value)
{
    display_lock();
    fill_rect_impl(x, y, w, h, op, value);
    display_unlock();
}

static bool draw_bitmap_impl(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint8_t bpp, uint8_t op,
                              const uint8_t *packed, size_t packed_len)
{
    if ((bpp != 1 && bpp != 2) || packed == NULL || w == 0 || h == 0) {
        return false;
    }
    size_t stride = ((size_t)w * bpp + 7) / 8;
    size_t expected = stride * h;
    if (packed_len != expected) {
        return false;
    }

    for (uint16_t row = 0; row < h; row++) {
        const uint8_t *rowptr = packed + (size_t)row * stride;
        for (uint16_t col = 0; col < w; col++) {
            bool src_dark;
            if (bpp == 1) {
                uint8_t byte = rowptr[col / 8];
                src_dark = ((byte >> (7 - (col % 8))) & 1) != 0;
            } else {
                uint8_t byte = rowptr[col / 4];
                uint8_t shift = (uint8_t)(6 - 2 * (col % 4));
                uint8_t val = (uint8_t)((byte >> shift) & 0x3);
                src_dark = (val < 2); /* down-convert 2bpp: 0,1 -> dark; 2,3 -> light */
            }

            uint16_t px = (uint16_t)(x + col);
            uint16_t py = (uint16_t)(y + row);
            if (px >= BYOK_DISPLAY_W || py >= BYOK_DISPLAY_H) {
                continue;
            }
            bool dst_dark = fb_get_pixel(px, py);
            bool result;
            switch (op) {
            case BYOK_BITMAP_OP_OR:  result = src_dark || dst_dark; break;
            case BYOK_BITMAP_OP_AND: result = src_dark && dst_dark; break;
            case BYOK_BITMAP_OP_XOR: result = src_dark != dst_dark; break;
            case BYOK_BITMAP_OP_COPY:
            default:                 result = src_dark; break;
            }
            fb_set_pixel(px, py, result);
        }
    }
    return true;
}

bool byok_display_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               uint8_t bpp, uint8_t op,
                               const uint8_t *packed, size_t packed_len)
{
    display_lock();
    bool ok = draw_bitmap_impl(x, y, w, h, bpp, op, packed, packed_len);
    display_unlock();
    return ok;
}

static bool load_full_raster_1bpp_impl(const uint8_t *raster, size_t raster_len)
{
    const size_t stride = (BYOK_DISPLAY_W + 7) / 8; /* 30 */
    const size_t expected = stride * BYOK_DISPLAY_H; /* 2400 */
    if (raster == NULL || raster_len != expected) {
        return false;
    }
    for (uint16_t y = 0; y < BYOK_DISPLAY_H; y++) {
        const uint8_t *rowptr = raster + (size_t)y * stride;
        for (uint16_t x = 0; x < BYOK_DISPLAY_W; x++) {
            bool dark = ((rowptr[x / 8] >> (7 - (x % 8))) & 1) != 0;
            fb_set_pixel(x, y, dark);
        }
    }
    return true;
}

bool byok_display_load_full_raster_1bpp(const uint8_t *raster, size_t raster_len)
{
    display_lock();
    bool ok = load_full_raster_1bpp_impl(raster, raster_len);
    display_unlock();
    return ok;
}

static bool save_framebuffer_impl(uint8_t *out, size_t out_len)
{
    if (out == NULL || out_len != sizeof(s_fb)) {
        return false;
    }
    memcpy(out, s_fb, sizeof(s_fb));
    return true;
}

bool byok_display_save_framebuffer(uint8_t *out, size_t out_len)
{
    display_lock();
    bool ok = save_framebuffer_impl(out, out_len);
    display_unlock();
    return ok;
}

static bool restore_framebuffer_impl(const uint8_t *buf, size_t buf_len)
{
    if (buf == NULL || buf_len != sizeof(s_fb)) {
        return false;
    }
    memcpy(s_fb, buf, sizeof(s_fb));
    return true;
}

bool byok_display_restore_framebuffer(const uint8_t *buf, size_t buf_len)
{
    display_lock();
    bool ok = restore_framebuffer_impl(buf, buf_len);
    display_unlock();
    return ok;
}

static esp_err_t full_refresh_impl(bool force_reinit)
{
    if (force_reinit) {
        CHK(lcd_hw_reset_and_init());
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = flush_rect(0, 0, BYOK_DISPLAY_W - 1, BYOK_DISPLAY_H - 1);
    s_last_full_refresh_us = (uint32_t)(esp_timer_get_time() - t0);
    ESP_LOGD(TAG, "full refresh: %" PRIu32 " us (%s path, %s)", s_last_full_refresh_us,
             display_path_str(), esp_err_to_name(err));
    if (err != ESP_OK) {
        return err;
    }
    if (force_reinit) {
        /* 0.1.8: force_reinit reproduces the vendor's whole gdisp_init()
         * bring-up shape (reset+init table, THEN the first full frame,
         * above) -- so it gets the same display-enable step
         * byok_display_init() sends after ITS first full refresh, per
         * hw_config.h Sec.3.2.1. A caller-requested reinit (a FRAME command
         * with the reinit bit set, main/app_main.c) is exactly this
         * project's equivalent of the vendor re-running gdisp_init(). */
        esp_err_t enable_err = lcd_send_display_enable();
        if (enable_err != ESP_OK) {
            ESP_LOGE(TAG, "display-enable step (CMD 0x%02X/DATA 0x%02X) failed: %s",
                     BYOK_LCD_CMD_DISPLAY_ENABLE, BYOK_LCD_DATA_DISPLAY_ENABLE,
                     esp_err_to_name(enable_err));
            return enable_err;
        }
    }
    return ESP_OK;
}

esp_err_t byok_display_full_refresh(bool force_reinit)
{
    display_lock();
    esp_err_t err = full_refresh_impl(force_reinit);
    display_unlock();
    return err;
}

static esp_err_t partial_refresh_impl(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t x1_32 = (uint32_t)x + w - 1;
    uint32_t y1_32 = (uint32_t)y + h - 1;
    uint16_t x1 = (x1_32 > BYOK_DISPLAY_W - 1) ? (BYOK_DISPLAY_W - 1) : (uint16_t)x1_32;
    uint16_t y1 = (y1_32 > BYOK_DISPLAY_H - 1) ? (BYOK_DISPLAY_H - 1) : (uint16_t)y1_32;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = flush_rect(x, y, x1, y1);
    s_last_partial_refresh_us = (uint32_t)(esp_timer_get_time() - t0);
    ESP_LOGD(TAG, "partial refresh (%ux%u): %" PRIu32 " us (%s path, %s)", (unsigned)w, (unsigned)h,
             s_last_partial_refresh_us, display_path_str(),
             esp_err_to_name(err));
    return err;
}

esp_err_t byok_display_partial_refresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    display_lock();
    esp_err_t err = partial_refresh_impl(x, y, w, h);
    display_unlock();
    return err;
}

uint32_t byok_display_get_last_full_refresh_us(void)
{
    return s_last_full_refresh_us;
}

uint32_t byok_display_get_last_partial_refresh_us(void)
{
    return s_last_partial_refresh_us;
}

uint32_t byok_display_get_i2c_error_count(void)
{
    return s_i2c_error_count;
}

uint32_t byok_display_get_i2c_txn_count(void)
{
    return s_i2c_txn_count;
}

uint32_t byok_display_get_i2c_byte_count(void)
{
    return s_i2c_byte_count;
}

esp_err_t byok_display_set_backlight(uint8_t level, uint16_t fade_ms)
{
    if (fade_ms > 5000) {
        fade_ms = 5000; /* docs/protocol.md Sec.6.4 hard max */
    }
    uint32_t pct = ((uint32_t)level * 100u) / 255u;
    uint32_t duty = BYOK_BACKLIGHT_PCT_TO_DUTY(pct);

    esp_err_t err;
    if (fade_ms == 0) {
        err = ledc_set_duty((ledc_mode_t)BYOK_BACKLIGHT_LEDC_MODE,
                             (ledc_channel_t)BYOK_BACKLIGHT_LEDC_CHANNEL, duty);
        if (err == ESP_OK) {
            err = ledc_update_duty((ledc_mode_t)BYOK_BACKLIGHT_LEDC_MODE,
                                    (ledc_channel_t)BYOK_BACKLIGHT_LEDC_CHANNEL);
        }
    } else {
        err = ledc_set_fade_time_and_start((ledc_mode_t)BYOK_BACKLIGHT_LEDC_MODE,
                                            (ledc_channel_t)BYOK_BACKLIGHT_LEDC_CHANNEL,
                                            duty, fade_ms, LEDC_FADE_NO_WAIT);
    }
    if (err == ESP_OK) {
        s_backlight_level = level;
    }
    return err;
}

uint8_t byok_display_get_backlight(void)
{
    return s_backlight_level;
}

static esp_err_t set_contrast_impl(uint8_t value)
{
    uint32_t pct = ((uint32_t)value * BYOK_LCD_CONTRAST_PCT_MAX) / 255u;
    uint8_t reg = BYOK_LCD_CONTRAST_PCT_TO_REG(pct);

    CHK(lcd_send(false, BYOK_LCD_CMD_SET_CONTRAST));
    CHK(lcd_send(true, reg));
    s_contrast_value = value;
    return ESP_OK;
}

esp_err_t byok_display_set_contrast(uint8_t value)
{
    display_lock();
    esp_err_t err = set_contrast_impl(value);
    display_unlock();
    return err;
}

uint8_t byok_display_get_contrast(void)
{
    return s_contrast_value;
}
