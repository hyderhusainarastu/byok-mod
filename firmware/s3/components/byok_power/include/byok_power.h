/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_power.h — WAKE (GPIO6) button-hold power-off handler
 * ============================================================================
 *
 * Reproduces exactly one piece of stock behaviour: holding the front WAKE
 * button (hw_config.h BYOK_BTN_WAKE_GPIO = GPIO6, active-low, GPIO_FLOATING
 * -- board supplies its own external pull-up, BYOK_BTN_INTERNAL_PULLUP == 0)
 * for 3+ seconds, THEN RELEASING IT, drives the power-latch pin
 * (BYOK_POWER_HOLD_GPIO = GPIO42) to BYOK_POWER_OFF_LEVEL (1) and spins
 * forever, which is what actually cuts power via the board's external
 * power-hold circuit -- see firmware/common/hw_config.h Sec.9, which carries
 * the stock-image offsets for both levels (the same evidence app_main.c's
 * init_power_hold_gpio() already cites for the boot-time LOW drive).
 *
 * The wait-for-release step (added 2026-09-03) exists because WAKE is also
 * this board's power-ON button: driving the latch while WAKE is still
 * pressed drops power and then immediately looks like a fresh power-ON
 * press under the same still-held finger, so the device rebooted instead of
 * turning off, on the version of this handler that drove the latch the
 * instant the 3 s threshold fired (device observation, 2026-09-03). See
 * byok_power.c's byok_power_do_shutdown() for the debounced wait itself.
 *
 * Stock's "On USB power, can't power off" refusal (2026-09-03, 0.1.10):
 *   NOW reproduced. It reads USB attach/detach state off the TUSB320
 *   CC-chip status register (I2C address 0x60, register 0x09, hw_config.h
 *   Sec.2/BYOK_TUSB320_*) on I2C BUS 1 (BYOK_SYS_I2C_PORT) via
 *   byok_usb_cdc_usb_attached() -- a fresh read, not the USB re-attach
 *   watcher's cached state. byok_display still owns only BUS 0 (the panel,
 *   hw_config.h Sec.1); this is a separate bus/component, brought up by
 *   byok_usb_cdc_init(). Fails OPEN (proceeds with the power-off) on an
 *   unreadable state, deliberately -- see byok_power.c's
 *   byok_power_do_shutdown() comment for why. Before this, holding WAKE 3+ s
 *   on USB power drove GPIO42 HIGH unconditionally and browned the rail out
 *   from under a live VBUS, which re-established power and rebooted the
 *   board instead of turning it off (docs/troubleshooting.md, "Device
 *   won't stay off -- restarts on its own after a WAKE-hold power-off
 *   (USB connected)") -- that defect is what this refusal fixes.
 *
 * 0.1.12: the refusal message no longer permanently replaces whatever was
 *   on the glass. draw_usb_refusal() now saves the framebuffer first
 *   (byok_display_save_framebuffer()); after the message's 1.5 s on-screen
 *   window, byok_power_do_shutdown() restores it (byok_display_
 *   restore_framebuffer() + one byok_display_full_refresh()) before falling
 *   back through to normal polling. On-glass testing had left the
 *   message on the panel instead of restoring what was there before -- a
 *   cosmetic gap, now closed; docs/testing.md Sec.9 records the
 *   stock-parity shutdown behaviour that was confirmed.
 *
 * Deliberately NOT reproduced:
 *   - The factory-reset gesture (WAKE+EXECUTE held 10 s -> stock erases
 *     NVS, hw_config.h Sec.8 BYOK_FACTORY_RESET_HOLD_MS). This task polls
 *     ONLY GPIO6 (BTN_EXECUTE_GPIO/GPIO16 is untouched here -- it keeps its
 *     own boot-time-only check_boot_button() in app_main.c) and never calls
 *     nvs_flash_erase() or anything like it -- see SAFETY.md's hard rules
 *     and app_main.c's own NVS comment.
 *
 * Thread-safety: GPIO6 and GPIO42 are owned solely by
 * byok_power_button_task -- nothing else in this firmware touches either.
 * The display is different: it is normally owned by app_main's
 * dispatch_task (see byok_display.h), and byok_power_button_task's
 * on-trigger draws (draw_powering_off()/draw_usb_refusal()) are the one
 * place this firmware calls into the display from a task that doesn't own
 * it -- a known, accepted single-writer-contract violation, not a solved
 * one. It no longer needs solving AT THIS CALL SITE, though: byok_display
 * itself now takes an internal mutex around every public draw/refresh entry
 * point (2026-09-03, docs/troubleshooting.md §5's
 * "recommended follow-up", done same release) so a call from this task
 * cannot interleave an I2C transaction with a concurrent call from
 * dispatch_task -- see byok_display.h's own thread-safety note and
 * byok_display.c's s_display_mutex comment for the actual mechanism. A
 * PRIOR revision of this file wrapped byok_power.c's own draw_powering_off()
 * in vTaskSuspendAll()/xTaskResumeAll() to get roughly this same property;
 * that wrapper is exactly what caused the 2026-09-03 shutdown crash (a
 * scheduler-suspended nonzero-timeout I2C call unconditionally
 * configASSERTs in FreeRTOS) and has been removed -- see byok_power.c's
 * draw_powering_off() comment for the full trace. */
#ifndef BYOK_POWER_H
#define BYOK_POWER_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configures GPIO6 (input, floating -- external pull-up) and starts the
 * button-poll task (20 ms period, debounced). Call once from app_main,
 * after byok_hw_shim/gpio are available; does not require the display or
 * USB CDC to be up yet (see byok_power_set_display_ready()). Returns
 * whatever xTaskCreate's failure maps to (ESP_ERR_NO_MEM) if the task
 * cannot be created. A GPIO config failure is logged and returned (also
 * non-fatal to app_main, which logs-and-continues on any error from this
 * function -- matching this firmware's usual "diagnosable over USB beats a
 * boot-time abort" policy, app_main.c's own comments on
 * byok_display_init/byok_usb_cdc_init make the same tradeoff), but in that
 * case the poll task is deliberately NOT started: an unconfigured GPIO6
 * pad is not guaranteed to read released, and starting the task anyway
 * risks a phantom "held" reading silently shutting the device down ~3 s
 * after boot. */
esp_err_t byok_power_init(void);

/** Tells byok_power the display is safe to draw to -- mirrors app_main's own
 * s_display_ready gate around sd_updater_status_cb() (see app_main.c). Call
 * once, right after byok_display_init() returns ESP_OK. Before this is
 * called (or if display init failed and it never is), a triggered power-off
 * still logs and still drives the latch -- it just skips the on-screen
 * "POWERING OFF" message rather than risk calling into an uninitialised
 * display driver. */
void byok_power_set_display_ready(bool ready);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_POWER_H */
