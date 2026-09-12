/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_menu.h — preset-menu overlay (byok_modes BYOK_MODE_MENU)
 * ============================================================================
 *
 * Owns the preset list (docs/protocol.md Sec.6.4b SET_PRESETS) and the menu
 * screen it drives: up to BYOK_PRESET_MAX_COUNT (8, byok_nvs.h) names, each
 * up to BYOK_PRESET_NAME_LEN (20) bytes, one per row, the current highlight
 * row drawn inverted (byok_display_draw_text() style bit0). Persisted under
 * our own NVS namespace ("byokmod", byok_nvs.h keys "presets"/"preset")
 * when CONFIG_BYOK_NVS_PERSIST_ARMED is on -- in-RAM only otherwise, same
 * "fully usable this session, not remembered across a reboot" posture as
 * every other byokmod-backed feature (byok_note, byok_idle).
 *
 * Entry points (docs/protocol.md Sec.6.4 SET_MODE value 7 / Sec.6.4b):
 *   (a) automatically at boot, for BYOK_MENU_BOOT_TIMEOUT_MS (5 s), if any
 *       preset is loaded -- byok_menu_open_boot(), called once from
 *       app_main() after byok_menu_init() and byok_idle_init() have both
 *       run. Skipped entirely (no-op) if the preset list is empty.
 *   (b) EXECUTE (GPIO16) held >= 1 s while no OTHER button-driven action
 *       owns it -- byok_idle.c's own long-press handling calls
 *       byok_menu_open_default() (BYOK_MENU_TIMEOUT_MS, 10 s). Works
 *       whether the device is currently host-active (BYOK_MODE_DASHBOARD_
 *       USB) or already showing one of its own idle screens -- byok_menu
 *       records whichever byok_modes_get() value was current (`s_prev_
 *       mode`) and restores exactly that on close.
 *   (c) protocol SET_MODE(mode=7) -- app_main.c's dispatch() calls
 *       byok_menu_open_default() directly (also 10 s).
 *
 * UP/DOWN move an inverted highlight row (byok_menu_move(), wrapping);
 * EXECUTE selects it (byok_menu_select()) -- both routed here from
 * byok_idle.c's button task, which checks byok_menu_is_open() FIRST, ahead
 * of its own idle-submode-cycle / host-EVT_BUTTON handling, so the menu
 * always wins input focus while it is open. A selection persists the new
 * index (byok_nvs_set_preset_idx(), when armed) and fires the registered
 * byok_menu_preset_changed_cb_t (app_main.c wires this to an outbound
 * EVT_PRESET_CHANGED, docs/protocol.md Sec.6.5) before restoring the
 * previous mode. No selection made in BYOK_MENU_TIMEOUT_MS/BYOK_MENU_BOOT_
 * TIMEOUT_MS closes the menu WITHOUT changing the selection (or firing the
 * callback) -- docs/protocol.md Sec.6.4b specifies that a 10 s timeout
 * with no confirmation closes the menu and keeps the current selection,
 * with no persistence write and no event.
 *
 * Display ownership while open: app_main.c's dispatch() skips the actual
 * byok_display_* call (but still validates/ACKs/NACKs exactly as
 * documented) for every drawing/refresh handler while byok_modes_get() ==
 * BYOK_MODE_MENU -- the same "device shows its own screen; host frames
 * [accepted but] ignored" posture protocol.md Sec.6.4 already documents
 * for link-level SET_MODE(IDLE). See app_main.c's own dispatch() comment
 * at each gated case.
 *
 * byok_menu_abort(): any code path OTHER than this component that is about
 * to force byok_modes_get() away from BYOK_MODE_MENU (today: only
 * app_main.c's SET_MODE dispatch, for an explicit mode value 0-6 arriving
 * while the menu happens to be open) MUST call this first, so byok_menu's
 * own `s_open`/deadline bookkeeping does not go stale relative to the mode
 * a byok_menu_tick() timeout would otherwise try to "restore" later. See
 * app_main.c's own call site comment.
 * ============================================================================
 */
#ifndef BYOK_MENU_H
#define BYOK_MENU_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Normal (button/SET_MODE-triggered) menu auto-close timeout. Sec.6.4b of
 * docs/protocol.md specifies 10 s, closing on the current selection. */
#define BYOK_MENU_TIMEOUT_MS 10000u

/** Boot-only auto-close timeout. Sec.6.4b of docs/protocol.md specifies
 * that the menu is shown at boot for 5 s, immediately after the boot
 * self-test, and skipped entirely when no preset is stored. */
#define BYOK_MENU_BOOT_TIMEOUT_MS 5000u

/** Loads the persisted preset list + selected index (if CONFIG_BYOK_NVS_
 * PERSIST_ARMED and either was ever saved; an empty list / index 0
 * otherwise) and starts this component's own timeout-watch task (200 ms
 * poll, closes an open menu once its deadline passes -- see
 * byok_menu_tick()). Does NOT open the menu itself -- call
 * byok_menu_open_boot() separately for the boot-time 5 s screen. Call once
 * from app_main, after byok_nvs_init() and byok_display_init(), before
 * byok_idle_init() (byok_idle.c's button task calls byok_menu_is_open()/
 * _move()/_select() from its very first poll). Safe to call more than once
 * (a second call is a no-op). */
void byok_menu_init(void);

/** Registers `cb` to be called with the newly-selected index exactly once
 * per COMPLETED selection (byok_menu_select(), never on a timeout-without-
 * a-change). Replaces any previously-registered callback (single slot --
 * this component has exactly one caller, app_main.c, unlike byok_modes'
 * multi-slot registry). May be called with cb == NULL to unregister. */
typedef void (*byok_menu_preset_changed_cb_t)(uint8_t index);
void byok_menu_set_preset_changed_cb(byok_menu_preset_changed_cb_t cb);

/** SET_PRESETS handler body: `count` (<= BYOK_PRESET_MAX_COUNT, from
 * byok_nvs.h) presets, `names` pointing at `count * BYOK_PRESET_NAME_LEN`
 * raw wire bytes (each entry NUL-padded, not necessarily NUL-terminated at
 * a shorter length). Replaces the in-RAM list, clamps the selected/
 * highlighted index into the new (possibly shorter) range, persists the
 * list (when armed), and re-renders immediately if the menu is currently
 * open. Returns ESP_ERR_INVALID_ARG if count > BYOK_PRESET_MAX_COUNT or
 * names is NULL with count > 0 -- the caller (app_main.c's dispatch())
 * should NACK/E_BAD_PARAM on that, matching every other SET_* handler's
 * own validation split (framing-level length checked at the call site,
 * semantic range checked here). */
esp_err_t byok_menu_set_presets(uint8_t count, const uint8_t *names);

/** True if at least one preset is currently loaded -- byok_menu_open_boot()
 * and byok_menu_open_default() are both no-ops when this is false ("skipped
 * if no presets stored"). */
bool byok_menu_has_presets(void);

/** Opens the menu (records byok_modes_get() as the mode to restore,
 * switches to BYOK_MODE_MENU, renders) with a BYOK_MENU_BOOT_TIMEOUT_MS (5
 * s) auto-close. No-op if already open or byok_menu_has_presets() is
 * false. Intended for exactly one call, from app_main() at boot. */
void byok_menu_open_boot(void);

/** Opens the menu (same mechanics as byok_menu_open_boot()) with a
 * BYOK_MENU_TIMEOUT_MS (10 s) auto-close -- used by SET_MODE(mode=7) and
 * byok_idle.c's EXECUTE long-press handler. No-op if already open or
 * byok_menu_has_presets() is false. */
void byok_menu_open_default(void);

/** True while the menu is currently showing. Checked FIRST by byok_idle.c's
 * button task, ahead of its own idle-cycle/host-EVT_BUTTON handling. */
bool byok_menu_is_open(void);

/** Moves the highlight row by `delta` (+1 from byok_idle.c's UP, -1 from its
 * DOWN), wrapping within [0, preset_count). `delta` is SUBTRACTED from the
 * index internally (0.1.15, CHANGELOG.md 0.1.14 Known Issue -- see this
 * function's own body comment): render() draws row 0 at the top and
 * increasing index further down the panel, so UP's +1 must decrement the
 * index (move the highlight up/toward row 0) and DOWN's -1 must increment
 * it (move it down/toward the last row) -- i.e. UP walks the highlight up
 * the screen and DOWN walks it down, matching the buttons' physical
 * position. No-op if the menu is not open or has no presets. Re-renders
 * immediately. */
void byok_menu_move(int delta);

/** Commits the highlighted row as the new selection: persists it (when
 * armed), fires the registered byok_menu_preset_changed_cb_t (if any and
 * if the index actually changed), and closes the menu (restoring the mode
 * byok_menu_open_boot()/_open_default() recorded). No-op if the menu is
 * not open. */
void byok_menu_select(void);

/** Currently PERSISTED/selected preset index (0 .. count-1; 0 if no
 * selection has ever been made or count is 0) -- for STATUS's reserved
 * flag bits, docs/protocol.md Sec.6.1. NOT the same as the highlight row
 * while the menu is open and not yet confirmed. */
uint8_t byok_menu_get_selected_index(void);

/** Immediately closes the menu WITHOUT restoring any mode and WITHOUT
 * touching the selection or firing the callback -- for use ONLY by code
 * that is about to change byok_modes_get() to something else on its own
 * (today: app_main.c's SET_MODE dispatch for an explicit mode value 0-6).
 * See this header's own top comment. A no-op if the menu is not open. */
void byok_menu_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_MENU_H */
