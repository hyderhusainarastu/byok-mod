/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_idle.h — back-button idle-submode cycling + backlight cycling
 * ============================================================================
 *
 * Owns the four "back" buttons (hw_config.h Sec.8, all POLLED/active-low,
 * board-supplied external pull-ups, matching every other button poller in
 * this tree -- byok_power.c's WAKE handler, check_boot_button()'s boot-time
 * EXECUTE check):
 *
 *   UP         GPIO15  BYOK_BTN_UP_GPIO
 *   DOWN       GPIO7   BYOK_BTN_DOWN_GPIO
 *   EXECUTE    GPIO16  BYOK_BTN_EXECUTE_GPIO  (a SEPARATE, runtime poll of
 *                                              the same physical pin
 *                                              check_boot_button() already
 *                                              reads once at boot -- see
 *                                              this file's own "EXECUTE"
 *                                              note below)
 *   BRIGHTNESS GPIO11  BYOK_BTN_BRIGHTNESS_GPIO
 *
 * ALL FOUR are gated on "no host is active", read as byok_modes_get() !=
 * BYOK_MODE_DASHBOARD_USB (the device is already showing one of its own
 * idle screens, whether via the 30 s idle timeout, a manual SET_MODE, or
 * boot with no host ever having connected) -- while a host owns the
 * display, this firmware leaves all four alone (available for a future
 * EVT_BUTTON path, not implemented in this release, see main/app_main.c's
 * own header comment on that gap). The vendor's stock behaviour, as
 * observed on the device, cycles idle modes only when no host is active;
 * this firmware applies that same condition uniformly to all four buttons,
 * not just UP/DOWN, treating EXECUTE/BRIGHTNESS as part of the same
 * idle-mode-cycling behaviour rather than as separate, always-on controls
 * (docs/protocol.md Sec.6.4).
 *
 *   - UP / DOWN: cycle the idle submode CLOCK -> STATIC_NOTE -> BLANK ->
 *     CLOCK (UP = forward, DOWN = backward -- only the forward order is
 *     externally defined; DOWN's direction is this component's own,
 *     documented choice, the ordinary meaning of a two-button "cycle"
 *     control). Applies immediately (byok_modes_set())
 *     and persists the new submode (byok_nvs_set_idle_mode(), when armed).
 *   - EXECUTE / BRIGHTNESS: both cycle the backlight level forward through
 *     the vendor's own five stock percentages (hw_config.h Sec.4
 *     BYOK_BACKLIGHT_LEVELS_PCT = {0, 4, 20, 50, 100}, BYOK_BACKLIGHT_
 *     LEVEL_COUNT = 5 -- the same table the vendor's own BRIGHTNESS handler
 *     cycles, CONFIRMED by disassembly; this is OUR OWN re-implementation
 *     against OUR OWN persisted index, never touching the vendor's own
 *     "BLBR" NVS key). Applies immediately (byok_display_set_backlight())
 *     and persists the new index (byok_nvs_set_backlight_idx(), when
 *     armed) -- independent of which idle submode is currently showing
 *     (see "BLANK and the backlight" below).
 *
 * EXECUTE: this is a SEPARATE runtime gpio_config()+poll of GPIO16 from
 * check_boot_button()'s own one-shot boot-time hold-check (app_main.c) --
 * that function already reconfigures + reads the same pin once, early in
 * app_main(), for the unrelated "hold 3 s at boot -> BOOT_ORIGINAL" gesture
 * (D-011) and returns; nothing continues to own or poll GPIO16 after it
 * returns until this component's own task starts. Both configure the pin
 * identically (input, pulled up) so there is no conflict, just two
 * different consumers active at two different, non-overlapping times.
 *
 * BLANK and the backlight. Entering BYOK_MODE_BLANK (via UP/DOWN or an
 * explicit SET_MODE 6) clears the panel and forces the backlight OFF
 * (byok_display_set_backlight(0, ...)) as part of BLANK's own definition
 * ("panel cleared, backlight off") -- this does NOT change the persisted
 * backlight index; it is restored (byok_idle_restore_backlight()) the
 * instant BLANK is left, whether by cycling to CLOCK/STATIC_NOTE (UP/DOWN)
 * or by a host frame arriving (byok_clock.c's byok_clock_notify_host_frame(),
 * which now checks all three idle submodes, not only CLOCK). Pressing
 * EXECUTE/BRIGHTNESS *while* BLANK is showing still cycles and applies the
 * backlight live (by design, simplest to reason about: the button always
 * does what it says) -- which visibly un-blanks the backlight but leaves
 * BLANK's own cleared panel content in place; a further UP/DOWN both draws
 * something new and, for CLOCK/STATIC_NOTE, re-applies the (now-current)
 * persisted level anyway.
 * ============================================================================
 */
/* 0.1.14 (docs/protocol.md Sec.6.4b, Sec.6.5): the "gated on no host is
 * active" framing above is now more precise than "all four buttons are
 * inert while a host owns the display" -- while byok_modes_get() ==
 * BYOK_MODE_DASHBOARD_USB (host active) AND the preset menu (byok_menu) is
 * NOT open, UP/DOWN/BRIGHTNESS/EXECUTE-short-press now emit an outbound
 * EVT_BUTTON instead of doing nothing (byok_idle_set_button_event_cb(),
 * below) -- the "reserved for a future EVT_BUTTON implementation" gap this
 * header used to name. EXECUTE additionally distinguishes a short press
 * (< 1 s, same "emit an event, or idle-cycle the backlight" branch as
 * UP/DOWN/BRIGHTNESS) from a long press (>= 1 s, opens the preset menu via
 * byok_menu_open_default() -- works whether the device is host-active or
 * already idle; docs/protocol.md Sec.6.4b specifies the long-press menu
 * open for both).
 * Whenever byok_menu_is_open() is true, ALL FOUR buttons are instead routed
 * to the menu (UP/DOWN move the highlight, EXECUTE confirms the
 * selection) -- checked first, ahead of both the idle-cycle path and the
 * host-EVT_BUTTON path, so the menu always has input focus while open. See
 * byok_menu.h for the menu side of this and idle_button_task()'s own
 * updated comment in the .c file for the press/release/long-press edge
 * detection this needed. */
#ifndef BYOK_IDLE_H
#define BYOK_IDLE_H

#include <stdint.h>

#include "byok_modes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configures the four button GPIOs (input, floating -- board supplies its
 * own external pull-ups, hw_config.h Sec.8), loads the persisted idle
 * submode + backlight index (if CONFIG_BYOK_NVS_PERSIST_ARMED and either
 * was ever saved; CLOCK / index 4 [100%] otherwise), registers this
 * component's byok_modes change callback (STATIC_NOTE/BLANK entry
 * rendering + backlight handling -- see byok_idle.c), and starts the
 * button-poll task (20 ms period, 2-poll debounce, same shape as every
 * other button poller in this tree). Call once from app_main, after
 * byok_modes_init(), byok_display_init(), byok_nvs_init() and
 * byok_note_init(), and BEFORE byok_clock_init() (byok_clock.c's own idle-
 * timeout path calls byok_idle_enter_selected_submode(), so the selected
 * submode must already be loaded by the time that timeout can first fire).
 * Safe to call more than once (a second call is a no-op). Logged-and-
 * continue on a GPIO config failure, matching every other non-essential
 * bring-up step in app_main.c -- the poll task is simply not started in
 * that case (an unconfigured pad is not guaranteed to read released, and
 * the SAME failure-handling posture check_boot_button()/byok_power.c
 * already apply to their own button GPIOs). */
void byok_idle_init(void);

/** Applies byok_app_mode_t `submode` (must be BYOK_MODE_CLOCK,
 * BYOK_MODE_STATIC_NOTE, or BYOK_MODE_BLANK -- any other value returns
 * ESP_ERR_INVALID_ARG and does nothing) as the new selected idle submode:
 * updates the internal cycle position, calls byok_modes_set(), and
 * persists it (when armed) -- exactly what a debounced UP/DOWN press
 * does, minus the "which direction" step. Used by main/app_main.c's
 * SET_MODE dispatch for the new explicit mode values 4/5/6 (CLOCK/NOTE/
 * BLANK, docs/protocol.md Sec.6.4 v1.1 note) -- host-forced regardless of
 * byok_modes_get()'s current value, unlike the button path above, which
 * only fires while no host is active. */
esp_err_t byok_idle_force_submode(byok_app_mode_t submode);

/** Applies whichever idle submode is CURRENTLY selected (the same value
 * byok_idle_force_submode() last set, or the persisted/default one if
 * that was never called) via byok_modes_set() -- used for SET_MODE's
 * `mode=0` (IDLE) and byok_clock.c's own 30 s no-host-traffic timeout, so
 * both land on the owner's actual choice of idle screen instead of a
 * hard-coded CLOCK (docs/protocol.md Sec.6.4). A no-op call before
 * byok_idle_init() has run defaults to BYOK_MODE_CLOCK, matching this
 * firmware's pre-0.1.13 behaviour exactly. */
void byok_idle_enter_selected_submode(void);

/** Re-applies the persisted/current backlight index to the hardware
 * (byok_display_set_backlight()) without touching the stored index or the
 * idle submode -- used to undo BLANK's own backlight-off effect when
 * BLANK is left by a path other than a further UP/DOWN press (currently:
 * a host frame arriving while BLANK is showing, byok_clock.c). A no-op
 * (does not touch the display) if byok_display_is_ready() is false. */
void byok_idle_restore_backlight(void);

/** Registered by app_main.c at boot: called with (BYOK_BUTTON_* id,
 * BYOK_BUTTON_STATE_* state, uptime ms) whenever this component decides an
 * EVT_BUTTON should go out (docs/protocol.md Sec.6.5) -- i.e.
 * byok_modes_get() == BYOK_MODE_DASHBOARD_USB, the menu (byok_menu) is not
 * open, and one of UP/DOWN/BRIGHTNESS/EXECUTE was pressed or released, or
 * EXECUTE was held long enough to count as a long press (state ==
 * BYOK_BUTTON_STATE_LONGPRESS is fired instead of opening the menu only
 * when... see byok_idle.c's own on_execute_long()/on_execute_short()
 * comment for the exact split). `cb` may be NULL to unregister (the
 * default -- no events go out until app_main.c registers one). This
 * component does not itself know how to reach the host; it only decides
 * WHEN an event is warranted. */
typedef void (*byok_idle_button_event_cb_t)(uint8_t button, uint8_t state, uint32_t t_ms);
void byok_idle_set_button_event_cb(byok_idle_button_event_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_IDLE_H */
