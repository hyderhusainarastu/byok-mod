/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_modes.h — application-level mode state machine (SKELETON)
 * ============================================================================
 *
 * Originally a skeleton where only BYOK_MODE_DASHBOARD_USB did anything
 * (app_main dispatches DRAW_* / FRAME_* / refresh commands to byok_display
 * regardless of mode, which is the "host renders" path, D-013).
 * BYOK_MODE_CLOCK gained real rendering in byok-mod 0.1.12
 * (firmware/s3/components/byok_clock: a locally-rendered clock face, driven
 * by the PCF8563 RTC via byok_rtc, entered on SET_MODE(IDLE) or an idle-host
 * timeout and left the instant any host frame arrives -- see byok_clock.h's
 * own header comment for the full transition rules).
 *
 * 0.1.13: BYOK_MODE_STATIC_NOTE gained real rendering too
 * (firmware/s3/components/byok_note: word-wrapped SET_NOTE text, NVS-backed
 * when CONFIG_BYOK_NVS_PERSIST_ARMED is on) and a new value,
 * BYOK_MODE_BLANK, was added (panel cleared, backlight off) -- both driven
 * by the new firmware/s3/components/byok_idle component, which owns the
 * UP/DOWN/EXECUTE/BRIGHTNESS back-button poll task that cycles CLOCK -> NOTE
 * -> BLANK -> CLOCK while no host is connected (see byok_idle.h). Because
 * TWO components now need to react to a mode change (byok_clock for CLOCK
 * entry, byok_idle for NOTE/BLANK entry and exit), the single-callback-slot
 * API this header used to expose (byok_modes_set_change_cb(), replace-only)
 * was widened to a small fixed-size registry
 * (byok_modes_register_change_cb()/_unregister_change_cb(), see below) --
 * exactly the growth this comment block already anticipated ("a future
 * second user... would need... this API would need to grow multi-callback
 * support").
 *
 * This is NOT the same enum as docs/protocol.md Sec.6.4 SET_MODE
 * (IDLE/HOST/MIRROR/SLEEP), which is about *who owns the display bus*
 * (host vs. device, and whether the panel is even on) and is a link-level
 * concept the host controls directly. byok_app_mode_t is a higher-level
 * "what is this device doing right now" concept that outlives any one USB
 * connection (e.g. CLOCK should keep ticking with no host attached at all).
 * A rough correspondence, for when the two get wired together:
 *
 *   BYOK_MODE_DASHBOARD_USB  <-> protocol SET_MODE HOST or MIRROR
 *   BYOK_MODE_CLOCK          <-> protocol SET_MODE IDLE, or explicit 4 (CLOCK)
 *   BYOK_MODE_STATIC_NOTE    <-> protocol SET_MODE IDLE, or explicit 5 (NOTE)
 *   BYOK_MODE_BLANK          <-> protocol SET_MODE IDLE, or explicit 6 (BLANK)
 *   BYOK_MODE_ORIGINAL       <-> not a protocol SET_MODE at all -- this is
 *                                the transient state app_main sets just
 *                                before BOOT_ORIGINAL's esp_restart(), so a
 *                                STATUS/EVT_STATUS sent in the last instant
 *                                before reboot can honestly report it.
 *
 * IDLE (protocol mode 0) maps to whichever of CLOCK/STATIC_NOTE/BLANK the
 * owner last selected (back buttons or an explicit SET_MODE 4/5/6) --
 * byok_idle.h's byok_idle_enter_selected_submode(), not a hard-coded CLOCK
 * -- see that component for the full mapping and docs/protocol.md Sec.6.4.
 */
#ifndef BYOK_MODES_H
#define BYOK_MODES_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BYOK_MODE_DASHBOARD_USB = 0, /*!< Host (USB CDC) owns the display. Implemented. */
    BYOK_MODE_CLOCK         = 1, /*!< Device-local clock face (PCF8563 RTC). Implemented, 0.1.12. */
    BYOK_MODE_STATIC_NOTE   = 2, /*!< Device shows the stored SET_NOTE text, word-wrapped. Implemented, 0.1.13. */
    BYOK_MODE_ORIGINAL      = 3, /*!< Transient: about to hand off via BOOT_ORIGINAL. */
    BYOK_MODE_BLANK         = 4, /*!< Panel cleared, backlight off. Implemented, 0.1.13 (byok_idle). */
    BYOK_MODE_MENU          = 5, /*!< Preset menu overlay (byok_menu). Implemented, 0.1.14. Entered
                                   *   from ANY of the other modes (remembers which one to restore,
                                   *   see byok_menu.h) via a >=1s EXECUTE hold, protocol SET_MODE
                                   *   value 7, or automatically for 5s at boot -- never entered by
                                   *   byok_idle's own UP/DOWN/EXECUTE cycle table (byok_menu owns
                                   *   its own entry/exit, not that cycle). */
    BYOK_MODE_COUNT,
} byok_app_mode_t;

/** Hook signature for byok_modes_register_change_cb(). Called after every
 * byok_modes_set() that actually changes the mode (old != new), once per
 * registered slot, in registration order. */
typedef void (*byok_modes_change_cb_t)(byok_app_mode_t new_mode, byok_app_mode_t old_mode, void *ctx);

/** How many callbacks byok_modes_register_change_cb() can hold at once.
 * 0.1.13's two claimants (byok_clock's CLOCK-entry render, byok_idle's
 * NOTE/BLANK-entry-and-exit handling) need two; sized to 4 for headroom
 * without the array itself costing anything worth avoiding (each slot is
 * one function pointer + one void*). Registering a 5th (BYOK_MODES_MAX_
 * CALLBACKS-th) callback fails with ESP_ERR_NO_MEM rather than silently
 * dropping one of the first four. */
#define BYOK_MODES_MAX_CALLBACKS 4

/** Sets the initial mode (normally BYOK_MODE_DASHBOARD_USB) and clears every
 * registered change callback (so a re-init, if one ever happens, doesn't
 * accumulate stale registrations). Call once from app_main, before any
 * component calls byok_modes_register_change_cb(). */
esp_err_t byok_modes_init(byok_app_mode_t initial);

/** Current mode. */
byok_app_mode_t byok_modes_get(void);

/** Transitions to `mode`. Returns ESP_ERR_INVALID_ARG for a value >=
 * BYOK_MODE_COUNT. Invokes every registered change callback (if any, in
 * registration order) after updating the stored mode -- but only when the
 * mode actually changes (old == new is a silent no-op, no callback fires).
 * No mode refuses a transition on its own. */
esp_err_t byok_modes_set(byok_app_mode_t mode);

/** Adds `cb` (with `ctx`) to the change-notification registry -- up to
 * BYOK_MODES_MAX_CALLBACKS at once. Returns ESP_ERR_INVALID_ARG if cb is
 * NULL, ESP_ERR_NO_MEM if the registry is already full. Each callback
 * should ignore mode transitions it doesn't care about (switch on
 * `new_mode`, return early otherwise) -- see byok_clock.c/byok_idle.c for
 * the pattern. */
esp_err_t byok_modes_register_change_cb(byok_modes_change_cb_t cb, void *ctx);

/** Removes `cb` from the registry, if present (a no-op otherwise). Compares
 * function pointers only -- if the same function pointer was registered
 * with two different `ctx` values (not done anywhere in this tree), this
 * removes whichever slot matches first. */
void byok_modes_unregister_change_cb(byok_modes_change_cb_t cb);

/** Short ASCII name, for logging / EVT_LOG / a future boot-banner line.
 * Never NULL, even for an out-of-range value ("?"). */
const char *byok_modes_name(byok_app_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_MODES_H */
