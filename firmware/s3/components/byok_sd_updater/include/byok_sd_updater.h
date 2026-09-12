/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_sd_updater.h — minimal, stock-compatible SD-card updater
 * ============================================================================
 *
 * Reproduces just enough of the stock updater's own behaviour
 * (docs/flash-layout-and-updater.md, D-011, D-015) that the owner can always get
 * BACK to stock: drop the pristine two-member `BYOK.tar`
 * (your own `BYOK.tar`, pulled from the device or obtained separately) at
 * `/SDCARD/Updates/`,
 * power-cycle, and whichever app is running processes it — because the SD
 * updater only ever runs inside the app that is currently booted, our own
 * firmware has to speak the same install format the stock app does, or the
 * SD recovery route would only work one way.
 *
 * What this does, at boot (see byok_sd_updater_check_and_run()):
 *   1. hold DOWN (GPIO7) -> skip entirely, mirroring the stock "Update
 *      escape hatch activated" behaviour (D-011).
 *   2. no SD card present (card-detect GPIO40) -> nothing to do.
 *   3. mount the card 1-bit SDMMC (hw_config.h Sec.6), read-only intent —
 *      format_if_mount_failed is FORCED OFF here regardless of what the
 *      stock firmware does (hw_config.h records the stock value as 1) —
 *      this project never formats the owner's card as a side effect of a
 *      boot-time scan. If the mount itself fails, that is logged and
 *      treated as "nothing to do", not a boot failure.
 *   4. no `/SDCARD/Updates/BYOK.tar` -> nothing to do.
 *   5. present: stream-walk it as a POSIX ustar archive (byok_tar.h,
 *      firmware/common/byok_tar) looking for a flat member named exactly
 *      `BYOK.bin`, typeflag '0' (any other member, including `assets.tar`,
 *      is skipped over — this firmware installs only the S3 app image; see
 *      README "Guesses and things to verify"). Found, it is validated BEFORE any
 *      flash is touched:
 *        - size must fit the OTA slot esp_ota_get_next_update_partition()
 *          would target;
 *        - the first 24 bytes must be a well-formed esp_image_header_t
 *          (magic 0xE9) whose chip_id is ESP32-S3 — NOTE: the obvious
 *          place to look for this, esp_app_desc_t (esp_app_desc.h),
 *          carries no chip field at all; the chip identity lives in
 *          esp_image_header_t, the image's own first 24 bytes, which is
 *          what is actually checked here. Documented rather than left
 *          to a reader's assumption.
 *      streamed straight into esp_ota_write() in bounded chunks — the
 *      whole ~3 MB image is never buffered in RAM at once.
 *   6. THE ACTUAL FLASH WRITE (esp_ota_begin/write/end +
 *      esp_ota_set_boot_partition) is gated behind its OWN flag,
 *      CONFIG_BYOK_SD_UPDATER_ARMED (default n in this component's own
 *      Kconfig — but firmware/s3/sdkconfig.defaults overrides that to `y`
 *      for this PoC build per docs/design-rationale.md D-017, so `idf.py build`
 *      in this tree always produces an ARMED image) — a SEPARATE gate from
 *      CONFIG_BYOK_ALLOW_OTADATA_WRITE (main/Kconfig.projbuild), which
 *      continues to gate BOOT_ORIGINAL, the boot-time EXECUTE-hold, and
 *      esp_ota_mark_app_valid_cancel_rollback() and is UNCHANGED by this
 *      component. D-017's reasoning for the split: this write reproduces
 *      exactly the stock updater's own operation (the designated,
 *      maker-documented recovery path — D-011/D-015), esp_ota_end()
 *      validates the image before esp_ota_set_boot_partition() is ever
 *      called, a corrupt image falls back to `factory` on this bootloader
 *      regardless, and `esptool erase_region 0x910000 0x2000` remains the
 *      ultimate off-device fallback either way — so it is armed on its own
 *      gate rather than waiting on CONFIG_BYOK_ALLOW_OTADATA_WRITE, which
 *      stays closed until the SD path itself has been exercised on the
 *      device. esp_ota_get_next_update_partition() can only ever hand back
 *      "the other" OTA slot, and on this device's CURRENT layout (D-016)
 *      that other slot may hold the stock-1.1.3 recovery rehearsal image,
 *      not free space — this is why the write is still validate-before-
 *      write and still its own explicit gate, not unconditional. There is
 *      deliberately no check of what the target slot already holds
 *      (docs/design-rationale.md D-019, overruling the target-slot guard once
 *      added in fbd1d6a): this is exact parity with the stock updater,
 *      which has no such guard either, and it is also the designated
 *      return-to-stock path — reinstalling the pristine stock BYOK.tar
 *      over a slot that already holds stock (e.g. the D-016 Stage A recovery
 *      image) must simply work, not be refused. Recovery of an
 *      overwritten OTA slot is always possible by re-applying the
 *      corresponding vendor tar or our own tar. With the
 *      ARMED flag off, every step through image validation still runs and
 *      is logged, including the exact target partition and size, but the
 *      write itself is skipped with a clearly logged reason ("would
 *      install"). See docs/design-rationale.md D-017 for the full rationale.
 *   7. On a successful write: best-effort remove (falling back to a
 *      rename() to `BYOK.tar.installed` if remove() fails) the tar from
 *      the card, unmount, and reboot into the new image — but ONLY if that
 *      removal/rename actually succeeded. If BOTH fail, the newly-written
 *      image stays selected to boot (esp_ota_set_boot_partition() already
 *      ran) but this boot does NOT reboot into it: rebooting with the
 *      identical tar still on the card would let the very next boot find
 *      it again and, since esp_ota_get_next_update_partition() would now
 *      point at the OTHER slot (the one just booted from), install into
 *      THAT slot too — an automatic ping-pong across both OTA slots that
 *      never reaches display or CDC. Staying up on the currently-running
 *      image instead means this boot still reaches display/CDC normally;
 *      the new image is picked up on the next manual power cycle, and even
 *      that boot still completes rather than looping. On any validation
 *      failure or with the write gated off: the tar is left in place
 *      (nothing is deleted) so the card's contents are inspectable and the
 *      attempt can be retried after a config change, and the SD card is
 *      unmounted normally.
 *
 *      NOTE — deliberate ordering divergence from stock: docs/flash-layout-
 *      and-updater.md records stock deleting BYOK.tar immediately after
 *      extraction and BEFORE the OTA write, which is why stock has no
 *      update loop under any outcome and needs no NVS flag to break one
 *      (that document's own note on the delete step). This component
 *      deletes only AFTER a successful esp_ota_end()+esp_ota_set_boot_
 *      partition(), so a power-interrupted install can be retried from the
 *      same tar on the next boot. Trade-off, not a bug: a payload that
 *      reliably fails validation or crashes the installer every time would
 *      retry indefinitely instead of stock's fail-once-then-stop behaviour.
 *      "Mirrors stock" in this file means the wire/archive format and the
 *      validate-then-write sequencing, not this byte-for-byte deletion
 *      timing.
 *
 * Never touches: `factory`, `nvs`, `phy_init`, the bootloader, or the
 * partition table — this component calls no API that can reach any of
 * those (esp_ota_get_next_update_partition() only ever returns an OTA app
 * slot by construction).
 *
 * Call once from app_main(), early — before byok_display_init() — so a
 * found-and-installed update reboots before any display/USB bring-up.
 */
#ifndef BYOK_SD_UPDATER_H_
#define BYOK_SD_UPDATER_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Optional step-status callback, invoked synchronously from inside
 * byok_sd_updater_check_and_run() at each notable step ("found BYOK.tar",
 * "validating BYOK.bin", "would install (SD_UPDATER_ARMED=n)",
 * "installing", "finalizing (esp_ota_end)", "installed" / "*_failed",
 * "removing BYOK.tar", "rebooting"). `ctx` is passed through unchanged
 * from the `status_ctx` argument. This component never touches a display
 * itself (no such dependency) -- the caller decides what to do with each
 * step string, e.g. draw it to the panel if already initialised, or just
 * let it fall through to the ESP_LOGx call already made alongside it. May
 * be called from a boot context earlier than most peripheral bring-up, so
 * the callback must not block or depend on anything not yet initialised.
 * `step` is a short, static (`__attribute__((unused))`-safe) string
 * literal -- do not free it, and do not assume it stays valid past the
 * call. */
typedef void (*byok_sd_updater_status_cb_t)(void *ctx, const char *step);

/** Runs the full check described above. `status_cb`/`status_ctx` are
 * optional (pass NULL/NULL for none) -- see byok_sd_updater_status_cb_t.
 * Returns ESP_OK for every "nothing to do" outcome (DOWN held, no card, no
 * tar, no BYOK.bin member, or the write gated off) as well as for a
 * genuinely successful install — a successful install that reboots calls
 * esp_restart() itself and never returns; an install that could not remove
 * or rename the tar (see step 7) also returns ESP_OK without rebooting.
 * Returns an error only for a real failure along the way (corrupt tar,
 * oversized/wrong-chip image, esp_ota_* failure); callers should log it
 * but must not treat it as fatal to booting. A mount
 * failure is NOT one of these — see step 3, it is treated as nothing to do
 * and returns ESP_OK. */
esp_err_t byok_sd_updater_check_and_run(byok_sd_updater_status_cb_t status_cb, void *status_ctx);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_SD_UPDATER_H_ */
