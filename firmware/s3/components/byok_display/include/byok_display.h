/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_display.h — UC1611-class LCD driver (BYOK v2.1 panel)
 * ============================================================================
 *
 * Drives the panel exactly as reverse-engineered in
 * firmware/common/hw_config.h Sec.3 (geometry, reset preamble, 21-entry init
 * table, window-program partial-update path, contrast) and Sec.4 (LEDC
 * backlight). Every wire-level detail (I2C addresses, opcodes, the
 * page/column framebuffer layout) is graded CONFIRMED/STRONGLY INDICATED
 * there; this driver does not invent new hardware behaviour, only a C API
 * around it plus the pixel-level drawing primitives the BYOK Link protocol
 * (docs/protocol.md Sec.6.2) needs.
 *
 * Framebuffer convention (internal, "hw convention"): one back buffer,
 * BYOK_DISPLAY_FB_BYTES (2400) bytes, byte-indexed by
 * BYOK_DISPLAY_FB_INDEX(x,y) from hw_config.h. Bit value 1 = pixel OFF
 * (light) -- this is CONFIRMED from the vendor's own blank pattern (0xFF)
 * plus "Inverse Display ON" in the init table (hw_config.h Sec.3, "3.2").
 * Every function in this header takes/returns pixels in the PROTOCOL's
 * convention instead (docs/protocol.md Sec.7.2: 1 = pixel on / dark) --
 * the inversion happens once, at the boundary, inside this driver.
 *
 * Thread-safety (revised 0.1.10, docs/troubleshooting.md
 * Sec.5): every public entry point that touches the framebuffer or issues
 * an I2C transaction (byok_display_clear/_draw_text/_fill_rect/_draw_bitmap/
 * _load_full_raster_1bpp/_full_refresh/_partial_refresh/_set_contrast) now
 * takes an internal FreeRTOS mutex for its own duration, so calls from
 * different tasks (app_main's dispatch task, and byok_power's WAKE-hold
 * shutdown handler) cannot interleave mid-transaction -- see byok_display.c's
 * own comment above s_display_mutex for the exact contract and why a plain
 * (non-recursive) mutex is safe here. This still does NOT make a caller's
 * own MULTI-CALL sequence atomic (e.g. clear() then draw_text() then
 * full_refresh() can still have another task's single call land between
 * them -- a possible torn frame, not a torn transaction); callers that need
 * a whole sequence to appear atomic must still coordinate at a higher level.
 * Backlight (byok_display_set_backlight/_get_backlight) and contrast/ack-
 * probe/polarity GETTERS are unaffected -- see byok_display.c.
 */
#ifndef BYOK_DISPLAY_H_
#define BYOK_DISPLAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -- geometry / capability constants, re-exported from hw_config.h so
 * callers (app_main's HELLO_ACK/INFO builders) don't need to reach into the
 * shim header themselves. -- */
#define BYOK_DISPLAY_W          240u
#define BYOK_DISPLAY_H          80u
#define BYOK_DISPLAY_BPP_NATIVE 1u
#define BYOK_DISPLAY_FONT_COUNT 1u  /* byok_font8x8 -- see byok_font8x8.h */

/* Rect-draw ops, docs/protocol.md Sec.6.2 DRAW_RECT.op */
#define BYOK_RECT_OP_OUTLINE       0u
#define BYOK_RECT_OP_FILLED        1u
#define BYOK_RECT_OP_INVERT        2u
#define BYOK_RECT_OP_CLEAR_REGION  3u

/* Bitmap composite ops, docs/protocol.md Sec.6.2 DRAW_BITMAP.op */
#define BYOK_BITMAP_OP_COPY 0u
#define BYOK_BITMAP_OP_OR   1u
#define BYOK_BITMAP_OP_AND  2u
#define BYOK_BITMAP_OP_XOR  3u

/* Brings up the I2C bus + both device addresses, runs the reset preamble and
 * the 21-entry init table, sets up the LEDC backlight channel, and clears
 * the back buffer to blank. Does one initial full_refresh() so the panel
 * shows blank rather than whatever power-on noise the controller RAM held,
 * then (0.1.8) sends the vendor's display-enable pair (hw_config.h
 * Sec.3.2.1: CMD 0xC9 / DATA 0xAD) right where gdisp_init() puts it --
 * immediately after that first frame reaches display RAM. Safe to call
 * exactly once, from app_main, before any other byok_display_* call. */
esp_err_t byok_display_init(void);

/** True once byok_display_init() has brought up the I2C bus/devices, the
 * back buffer and the reset+init-table sequence far enough that every other
 * public entry point in this header is safe to call (framebuffer allocated,
 * mutex created, device handles valid) -- false before that point. Note
 * this can read true for a few more instructions than byok_display_init()
 * itself takes to RETURN ESP_OK: the flag is set just before that function's
 * own trailing initial-full-refresh/display-enable steps, so a caller
 * checking THIS getter from another task, racing that narrow window, could
 * see "ready" a moment before those two steps finish (both non-fatal if
 * they fail -- the panel just might not visibly update yet, never a crash
 * or a call into an invalid handle). Added 0.1.12 so a caller that does not
 * own byok_display_init()'s own call site (byok_clock, rendering the CLOCK
 * mode screen) can check readiness itself instead of needing its own copy
 * of a flag -- mirrors the convention app_main.c already applied by hand
 * for byok_power (byok_power_set_display_ready()), now available
 * generically. A single bool read, safe from any task without the display
 * mutex (it only ever transitions false->true once and is never cleared
 * again). */
bool byok_display_is_ready(void);

/* -- back-buffer drawing (docs/protocol.md Sec.6.2). None of these touch the
 * panel -- call byok_display_full_refresh() / byok_display_partial_refresh()
 * afterwards (Sec.7.1: "nothing appears on the panel until a refresh
 * happens"). -- */

/** CLEAR: value false = all pixels off (light), true = all pixels on (dark). */
void byok_display_clear(bool dark);

/** DRAW_TEXT payload already split into fields by the caller (app_main),
 * text as an ASCII-decoded string (UTF-8 codepoints outside the placeholder
 * font already collapsed to the fallback glyph by byok_font8x8_lookup).
 * font_id is validated by the caller against BYOK_DISPLAY_FONT_COUNT;
 * style bit0 = inverted, bit1 = wrap at panel edge, bit2 = clip instead of
 * wrap (bit1 takes precedence if both are set; docs/protocol.md Sec.6.2
 * reserves the remaining style bits and does not define what setting both
 * means, so this component fixes the precedence itself). */
void byok_display_draw_text(uint16_t x, uint16_t y, uint8_t style,
                             const char *ascii, size_t len);

/** DRAW_RECT. value is the pixel value used by op 0/1 (false=light,
 * true=dark); ignored for op 2/3. */
void byok_display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             uint8_t op, bool value);

/** DRAW_BITMAP / the FRAME_END raster loader's shared primitive. `packed`
 * is exactly the wire format of docs/protocol.md Sec.7.2 (row-major,
 * MSB-leftmost, 1 or 2 bpp, stride = ceil(w*bpp/8)); `packed_len` must equal
 * that stride * h (RLE must already be decoded by the caller). 2bpp input is
 * down-converted (value < 2 => dark) since the panel's native bpp is 1 --
 * see docs/protocol.md Sec.6.6 capability bit 1, which this device does NOT
 * set. Returns false (caller should NACK/E_BAD_LENGTH) if packed_len does
 * not match the expected size for w/h/bpp. */
bool byok_display_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               uint8_t bpp, uint8_t op,
                               const uint8_t *packed, size_t packed_len);

/** Loads a full-panel (240x80, 1bpp) row-major raster -- exactly the
 * FRAME_BEGIN/FRAME_DATA/FRAME_END reassembly buffer, in wire convention --
 * as the entire back buffer in one pass. `raster_len` must be
 * ceil(BYOK_DISPLAY_W/8)*BYOK_DISPLAY_H (30*80 = 2400). This is the only
 * FRAME_* shape this driver supports; app_main NACKs
 * (E_UNSUPPORTED) any FRAME_BEGIN whose w/h/bpp don't match, and any partial
 * frame (flags bit0) -- see docs/protocol.md Sec.6.3's own "v1 note" about
 * how awkward that path is; it is not implemented in this release. */
bool byok_display_load_full_raster_1bpp(const uint8_t *raster, size_t raster_len);

/** Copies the current back buffer (hw convention -- see this header's own
 * "Framebuffer convention" note; NOT protocol/pixel-on convention, since
 * this is a raw byte-for-byte snapshot meant only to be handed back to
 * byok_display_restore_framebuffer(), never interpreted by the caller) into
 * `out`. `out_len` must be exactly hw_config.h's BYOK_DISPLAY_FB_BYTES
 * (2400, `BYOK_DISPLAY_W * BYOK_DISPLAY_H / 8`) -- returns false (no copy
 * performed) otherwise. Added 0.1.12 for byok_power's USB-power-refusal
 * message (docs/protocol.md has no wire concept of this -- it is purely an
 * internal save/restore pair for one caller that needs to interrupt
 * whatever is on the glass, then put it back). Takes the display mutex for
 * its own duration, same as every other public entry point here. */
bool byok_display_save_framebuffer(uint8_t *out, size_t out_len);

/** Replaces the back buffer with a previously-saved snapshot (as from
 * byok_display_save_framebuffer()). `buf_len` must be exactly
 * BYOK_DISPLAY_FB_BYTES -- returns false (no copy performed) otherwise.
 * Does NOT itself refresh the panel -- the back buffer and the panel can
 * differ until the caller's own following byok_display_full_refresh() (or
 * _partial_refresh()) call, same "nothing appears until a refresh happens"
 * contract as every other back-buffer-only function in this header
 * (Sec.7.1). Takes the display mutex for its own duration. */
bool byok_display_restore_framebuffer(const uint8_t *buf, size_t buf_len);

/* -- refresh (Sec.7.1, Sec.6.2 PARTIAL_REFRESH/FULL_REFRESH) -- */

/** FULL_REFRESH. force_reinit replays the whole reset + 21-entry init
 * sequence first (the "unstick a confused panel" escape, expected to be
 * slow) before pushing the full back buffer, and (0.1.8) then re-sends the
 * display-enable pair (hw_config.h Sec.3.2.1) -- force_reinit reproduces the
 * vendor's whole gdisp_init() bring-up shape, so it gets that step too, same
 * as byok_display_init()'s own initial refresh. */
esp_err_t byok_display_full_refresh(bool force_reinit);

/** PARTIAL_REFRESH. Rectangle is clamped to the panel and silently enlarged
 * to the controller's page (8-row) granularity, per docs/protocol.md
 * Sec.6.3. */
esp_err_t byok_display_partial_refresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* -- backlight / contrast (Sec.6.4) -- */

/** SET_BACKLIGHT. level 0-255 (0 = off). fade_ms 0 = immediate, else an
 * LEDC hardware fade, clamped to <= 5000 ms per the protocol's own limit. */
esp_err_t byok_display_set_backlight(uint8_t level, uint16_t fade_ms);

/** Current backlight level, 0-255, for STATUS/EVT_STATUS. */
uint8_t byok_display_get_backlight(void);

/** SET_CONTRAST. value 0-255, mapped through the vendor's 0-99 percent
 * domain to the panel's 0-255 VBIAS register (hw_config.h
 * BYOK_LCD_CONTRAST_PCT_TO_REG); the round trip is near-identity with
 * ~100 distinct steps. */
esp_err_t byok_display_set_contrast(uint8_t value);

/** Current contrast, 0-255, for STATUS/EVT_STATUS. */
uint8_t byok_display_get_contrast(void);

/* -- 0.1.14: runtime per-byte/bulk write-path toggle (docs/protocol.md
 * Sec.6.4b DISPLAY_CFG). components/byok_display/Kconfig BYOK_DISPLAY_
 * BULK_WRITES used to be a BUILD-time choice between two mutually
 * exclusive #if branches in flush_rect() -- one compiled in, the other
 * not. Both paths are now ALWAYS compiled in; this bool just selects which
 * one flush_rect() takes on its next call, so a host can A/B the bulk
 * path's real refresh time (byok_display_get_last_full_refresh_us()) live,
 * on real hardware, without a rebuild. The Kconfig option still exists and
 * still sets the BOOT-time initial value of this flag (sdkconfig.defaults
 * currently pins it off -- CONFIG_BYOK_DISPLAY_BULK_WRITES=n, see that
 * file's own long-standing rationale: the bulk path was never exercised on
 * real pixels until 0.1.14 confirmed the panel itself works), so the
 * shipped default is still per-byte/vendor-parity unless a host opts in
 * over the link. -- */

/** true = bulk I2C transfers (one transaction per full-width page, or per
 * whole full-width window), false = one i2c_master_transmit() per
 * framebuffer byte (vendor-parity fallback). Takes effect on the NEXT
 * flush_rect() call (i.e. the next _full_refresh()/_partial_refresh()) --
 * does not touch the panel or re-send anything itself. */
void byok_display_set_bulk_writes(bool enable);

/** Current write-path setting, for GET_INFO/logging -- mirrors the last
 * byok_display_set_bulk_writes() call, or CONFIG_BYOK_DISPLAY_BULK_WRITES's
 * boot-time value if that was never called. */
bool byok_display_get_bulk_writes(void);

/* -- refresh timing / I2C health (added for the bulk-write rework; see
 * components/byok_display/Kconfig BYOK_DISPLAY_BULK_WRITES). Neither
 * docs/protocol.md's INFO (64 B, fully allocated) nor STATUS (20 B, fully
 * allocated) payload has reserved room for these -- both are laid out with
 * every byte assigned and Sec.6.6's own reserved bits are for the
 * capability bitmap, not a place to smuggle unrelated fields. So these are
 * DEBUG-log-only for now; a v2 of the protocol should give them real wire
 * fields (or an
 * EVT_STATUS-like diagnostic event) if a host ever needs to read them
 * live instead of off the serial log. -- */

/** Wall-clock duration (esp_timer, microseconds) of the most recent
 * byok_display_full_refresh() call's actual I2C flush (window program +
 * data write), not counting an optional force_reinit's reset/init replay.
 * 0 before the first full refresh. */
uint32_t byok_display_get_last_full_refresh_us(void);

/** Same, for the most recent byok_display_partial_refresh() call. 0 before
 * the first partial refresh. */
uint32_t byok_display_get_last_partial_refresh_us(void);

/** Cumulative count of I2C transactions (command or data, bulk or
 * per-byte) that returned anything other than ESP_OK since boot. The
 * driver does not stop or retry on an I2C error -- callers (refresh
 * functions, the boot self-test) keep going and this counter is how a
 * human finds out afterwards that something on the bus misbehaved. */
uint32_t byok_display_get_i2c_error_count(void);

/** 0.1.5: cumulative count of i2c_master_transmit() CALLS issued to either
 * device address (command 0x38 or data 0x39), bulk or per-byte, since boot
 * -- regardless of result. Together with byok_display_get_i2c_error_count()
 * this lets a caller (the boot self-test) log "N transactions issued, M of
 * them non-OK" per phase, not just the error count alone -- added per
 * docs/troubleshooting.md's follow-up device evidence
 * (0.1.3 fully up, DRAW_TEXT/IMAGE/contrast-sweep all ACK with 0 logged I2C
 * errors, but nothing ever appears on the glass) so a real boot's log can
 * show whether the expected number of transactions was even attempted. */
uint32_t byok_display_get_i2c_txn_count(void);

/** 0.1.8: cumulative count of PAYLOAD BYTES written to the panel (command
 * bytes + data bytes) across every transaction counted by
 * byok_display_get_i2c_txn_count(), since boot. txn_count / byte_count is a
 * measured transactions-per-byte ratio -- 1.0 on this driver's vendor-parity
 * per-byte path (hw_config.h Sec.1 BYOK_LCD_I2C_BYTES_PER_XFER=1,
 * CONFIG_BYOK_DISPLAY_BULK_WRITES=n) -- added for the boot self-test log,
 * and cross-checked against docs/display.md Sec.3.1's transaction table. */
uint32_t byok_display_get_i2c_byte_count(void);

/* -- panel ACK probe (0.1.6) -- see byok_display.c's lcd_ack_probe() for
 * the full design rationale. Diagnostic only: NOT_RUN means
 * byok_display_init() has not reached that point yet (or failed before
 * getting there); it never blocks or fails init either way. -- */
typedef enum {
    BYOK_ACK_PROBE_NOT_RUN = 0,
    BYOK_ACK_PROBE_ACKED   = 1,
    BYOK_ACK_PROBE_NO_ACK  = 2,
} byok_ack_probe_result_t;

/** Result of the ACK probe run immediately before the reset preamble
 * (before any command has been sent to the panel this boot). */
byok_ack_probe_result_t byok_display_get_ack_probe_pre_init(void);

/** Result of the ACK probe run immediately after the reset preamble + the
 * 21-entry init table (whether or not that sequence itself reported
 * ESP_OK). */
byok_ack_probe_result_t byok_display_get_ack_probe_post_init(void);

/* -- command/data address polarity (0.1.7) -- docs/display.md Sec.6.
 * Re-derivation found NO inversion (CONFIRMED: 16/19 init-table opcodes
 * match the real UC1611 datasheet exactly, C/D bit included) -- this exists
 * to also gather independent on-device evidence, per
 * components/byok_display/Kconfig BYOK_DISPLAY_POLARITY_EXPERIMENT's help
 * text. Affects lcd_send()/lcd_send_bulk_data() only -- both I2C device
 * handles (0x38, 0x39) always exist; this selects which one each treats as
 * "command" and which as "data" for every subsequent call, including a
 * later byok_display_full_refresh(true)'s own reset+init+window-program
 * replay. Does not itself touch the panel or re-run init -- callers must
 * still call byok_display_full_refresh(true) (or equivalent) afterwards for
 * the new mapping to actually reach the wire. -- */

/** false (default) = cmd 0x38 / data 0x39 (vendor-parity mapping, this
 * driver's mapping since 0.1.0). true = SWAPPED: cmd 0x39 / data 0x38. */
void byok_display_set_polarity(bool swapped);

/** Current polarity mapping, for logging -- mirrors byok_display_set_polarity()'s
 * last call (false before it is ever called). */
bool byok_display_get_polarity_swapped(void);

/** UC1611 STATUS READ probe (0.1.7): attempts i2c_master_receive() of one
 * byte from BOTH the 0x38 and 0x39 device handles (not affected by the
 * polarity setting above -- this always probes the two physical addresses
 * directly) and logs the byte or the error for each, tagged with `when`.
 * Per the UC1611 datasheet's "Get Status" row (C/D=0, W/R=1), a real status
 * byte is only expected at the command address -- reading the data address
 * is included anyway per the experiment's own design (independent evidence,
 * not an assumption about which address will answer). Both device handles
 * have ACK checking disabled for vendor parity (see lcd_ack_probe()'s own
 * comment in byok_display.c for what that means for reads: a NACK'd address
 * may not be reported as an error, so a returned byte is not on its own
 * proof the panel actually drove it). Diagnostic only -- never fails,
 * always logs, safe to call at any point after byok_display_init(). */
void byok_display_status_read(const char *when);

/** Runs the command/data polarity A/B/C experiment described in
 * components/byok_display/Kconfig BYOK_DISPLAY_POLARITY_EXPERIMENT's help
 * text, in place of byok_display_run_boot_selftest() when that option is
 * enabled. Must be called after byok_display_init() has already run once
 * (so the panel has already been through one normal init under the default
 * mapping) -- each pass then calls byok_display_full_refresh(true) itself
 * to force a fresh reset+init+window-program replay under whatever mapping
 * byok_display_set_polarity() was just set to. Leaves the panel on pass C's
 * mapping (= the current/vendor-parity mapping, CONFIRMED correct in
 * docs/display.md Sec.6) with the "POLARITY A OK?" banner -- a string this
 * firmware itself draws -- as the resting screen; same
 * "caller does not need to draw anything else afterwards" contract as
 * byok_display_run_boot_selftest(). */
void byok_display_run_polarity_experiment(const char *fw_version);

/* -- boot self-test -- */

/** Runs the boot-time self-test pattern sequence: an 8px checkerboard held
 * for ~2 s, then a 1px border + "BYOK LAB" + "240X80 1BPP" + `fw_version` +
 * "RST:`reset_reason_str`" + "USB: WAITING FOR HOST" (font is uppercase-only
 * -- see byok_font8x8.h -- so `fw_version` is upper-cased on screen; pass it
 * as produced by esp_app_desc_t verbatim; `reset_reason_str` is drawn as
 * given, so pass it already upper-case/short if that matters to the caller
 * -- byok-mod 0.1.2 onward passes a short fixed token such as "POWERON/EN"
 * or "SW", see app_main.c's byok_reset_reason_str(); the USB line is a
 * fixed string, not a live status -- see byok_display_selftest.c's own
 * comment at that line; 0.1.6 adds one more row, "PANEL NO ACK", drawn only
 * when byok_display_get_ack_probe_pre_init()/_post_init() reports
 * BYOK_ACK_PROBE_NO_ACK for either probe -- diagnostic text, not a claim
 * that drawing itself failed, since the two device handles this driver's
 * own drawing path uses have ACK checking disabled for vendor parity and
 * so cannot detect a no-ACK panel themselves), then a horizontal/vertical line
 * test, then a screen reporting whether BOOT_ORIGINAL (EXECUTE-held-at-boot
 * / the protocol command) is actually armed in this build
 * (`boot_original_gate_open` — CONFIG_BYOK_ALLOW_OTADATA_WRITE's runtime
 * value, so the owner sees the gate state on the glass, not just in a log
 * they may not be watching). Each phase's wall-clock duration is logged
 * (esp_timer), and (0.1.5) so is the exact number of I2C transactions that
 * phase issued and whether any of them returned non-OK -- see
 * byok_display_get_i2c_txn_count(). Must be called after byok_display_init(). Requires
 * byok_display_init() to have already run; does not itself call
 * byok_display_init(). Leaves the border+text screen on the panel
 * afterwards as the boot-complete resting state -- the caller does not
 * need to draw anything else once this returns. If any I2C transaction
 * inside a phase fails, the phase keeps going (matching this driver's
 * usual no-retry-no-abort policy) and the failure is reflected afterwards
 * in byok_display_get_i2c_error_count(); this function itself has no
 * failure return because "the self-test observed an I2C error" is not a
 * reason to stop running the rest of the self-test.
 *
 * `reset_reason_str` may be NULL (renders as "?"), matching `fw_version`'s
 * own existing NULL handling -- see docs/recovery.md Sec.4/Sec.8
 * item 2: this is the on-glass half of the esp_reset_reason() diagnostic,
 * the only channel that still works when the USB-Serial-JTAG console is
 * dead. */
void byok_display_run_boot_selftest(const char *fw_version, bool boot_original_gate_open,
                                     const char *reset_reason_str);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_DISPLAY_H_ */
