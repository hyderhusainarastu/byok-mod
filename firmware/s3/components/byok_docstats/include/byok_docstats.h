/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_docstats.h — bounded /SDCARD/Projects (recursive) *.txt writing-stats scan
 * ============================================================================
 *
 * Answers protocol.md Sec.6.4b GET_DOCSTATS: a background task mounts the
 * SD card (its OWN mount/unmount pass -- byok_sd_updater's own mount at
 * boot has already finished and unmounted by the time this component's
 * task starts, so there is no runtime overlap to coordinate), walks
 * `/SDCARD/Projects` recursively looking for `*.txt` files (case-
 * insensitive suffix), and produces a files/words/bytes/newest-mtime/
 * words-today snapshot -- READ-ONLY throughout: this component never
 * opens a file for write, never creates/removes/renames anything on the
 * card, and never calls f_mkfs / format_if_mount_failed (always false,
 * same posture as byok_sd_updater.c).
 *
 * Bounds (deliberately conservative, matching this project's own established
 * "a background scan must never be able to hang or exhaust memory" posture
 * -- byok_sd_updater.c's MAX_MEMBER_SIZE_BYTES/MAX_MEMBERS/MAX_TOTAL_WALK_
 * BYTES are the precedent this mirrors):
 *   - at most BYOK_DOCSTATS_MAX_FILES (200) files counted per scan;
 *   - at most BYOK_DOCSTATS_MAX_READ_BYTES (64 KiB) read per file for word
 *     counting (a bigger file's `words` contribution is undercounted, but
 *     its full size still counts toward `bytes` via stat(), and its mtime
 *     still counts toward `newest_epoch`/`words_today` eligibility);
 *   - directory recursion capped at BYOK_DOCSTATS_MAX_DEPTH levels below
 *     `Projects` itself.
 *
 * Scan cadence: once at boot (unconditionally -- app_main calls
 * byok_docstats_init(), which fires the first scan itself, before any host
 * has had a chance to connect) and then at most once per
 * BYOK_DOCSTATS_PERIOD_MS (10 min) while idle -- "idle" and "never during
 * host frames" are both read as `byok_modes_get() != BYOK_MODE_DASHBOARD_
 * USB`, the same primitive byok_idle.c already uses for "no host is
 * active" (host activity and DASHBOARD_USB are the same condition in this
 * firmware -- see byok_modes.h's own "rough correspondence" note), so a
 * periodic scan is simply skipped (and re-tried on the next 1-minute tick)
 * for as long as a host is connected and drawing.
 * ============================================================================
 */
#ifndef BYOK_DOCSTATS_H
#define BYOK_DOCSTATS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BYOK_DOCSTATS_MAX_FILES      200u
#define BYOK_DOCSTATS_MAX_READ_BYTES (64u * 1024u)
#define BYOK_DOCSTATS_MAX_DEPTH      8u
#define BYOK_DOCSTATS_PERIOD_MS      (10u * 60u * 1000u)

typedef struct {
    uint32_t files;        /*!< matched *.txt files, capped at BYOK_DOCSTATS_MAX_FILES */
    uint32_t words;        /*!< sum of whitespace-separated words, over up to the first
                             *   BYOK_DOCSTATS_MAX_READ_BYTES of each matched file */
    uint32_t bytes;        /*!< sum of each matched file's real size (stat().st_size, not
                             *   clamped by the per-file read cap) */
    uint32_t newest_epoch; /*!< max mtime (unix epoch, UTC) across matched files; 0 if none */
    uint32_t words_today;  /*!< sum of `words` for files whose mtime's calendar date (UTC)
                             *   matches the PCF8563 RTC's current date; 0 if the RTC is not
                             *   ready (byok_rtc_is_ready() false) -- "if derivable... else 0",
                             *   docs/protocol.md Sec.6.4b */
} byok_docstats_t;

/** Starts the background scan task: one scan immediately, then one more
 * per BYOK_DOCSTATS_PERIOD_MS while byok_modes_get() != BYOK_MODE_
 * DASHBOARD_USB (see this header's own top comment). Call once from
 * app_main, after byok_rtc_init() (so words_today can be derived from the
 * very first scan if the RTC is already up) and after byok_modes_init().
 * Safe to call more than once (a second call is a no-op). Never touches
 * the SD card synchronously -- this returns immediately, the first scan
 * runs on the new task. */
void byok_docstats_init(void);

/** Copies the most recently COMPLETED scan's results into `*out` (all
 * zero if no scan has completed yet -- byok_docstats_is_ready() is false
 * in that case). Safe to call from any task; the snapshot is a set of
 * plain aligned uint32_t fields updated atomically-enough for this
 * purpose by a single background task (see byok_docstats.c's own comment
 * on why no explicit lock is used). */
void byok_docstats_get(byok_docstats_t *out);

/** True once at least one scan has completed (successfully or not -- an
 * SD-card-absent/mount-failure "scan" still counts, producing an
 * all-zero snapshot, so GET_DOCSTATS has something well-defined to reply
 * with soon after boot rather than waiting indefinitely). */
bool byok_docstats_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_DOCSTATS_H */
