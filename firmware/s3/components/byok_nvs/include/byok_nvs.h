/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_nvs.h — this firmware's OWN NVS namespace ("byokmod"), gated
 * ============================================================================
 *
 * 0.1.13 need: SET_NOTE text, the chosen idle submode (CLOCK/STATIC_NOTE/
 * BLANK) and the chosen backlight level index all want to survive a reboot.
 * The only NVS-type partition that physically exists on this device is the
 * vendor's own `nvs` (0x9000, 16 KiB, `firmware/s3/partitions.csv`) --
 * SAFETY.md §1 and that file's own header comment forbid adding a
 * partition-table entry ("Do not add an entry to 'reclaim' it"), and
 * `partitions.csv` must reproduce the *device's actual* table, not one this
 * project gets to design. So there is no separate partition available for
 * a dedicated store, only namespace isolation within the one that exists.
 *
 * This is a real tension with `main/app_main.c`'s own prior note ("A future
 * feature that needs key-value storage must use a dedicated partition of
 * its own") -- that note pre-dates this constraint being worked through
 * this thoroughly; there simply is no dedicated partition to give it
 * without editing a table this project has separately, explicitly
 * committed to never editing. The resolution here is the mechanism NVS
 * namespaces exist for in the first place: multiple independent consumers
 * safely sharing one physical NVS partition, each confined to their own
 * namespace's keys. This component:
 *
 *   - Opens ONLY `nvs_open("byokmod", NVS_READWRITE, ...)` -- BYOK_NVS_
 *     NAMESPACE below. It never opens, reads, or writes the vendor's own
 *     namespace ("BYOK") or any of the other vendor namespaces
 *     enumerated from the stock NVS partition (RTCI, KBLY, WFST, SODU, SYSI,
 *     DFNT, CONTR, CTYP, BTKL, BT_K, WIFI, LWSS, DJWT, BLBR, DMDE, ACPR) --
 *     a namespace is a hard boundary NVS itself enforces (nvs_open() can
 *     only ever see keys under the namespace it was opened with).
 *   - NEVER calls nvs_flash_erase(). Full stop, no code path in this file
 *     calls it, matching SAFETY.md §1's hard rule verbatim ("Never
 *     overwrite NVS... until a verified backup AND a tested recovery path
 *     both exist") -- the common ESP-IDF example idiom of erase-then-
 *     retry-init on ESP_ERR_NVS_NO_FREE_PAGES/_NEW_VERSION_FOUND is
 *     deliberately NOT reproduced here; byok_nvs_init() instead logs and
 *     disables persistence for the rest of that boot on any nvs_flash_
 *     init() result other than ESP_OK (or the benign "already initialised"
 *     ESP_ERR_INVALID_STATE, which esp_idf's own nvs_flash_init() does not
 *     return, but nvs_open() does -- see the .c file).
 *
 * Gated behind CONFIG_BYOK_NVS_PERSIST_ARMED (this component's own Kconfig,
 * default **n**), the same "off until a decision record clears it" shape
 * as CONFIG_BYOK_ALLOW_OTADATA_WRITE and CONFIG_BYOK_SD_UPDATER_ARMED
 * elsewhere in this tree (main/Kconfig.projbuild, components/
 * byok_sd_updater/Kconfig): `docs/design-rationale.md` D-003 is an AND-gate
 * (verified backup + a *tested* recovery path) that has only been met, so
 * far, for the OTA app-partition write path (D-016's Stage A rehearsal,
 * whose outcome D-018 records) --
 * nobody has ever exercised writing to, or restoring, THIS device's `nvs`
 * partition specifically, vendor-namespace-adjacent or not. With the gate
 * OFF (the default, and what this release ships and packages), every
 * getter/setter below is a pure in-RAM operation with no flash access at
 * all -- the feature (note text, idle-mode cycling, backlight cycling) is
 * fully usable for the running session, it just does not survive a reboot
 * yet. Turning the gate on stays a deliberate decision, to be taken only
 * once D-003's second conjunct is satisfied for this specific partition/
 * namespace -- not a build-time convenience. See this component's Kconfig for the full
 * rationale and see docs/protocol.md Sec.10 for the wire-facing summary.
 * ============================================================================
 */
#ifndef BYOK_NVS_H
#define BYOK_NVS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Our own namespace, inside the vendor's shared `nvs` partition -- never
 * the vendor's own namespace string ("BYOK") or any other vendor
 * namespace. See this header's own comment for why there is no separate
 * partition to use instead. */
#define BYOK_NVS_NAMESPACE "byokmod"

/** Keys within BYOK_NVS_NAMESPACE. NVS key names are capped at 15 bytes by
 * the NVS API itself -- all three fit comfortably. */
#define BYOK_NVS_KEY_NOTE      "note"          /* string, <= BYOK_NVS_NOTE_MAX_LEN bytes */
#define BYOK_NVS_KEY_IDLE_MODE "idle_mode"     /* u8, a byok_app_mode_t value (CLOCK/STATIC_NOTE/BLANK) */
#define BYOK_NVS_KEY_BACKLIGHT "backlight_lvl" /* u8, an index 0..BYOK_BACKLIGHT_LEVEL_COUNT-1 */
/* 0.1.14 (docs/protocol.md Sec.6.4b, byok_menu component): the preset-menu
 * name list and the owner's persisted selection. "presets" stores a fixed
 * BYOK_NVS_PRESETS_BLOB_LEN-byte blob -- 1 count byte followed by
 * BYOK_PRESET_MAX_COUNT * BYOK_PRESET_NAME_LEN name bytes, unused name
 * slots (index >= count) zero-filled -- rather than a variable-length blob,
 * so byok_nvs_get_presets()/_set_presets() never need to query the stored
 * length first. "preset" is the separately-persisted selected index (0 ..
 * count-1), written only on a completed menu selection, independent of
 * whether the preset list itself changed since. */
#define BYOK_NVS_KEY_PRESETS   "presets"       /* blob, BYOK_NVS_PRESETS_BLOB_LEN bytes, see above */
#define BYOK_NVS_KEY_PRESET    "preset"        /* u8, selected preset index (0..BYOK_PRESET_MAX_COUNT-1) */

/** Preset-menu limits (docs/protocol.md Sec.6.4b SET_PRESETS): at most 8
 * presets, each an up-to-20-byte NUL-padded name -- the same layout
 * SET_PRESETS carries on the wire (count u8 + count*20 bytes), which is
 * also the byok_menu component's own in-RAM representation. */
#define BYOK_PRESET_MAX_COUNT   8u
#define BYOK_PRESET_NAME_LEN   20u
#define BYOK_NVS_PRESETS_BLOB_LEN (1u + BYOK_PRESET_MAX_COUNT * BYOK_PRESET_NAME_LEN)

/** Longest SET_NOTE payload this firmware accepts (docs/protocol.md Sec.6.4
 * SET_NOTE) -- also the buffer size byok_note.c's in-RAM copy uses.
 * Excludes any NUL terminator: the note is stored/handled as an explicit
 * byte length throughout, not a C string, since arbitrary UTF-8/ASCII text
 * is not guaranteed NUL-free. */
#define BYOK_NVS_NOTE_MAX_LEN 200u

/** Brings up NVS for this component's own use, if and only if
 * CONFIG_BYOK_NVS_PERSIST_ARMED is on -- see this header's own top comment.
 * With the gate off, this is a no-op that returns ESP_OK immediately (every
 * getter/setter below then behaves as "not armed", see byok_nvs_is_armed()).
 * With the gate on: calls nvs_flash_init() (never nvs_flash_erase()) and
 * opens BYOK_NVS_NAMESPACE once, caching the handle. On any failure
 * (including "the NVS partition needs erasing", which this code refuses to
 * do on its own) logs a warning and leaves persistence disabled for the
 * rest of this boot -- callers do not need to check this function's return
 * value themselves, byok_nvs_is_armed() is the one thing that matters
 * afterward. Safe to call more than once (a second call is a no-op).
 * Call once from app_main, before byok_note_init()/byok_idle_init(). */
esp_err_t byok_nvs_init(void);

/** True only when CONFIG_BYOK_NVS_PERSIST_ARMED is on AND byok_nvs_init()
 * actually got a working namespace handle. False in every other case
 * (gate off, or the gate is on but nvs_flash_init()/nvs_open() failed) --
 * callers use this to decide whether to bother calling the getters/setters
 * below at all versus just keeping their own in-RAM default. */
bool byok_nvs_is_armed(void);

/** Reads the persisted note into `out` (capacity `out_cap`), writing the
 * actual byte count read to `*out_len` (0 if no note was ever persisted).
 * Internally stages the raw NVS read into a BYOK_NVS_NOTE_MAX_LEN+1 buffer
 * (room for nvs_get_str()'s own NUL terminator, which `out_cap` need not
 * itself account for) before copying the byte-length payload out to `out` --
 * so a caller that sizes `out`/`out_cap` to exactly BYOK_NVS_NOTE_MAX_LEN
 * (the same cap byok_nvs_set_note() enforces on the way in, e.g. byok_
 * note.c's s_note) can always read back a note at that exact length.
 * Returns ESP_ERR_INVALID_STATE if !byok_nvs_is_armed(), ESP_ERR_NOT_FOUND
 * if armed but no note key exists yet (first boot), ESP_ERR_INVALID_SIZE if
 * `out_cap` is smaller than BYOK_NVS_NOTE_MAX_LEN and the stored note
 * exceeds it. */
esp_err_t byok_nvs_get_note(char *out, size_t out_cap, size_t *out_len);

/** Persists `len` bytes from `note` under BYOK_NVS_KEY_NOTE. `len` must be
 * <= BYOK_NVS_NOTE_MAX_LEN (ESP_ERR_INVALID_ARG otherwise). No-op returning
 * ESP_ERR_INVALID_STATE if !byok_nvs_is_armed(). */
esp_err_t byok_nvs_set_note(const char *note, size_t len);

/** Reads the persisted idle submode (a byok_app_mode_t value) into `*out`.
 * Same ESP_ERR_INVALID_STATE/ESP_ERR_NOT_FOUND convention as byok_nvs_get_
 * note(). Does not itself validate the stored value against byok_app_
 * mode_t's range -- byok_idle.c does that (a value it doesn't recognise is
 * treated the same as "nothing persisted", not applied blindly). */
esp_err_t byok_nvs_get_idle_mode(uint8_t *out);

/** Persists `mode` under BYOK_NVS_KEY_IDLE_MODE. ESP_ERR_INVALID_STATE if
 * !byok_nvs_is_armed(). */
esp_err_t byok_nvs_set_idle_mode(uint8_t mode);

/** Reads the persisted backlight level index into `*out`. Same convention
 * as byok_nvs_get_idle_mode(). */
esp_err_t byok_nvs_get_backlight_idx(uint8_t *out);

/** Persists `idx` under BYOK_NVS_KEY_BACKLIGHT. ESP_ERR_INVALID_STATE if
 * !byok_nvs_is_armed(). */
esp_err_t byok_nvs_set_backlight_idx(uint8_t idx);

/** Reads the persisted preset-menu blob (BYOK_NVS_KEY_PRESETS,
 * BYOK_NVS_PRESETS_BLOB_LEN bytes: count u8 + BYOK_PRESET_MAX_COUNT *
 * BYOK_PRESET_NAME_LEN name bytes) into `out` (capacity `out_cap`, must be
 * >= BYOK_NVS_PRESETS_BLOB_LEN -- ESP_ERR_INVALID_SIZE otherwise). Same
 * ESP_ERR_INVALID_STATE/ESP_ERR_NOT_FOUND convention as byok_nvs_get_
 * idle_mode(). Does not itself validate the stored count against
 * BYOK_PRESET_MAX_COUNT -- byok_menu.c does that. */
esp_err_t byok_nvs_get_presets(uint8_t *out, size_t out_cap);

/** Persists exactly BYOK_NVS_PRESETS_BLOB_LEN bytes from `blob` under
 * BYOK_NVS_KEY_PRESETS (a fixed-size nvs_set_blob(), never nvs_set_str() --
 * this is not text). ESP_ERR_INVALID_STATE if !byok_nvs_is_armed(). */
esp_err_t byok_nvs_set_presets(const uint8_t *blob);

/** Reads the persisted selected preset index into `*out`. Same convention
 * as byok_nvs_get_idle_mode(). Does not itself validate `*out` against the
 * currently-loaded preset count -- byok_menu.c does that (an
 * out-of-range/stale index from a shorter list persisted earlier is
 * clamped there, not applied blindly). */
esp_err_t byok_nvs_get_preset_idx(uint8_t *out);

/** Persists `idx` under BYOK_NVS_KEY_PRESET. ESP_ERR_INVALID_STATE if
 * !byok_nvs_is_armed(). */
esp_err_t byok_nvs_set_preset_idx(uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_NVS_H */
