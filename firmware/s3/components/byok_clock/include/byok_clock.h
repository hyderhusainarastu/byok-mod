/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_clock.h — device-local CLOCK mode rendering (byok_modes BYOK_MODE_CLOCK)
 * ============================================================================
 *
 * Owns the one thing byok_modes.h's own header comment left "future": actual
 * rendering for BYOK_MODE_CLOCK. Draws large scaled HH:MM (byok_font8x8
 * glyphs, nearest-neighbour-scaled via repeated byok_display_fill_rect()
 * calls -- see byok_clock.c's own comment on why that reuses byok_display's
 * existing public API instead of adding a new primitive there), a
 * YYYY-MM-DD date line and a battery line, sourced from byok_rtc (PCF8563,
 * hw_config.h Sec.2) and byok_battery respectively.
 *
 * Mode transitions (docs/protocol.md Sec.6.4 SET_MODE, byok_modes.h):
 *
 *   - Idle timeout: entering the SELECTED idle submode (CLOCK by default,
 *     see byok_idle.h) automatically after CONFIG_BYOK_CLOCK_IDLE_TIMEOUT_MS
 *     (default 30 s) with no frame from a connected host -- this ALSO
 *     covers "at boot before any host connects" (the idle timer's t=0 is
 *     boot time either way, see this component's own Kconfig help text for
 *     why that is one mechanism, not two). 0.1.13: this component's own
 *     clock_task() calls byok_idle_enter_selected_submode() here, not
 *     byok_modes_set(BYOK_MODE_CLOCK) directly any more -- byok_idle.h owns
 *     which of CLOCK/STATIC_NOTE/BLANK that resolves to.
 *   - Any host frame switches straight back to BYOK_MODE_DASHBOARD_USB --
 *     byok_clock_notify_host_frame(), called from app_main.c's
 *     byok_on_frame() on every frame the dispatcher sees (including a
 *     duplicate or a seq-gap one; "any host frame" is read literally).
 *     0.1.13: this now fires from any of CLOCK/STATIC_NOTE/BLANK, not only
 *     CLOCK -- see the .c file's own updated comment.
 *   - SET_MODE(IDLE) enters the selected idle submode (byok_idle.h, same
 *     function as the timeout above), SET_MODE(HOST)/SET_MODE(MIRROR)
 *     forces DASHBOARD_USB, and 0.1.13 adds explicit SET_MODE values 4/5/6
 *     for CLOCK/STATIC_NOTE/BLANK directly -- all of that mapping lives in
 *     app_main.c's dispatch(), not here (this component only reacts to
 *     byok_modes_get()/_set(), it does not parse SET_MODE/SET_TIME frames
 *     itself).
 *
 * byok_modes' change-callback registry (byok_modes_register_change_cb(),
 * byok_modes.h): byok_clock_init() registers its own slot here, for
 * CLOCK-entry rendering only (clock_mode_changed() below ignores every
 * other transition). 0.1.13 widened the registry from a single slot to a
 * small fixed array specifically so byok_idle.c could register its OWN
 * callback alongside this one (STATIC_NOTE/BLANK entry rendering, plus
 * backlight handling on every CLOCK/STATIC_NOTE/BLANK transition) without
 * the two clobbering each other -- see byok_modes.h's own doc comment.
 *
 * Refresh cadence (this mode's own spec): once per minute, a PARTIAL refresh
 * covering only the glyph cells that actually changed since the last render
 * (the HH:MM digits that changed, the date line if the day rolled over, the
 * battery line if the reading changed) -- see clock_render_minute_update()
 * in the .c file. Once per hour (60 such minute ticks), a FULL refresh
 * instead, both to correct any partial-refresh drift the controller might
 * accumulate (the same reasoning byok_display_full_refresh(force_reinit)'s
 * own doc comment gives for a periodic full pass) and to re-establish the
 * whole screen's baseline for the next hour's diffing. Entering CLOCK
 * (from any other mode) always does one immediate FULL refresh first,
 * synchronously inside the byok_modes change callback -- the owner sees the
 * clock the instant the mode actually changes, not up to a tick period
 * later.
 * ============================================================================
 */
#ifndef BYOK_CLOCK_H
#define BYOK_CLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/** Registers this component as byok_modes' change callback and starts its
 * own 1 Hz task (idle-timeout watch while not in CLOCK, per-minute/per-hour
 * rendering while in CLOCK). Call once from app_main(), after
 * byok_modes_init(), byok_display_init() and byok_rtc_init() have all
 * already run (byok_clock degrades gracefully -- logs and skips rendering,
 * never crashes -- if byok_display or byok_rtc did not come up, so calling
 * this even when one of them failed is safe and is what app_main.c does:
 * a dead RTC or panel is still diagnosable over USB, same reasoning as
 * every other "logged and continue" boot step in this project). Safe to
 * call more than once (a second call is a no-op). */
void byok_clock_init(void);

/** Call on every frame app_main's dispatcher sees from a connected host
 * (byok_on_frame(), before dispatch() -- "any host frame", read literally,
 * including a duplicate or a seq-gap one). Resets the idle timer and, if
 * the device is currently in CLOCK mode, switches it straight back to
 * BYOK_MODE_DASHBOARD_USB. Safe to call even if byok_clock_init() was never
 * called (a no-op beyond recording the timestamp, which nothing then reads)
 * -- app_main.c does not need to guard the call site on init having
 * succeeded. */
void byok_clock_notify_host_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_CLOCK_H */
