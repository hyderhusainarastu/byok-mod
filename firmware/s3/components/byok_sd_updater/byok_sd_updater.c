/* SPDX-License-Identifier: MIT
 * See byok_sd_updater.h for the full behaviour writeup, the safety-gating
 * rationale, and what this deliberately does NOT reproduce from stock.
 */
#include "byok_sd_updater.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_BYOK_ENABLE_SD_UPDATER

#include "esp_app_format.h" /* esp_image_header_t, ESP_IMAGE_HEADER_MAGIC (bootloader_support) */
#include "esp_heap_caps.h" /* heap_caps_malloc/_free -- see the 2026-09-03 stack-overflow
                             * fix at each STREAM_CHUNK_BYTES/BYOK_TAR_BLOCK_SIZE buffer
                             * below: docs/troubleshooting.md */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "byok_hw_shim.h" /* hw_config.h pins -- BYOK_SD_*, BYOK_BTN_DOWN_GPIO */
#include "byok_tar.h"

static const char *TAG = "byok_sd_updater";

#define UPDATE_TAR_PATH (BYOK_SD_MOUNT_POINT "/Updates/BYOK.tar")
/* Fallback rename() target when remove(UPDATE_TAR_PATH) fails after a
 * successful install -- see byok_sd_updater_check_and_run(). A distinct
 * macro rather than `UPDATE_TAR_PATH ".installed"`: UPDATE_TAR_PATH is
 * itself parenthesized, so adjacent-string-literal concatenation with it
 * is not valid C. */
#define UPDATE_TAR_INSTALLED_PATH (BYOK_SD_MOUNT_POINT "/Updates/BYOK.tar.installed")
#define STREAM_CHUNK_BYTES 4096u

/* Anti-overflow / anti-hang bounds for walk_tar(), applied to every header
 * BEFORE any block-count arithmetic or fseek() is done with it. A crafted
 * (checksum-valid) header can claim up to ~2^36 bytes (12 octal digits);
 * byok_tar_size_to_blocks() * BYOK_TAR_BLOCK_SIZE on that would overflow a
 * 32-bit `long` and produce a wrapped-negative fseek(SEEK_CUR) offset, which
 * walks the file BACKWARDS and re-reads headers forever -- a hang before
 * display/USB bring-up (this runs first in app_main()), with no watchdog
 * panic configured. 16 MiB comfortably covers every real member this
 * project's own packaging produces (BYOK.bin's own OTA-slot-size gate in
 * install_member() is 0x300000 = 3 MiB; assets.tar and BYOK-Pico.bin are
 * both far smaller -- scripts/make-update-tar.sh) while keeping every block
 * multiply well inside `long` range on a 32-bit target. */
#define MAX_MEMBER_SIZE_BYTES (16u * 1024u * 1024u)
/* scripts/make-update-tar.sh only ever produces 2- or 3-member archives
 * (BYOK.bin [+ BYOK-Pico.bin] + assets.tar); this is a generous ceiling
 * against a header stream that is technically well-formed at each step but
 * never reaches two end-of-archive markers. */
#define MAX_MEMBERS 64u
/* Total bytes walked (headers + skipped/consumed data), as a second,
 * independent bound against a pathological archive -- catches a long run
 * of small members even though each individually passes MAX_MEMBER_SIZE_BYTES. */
#define MAX_TOTAL_WALK_BYTES (64u * 1024u * 1024u)

/* ==========================================================================
 * Button / card-detect gates
 * ========================================================================== */

static bool down_held(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BYOK_BTN_DOWN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* defensive; board supplies its own pull-ups */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(5)); /* let the input settle before the one sample we take */
    return gpio_get_level(BYOK_BTN_DOWN_GPIO) == 0; /* active-low, hw_config.h Sec.8 */
}

static bool sd_card_present(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BYOK_SD_DETECT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BYOK_SD_DETECT_INTERNAL_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    return gpio_get_level(BYOK_SD_DETECT_GPIO) == BYOK_SD_DETECT_PRESENT_LEVEL;
}

/* ==========================================================================
 * Mount / unmount
 * ========================================================================== */

static esp_err_t mount_sd(sdmmc_card_t **out_card)
{
    /* format_if_mount_failed is intentionally FALSE here regardless of
     * hw_config.h's recorded stock value (BYOK_SD_FORMAT_IF_MOUNT_FAILED=1)
     * -- see byok_sd_updater.h step 3. This project never formats the
     * owner's card as a side effect of a boot-time background scan. */
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = BYOK_SD_MAX_FILES,
        .allocation_unit_size = BYOK_SD_ALLOC_UNIT_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = BYOK_SD_SLOT;
    host.max_freq_khz = BYOK_SD_MAX_FREQ_KHZ;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = (gpio_num_t)BYOK_SD_CLK_GPIO;
    slot_config.cmd = (gpio_num_t)BYOK_SD_CMD_GPIO;
    slot_config.d0 = (gpio_num_t)BYOK_SD_D0_GPIO;
    slot_config.width = BYOK_SD_BUS_WIDTH;
    /* cd/wp left at SDMMC_SLOT_NO_CD/NO_WP -- hw_config.h Sec.6 confirms the
     * SDMMC driver's own cd/wp are unused (-1); card presence is handled
     * separately above via the polled GPIO40 card-detect pin. */

    return esp_vfs_fat_sdmmc_mount(BYOK_SD_MOUNT_POINT, &host, &slot_config, &mount_config, out_card);
}

/* ==========================================================================
 * BYOK.bin install (validate-then-write, bounded streaming)
 * ========================================================================== */

/* Validates and installs one BYOK.bin member (already positioned at its
 * first data byte within `f`, `size` bytes to consume). Only the success
 * path and the "validated but write gated off" path consume all `size`
 * bytes of member data -- every early-return error path (no OTA partition,
 * wrong type/subtype, size gate, short header, bad magic, bad chip_id,
 * esp_ota_begin failure) returns without reading the rest of the member, so
 * the file position is left mid-member on error. That is fine only because
 * walk_tar() treats any non-ESP_OK return from this function as fatal and
 * aborts the whole scan rather than trying to resync to the next header. */
static esp_err_t install_member(FILE *f, uint64_t size, bool *out_installed,
                                 byok_sd_updater_status_cb_t status_cb, void *status_ctx)
{
    *out_installed = false;

    if (status_cb != NULL) {
        status_cb(status_ctx, "validating BYOK.bin");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "no OTA update partition available -- refusing");
        return ESP_ERR_NOT_FOUND;
    }
    /* Belt-and-suspenders: esp_ota_get_next_update_partition() only ever
     * returns an APP_OTA_x partition by construction, but this component's
     * one hard rule (never write factory/nvs/phy_init/bootloader/partition
     * table) is worth asserting explicitly rather than trusting silently. */
    if (target->type != ESP_PARTITION_TYPE_APP ||
        (target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 && target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        ESP_LOGE(TAG, "esp_ota_get_next_update_partition returned a non-OTA partition (subtype 0x%02x) -- refusing",
                 (unsigned)target->subtype);
        return ESP_ERR_INVALID_STATE;
    }

    /* D-019: no target-slot content guard here, by design -- exact parity
     * with the stock updater, which has none either. esp_ota_get_next_
     * update_partition() can only ever hand back an OTA app slot (asserted
     * above); whatever it currently holds (a previous build, a foreign
     * image such as a stock recovery rehearsal, or blank/erased) is
     * unconditionally a valid write target, same as stock. This is also
     * the designated return-to-stock path: reinstalling the pristine stock
     * BYOK.tar over a slot that already holds stock must simply work.
     * Recovery of an overwritten OTA slot is always possible by
     * re-applying the corresponding vendor tar or our own tar. */

    if (size == 0 || size > target->size) {
        ESP_LOGE(TAG, "BYOK.bin is %llu bytes, target slot %s is %lu bytes -- refusing (size gate)",
                 (unsigned long long)size, target->label, (unsigned long)target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_image_header_t img_hdr;
    if (size < sizeof(img_hdr) || fread(&img_hdr, 1, sizeof(img_hdr), f) != sizeof(img_hdr)) {
        ESP_LOGE(TAG, "BYOK.bin (%llu bytes) is too short to hold an image header -- refusing",
                 (unsigned long long)size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (img_hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "BYOK.bin: image magic 0x%02x != 0x%02x -- refusing",
                 (unsigned)img_hdr.magic, (unsigned)ESP_IMAGE_HEADER_MAGIC);
        return ESP_ERR_INVALID_VERSION;
    }
    /* NOTE: the obvious place to look for a chip-identity check is
     * esp_app_desc_t (esp_app_desc.h), and it has no chip field at all.
     * The chip identity actually lives in esp_image_header_t, the image's
     * own first 24 bytes, which is what is checked here. See
     * byok_sd_updater.h's header comment for this note in context. */
    if (img_hdr.chip_id != ESP_CHIP_ID_ESP32S3) {
        ESP_LOGE(TAG, "BYOK.bin: image chip_id %u != ESP32-S3 (%u) -- refusing",
                 (unsigned)img_hdr.chip_id, (unsigned)ESP_CHIP_ID_ESP32S3);
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "BYOK.bin validated: %llu bytes, chip_id=ESP32-S3, target=%s @0x%06lx (slot size %lu)",
             (unsigned long long)size, target->label, (unsigned long)target->address, (unsigned long)target->size);

#if !CONFIG_BYOK_SD_UPDATER_ARMED
    /* Everything above ran and is logged; the flash write itself is gated
     * off by default -- see byok_sd_updater.h step 6, this component's
     * Kconfig, and docs/design-rationale.md D-017. Still stream-consume the
     * remaining bytes so the caller's tar-walk block accounting stays
     * correct. */
    ESP_LOGW(TAG, "CONFIG_BYOK_SD_UPDATER_ARMED is OFF in this build -- would install %llu bytes into %s "
                  "but NOT calling esp_ota_* (see components/byok_sd_updater/Kconfig, D-017).",
             (unsigned long long)size, target->label);
    if (status_cb != NULL) {
        status_cb(status_ctx, "would install (SD_UPDATER_ARMED=n)");
    }
    uint64_t remaining = size - sizeof(img_hdr);
    /* Same STREAM_CHUNK_BYTES heap move as the ARMED branch's `buf` above,
     * for consistency -- this branch is compiled OUT of the current build
     * (CONFIG_BYOK_SD_UPDATER_ARMED=y, so `#if !CONFIG_BYOK_SD_UPDATER_ARMED`
     * above is false) but a future build that flips the gate off would
     * reintroduce the identical 4096 B-on-a-3584 B-stack shape if this were
     * left as a stack local -- see docs/troubleshooting.md. */
    uint8_t *discard = heap_caps_malloc(STREAM_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (discard == NULL) {
        ESP_LOGE(TAG, "heap_caps_malloc(%u) failed for the discard buffer -- refusing",
                 (unsigned)STREAM_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }
    while (remaining > 0) {
        size_t chunk = remaining < STREAM_CHUNK_BYTES ? (size_t)remaining : STREAM_CHUNK_BYTES;
        if (fread(discard, 1, chunk, f) != chunk) {
            ESP_LOGW(TAG, "short read while discarding BYOK.bin payload (harmless -- write was skipped anyway)");
            break;
        }
        remaining -= chunk;
    }
    heap_caps_free(discard);
    return ESP_OK;
#else
    if (status_cb != NULL) {
        status_cb(status_ctx, "installing");
    }
    esp_ota_handle_t handle;
    esp_err_t e = esp_ota_begin(target, size, &handle);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e));
        return e;
    }

    /* STREAM_CHUNK_BYTES (4096 B), heap-allocated -- NOT a task-stack local.
     * docs/troubleshooting.md: this buffer, plus
     * walk_tar()'s 512 B header buffer below, is exactly what overflowed
     * "main"'s 3584 B stack the instant an install actually started, in
     * every build from 0.1.0 through 0.1.2. MALLOC_CAP_INTERNAL|
     * MALLOC_CAP_8BIT matches what esp_ota_write()'s own DMA-capable-flash
     * write path needs, and is also what byok_sd_updater_task's own
     * (already internal-RAM) stack would have provided had this stayed a
     * stack local -- freed unconditionally right after the write loop
     * below, before any of this function's several error-return paths, so
     * every one of them is already buffer-free by construction and none of
     * them need their own free() call. */
    uint8_t *buf = heap_caps_malloc(STREAM_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "heap_caps_malloc(%u) failed for the streaming buffer -- aborting install",
                 (unsigned)STREAM_CHUNK_BYTES);
        esp_ota_abort(handle);
        return ESP_ERR_NO_MEM;
    }

    e = esp_ota_write(handle, &img_hdr, sizeof(img_hdr));
    uint64_t remaining = size - sizeof(img_hdr);
    while (e == ESP_OK && remaining > 0) {
        size_t chunk = remaining < STREAM_CHUNK_BYTES ? (size_t)remaining : STREAM_CHUNK_BYTES;
        size_t got = fread(buf, 1, chunk, f);
        if (got != chunk) {
            ESP_LOGE(TAG, "short read from BYOK.bin (got %zu of %zu requested)", got, chunk);
            e = ESP_ERR_INVALID_SIZE;
            break;
        }
        e = esp_ota_write(handle, buf, chunk);
        remaining -= chunk;
    }
    heap_caps_free(buf);

    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(e));
        esp_ota_abort(handle);
        if (status_cb != NULL) {
            status_cb(status_ctx, "write failed");
        }
        return e;
    }

    if (status_cb != NULL) {
        status_cb(status_ctx, "finalizing (esp_ota_end)");
    }
    e = esp_ota_end(handle);
    if (e != ESP_OK) {
        /* esp_ota_end() is where image validation actually happens (magic,
         * checksum, SHA-256) -- a corrupt/truncated image is refused HERE,
         * before esp_ota_set_boot_partition() is ever reached, so nothing
         * has been selected to boot yet (D-017). */
        ESP_LOGE(TAG, "esp_ota_end: %s (image validation failed -- nothing was selected to boot)",
                 esp_err_to_name(e));
        if (status_cb != NULL) {
            status_cb(status_ctx, "validation failed");
        }
        return e;
    }

    e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(e));
        if (status_cb != NULL) {
            status_cb(status_ctx, "set_boot_partition failed");
        }
        return e;
    }

    ESP_LOGW(TAG, "BYOK.bin installed into %s and set as the next boot partition", target->label);
    if (status_cb != NULL) {
        status_cb(status_ctx, "installed");
    }
    *out_installed = true;
    return ESP_OK;
#endif
}

/* ==========================================================================
 * Tar walk
 * ========================================================================== */

/* Seeks forward by `amount` bytes in bounded chunks, each within `long`
 * range, so this is correct regardless of the local `long`'s width (32-bit
 * on xtensa). Defense in depth alongside the MAX_MEMBER_SIZE_BYTES cap
 * below -- with that cap in place `amount` is never actually large enough
 * to need more than one chunk, but this makes the seek itself safe on its
 * own terms rather than relying solely on the cap upstream. */
static esp_err_t seek_forward_bounded(FILE *f, uint64_t amount)
{
    while (amount > 0) {
        long chunk = (amount > (uint64_t)LONG_MAX) ? LONG_MAX : (long)amount;
        if (fseek(f, chunk, SEEK_CUR) != 0) {
            return ESP_FAIL;
        }
        amount -= (uint64_t)chunk;
    }
    return ESP_OK;
}

static esp_err_t walk_tar(FILE *f, bool *out_found, bool *out_installed,
                           byok_sd_updater_status_cb_t status_cb, void *status_ctx)
{
    *out_found = false;
    *out_installed = false;

    /* BYOK_TAR_BLOCK_SIZE (512 B), heap-allocated -- NOT a task-stack local.
     * See install_member()'s `buf` comment above for the full incident
     * context; this is the other half of the 4608 B (512+4096) that
     * overflowed "main"'s stack. Held for the whole scan (every header read
     * reuses it), so unlike `buf`/`discard` above it can't be freed at one
     * mid-function point -- every return path below goes through `done:`,
     * which frees it exactly once regardless of which path got there. */
    uint8_t *block = heap_caps_malloc(BYOK_TAR_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (block == NULL) {
        ESP_LOGE(TAG, "heap_caps_malloc(%u) failed for the tar header buffer -- aborting scan",
                 (unsigned)BYOK_TAR_BLOCK_SIZE);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    int zero_streak = 0;
    unsigned members = 0;
    uint64_t total_walked = 0;

    while (fread(block, 1, BYOK_TAR_BLOCK_SIZE, f) == BYOK_TAR_BLOCK_SIZE) {
        total_walked += BYOK_TAR_BLOCK_SIZE;
        byok_tar_header_t hdr;
        byok_tar_hdr_status_t st = byok_tar_parse_header(block, &hdr);

        if (st == BYOK_TAR_HDR_END) {
            if (++zero_streak >= 2) {
                break; /* standard ustar end-of-archive marker */
            }
            continue;
        }
        zero_streak = 0;

        if (st != BYOK_TAR_HDR_OK) {
            ESP_LOGE(TAG, "%s: corrupt tar header (status=%d) -- aborting scan", UPDATE_TAR_PATH, (int)st);
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }

        if (++members > MAX_MEMBERS) {
            ESP_LOGE(TAG, "%s: more than %u members without reaching end-of-archive -- aborting scan",
                     UPDATE_TAR_PATH, MAX_MEMBERS);
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }

        /* Reject before any block-count arithmetic or seek: a checksum-valid
         * header can still claim up to ~2^36 bytes (12 octal digits), and
         * byok_tar_size_to_blocks(hdr.size) * BYOK_TAR_BLOCK_SIZE on that
         * would overflow a 32-bit `long` seek offset -- see
         * MAX_MEMBER_SIZE_BYTES's own comment above. */
        if (hdr.size > MAX_MEMBER_SIZE_BYTES) {
            ESP_LOGE(TAG, "%s: member \"%s\" claims %llu bytes (> %u MiB cap) -- aborting scan",
                     UPDATE_TAR_PATH, hdr.name, (unsigned long long)hdr.size, MAX_MEMBER_SIZE_BYTES / (1024u * 1024u));
            ret = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        total_walked += hdr.size;
        if (total_walked > MAX_TOTAL_WALK_BYTES) {
            ESP_LOGE(TAG, "%s: total walked exceeds %u MiB -- aborting scan",
                     UPDATE_TAR_PATH, MAX_TOTAL_WALK_BYTES / (1024u * 1024u));
            ret = ESP_ERR_INVALID_SIZE;
            goto done;
        }

        uint32_t data_blocks = byok_tar_size_to_blocks(hdr.size);
        bool is_regular_file = (hdr.typeflag == '0' || hdr.typeflag == '\0');

        if (is_regular_file && strcmp(hdr.name, "BYOK.bin") == 0) {
            *out_found = true;
            ESP_LOGI(TAG, "%s: found member BYOK.bin (%llu bytes)", UPDATE_TAR_PATH, (unsigned long long)hdr.size);
            bool installed = false;
            esp_err_t e = install_member(f, hdr.size, &installed, status_cb, status_ctx);
            if (e != ESP_OK) {
                ret = e;
                goto done;
            }
            *out_installed = installed;
            /* install_member() consumed exactly hdr.size bytes; skip the
             * ustar padding up to the next 512-byte boundary. Both operands
             * are bounded by MAX_MEMBER_SIZE_BYTES above, so this
             * subtraction cannot underflow. */
            uint64_t pad = (uint64_t)data_blocks * BYOK_TAR_BLOCK_SIZE - hdr.size;
            if (pad > 0 && seek_forward_bounded(f, pad) != ESP_OK) {
                ESP_LOGE(TAG, "%s: seek past BYOK.bin padding failed", UPDATE_TAR_PATH);
                ret = ESP_FAIL;
                goto done;
            }
            if (installed) {
                goto done; /* our own image is being installed -- stop here (ret stays ESP_OK) */
            }
            continue;
        }

        ESP_LOGI(TAG, "%s: skipping member \"%s\" (%llu bytes, typeflag '%c')",
                 UPDATE_TAR_PATH, hdr.name, (unsigned long long)hdr.size,
                 hdr.typeflag ? hdr.typeflag : '0');
        uint64_t skip_bytes = (uint64_t)data_blocks * BYOK_TAR_BLOCK_SIZE;
        if (skip_bytes > 0 && seek_forward_bounded(f, skip_bytes) != ESP_OK) {
            ESP_LOGE(TAG, "%s: seek past member \"%s\" failed", UPDATE_TAR_PATH, hdr.name);
            ret = ESP_FAIL;
            goto done;
        }
    }

done:
    heap_caps_free(block);
    return ret;
}

/* ==========================================================================
 * Public entry point
 * ========================================================================== */

esp_err_t byok_sd_updater_check_and_run(byok_sd_updater_status_cb_t status_cb, void *status_ctx)
{
    if (down_held()) {
        ESP_LOGI(TAG, "DOWN held at boot -- SD updater skipped (mirrors the stock "
                      "\"Update escape hatch activated\" behaviour)");
        return ESP_OK;
    }

    if (!sd_card_present()) {
        return ESP_OK; /* nothing to do -- quiet, this is the common case */
    }

    sdmmc_card_t *card = NULL;
    esp_err_t e = mount_sd(&card);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) -- skipping the updater check this boot", esp_err_to_name(e));
        return ESP_OK; /* not fatal to booting */
    }

    FILE *f = fopen(UPDATE_TAR_PATH, "rb");
    if (f == NULL) {
        esp_vfs_fat_sdcard_unmount(BYOK_SD_MOUNT_POINT, card);
        return ESP_OK; /* no update tar present -- the common case once mounted */
    }

    ESP_LOGI(TAG, "found %s -- scanning for a BYOK.bin member", UPDATE_TAR_PATH);
    if (status_cb != NULL) {
        status_cb(status_ctx, "found BYOK.tar");
    }
    bool found = false, installed = false;
    esp_err_t result = walk_tar(f, &found, &installed, status_cb, status_ctx);
    fclose(f);

    if (result == ESP_OK && !found) {
        ESP_LOGW(TAG, "%s has no BYOK.bin member -- leaving it on the card", UPDATE_TAR_PATH);
    }

    bool tar_cleared = false;
    if (installed) {
        /* esp_ota_set_boot_partition() has already run inside
         * install_member() by this point -- the new image IS selected to
         * boot. Best-effort attempt to keep the same tar from being
         * re-applied: try remove() first, then rename() as a fallback (a
         * FAT read-only attribute or a dirty directory entry can make one
         * fail where the other succeeds). If BOTH fail we do NOT reboot
         * below: rebooting with the tar still named BYOK.tar would let the
         * next boot's SD updater find the identical file again, and because
         * esp_ota_get_next_update_partition() now points at the OTHER slot
         * (the one this device just booted FROM), that pass would erase and
         * rewrite THAT slot too -- an automatic ping-pong across ota_0/
         * ota_1 that never reaches display or CDC. Staying up on the
         * currently-running image instead means this boot still reaches
         * display/CDC; the new image is picked up on the next *manual*
         * power cycle, which is the only way this can recur, and even then
         * each such boot still completes normally rather than looping. */
        ESP_LOGI(TAG, "SD UPDATE: removing %s before reboot (best-effort -- see comment)", UPDATE_TAR_PATH);
        if (status_cb != NULL) {
            status_cb(status_ctx, "removing BYOK.tar");
        }
        if (remove(UPDATE_TAR_PATH) == 0) {
            ESP_LOGI(TAG, "SD UPDATE: %s removed", UPDATE_TAR_PATH);
            tar_cleared = true;
        } else {
            ESP_LOGW(TAG, "SD UPDATE: could not remove %s -- trying rename() fallback", UPDATE_TAR_PATH);
            if (rename(UPDATE_TAR_PATH, UPDATE_TAR_INSTALLED_PATH) == 0) {
                ESP_LOGI(TAG, "SD UPDATE: %s renamed to %s", UPDATE_TAR_PATH, UPDATE_TAR_INSTALLED_PATH);
                tar_cleared = true;
            } else {
                ESP_LOGE(TAG, "SD UPDATE: could not remove or rename %s -- NOT rebooting this cycle "
                              "(would risk an automatic reinstall ping-pong across both OTA slots; "
                              "continuing on the currently-running image, see comment above)",
                         UPDATE_TAR_PATH);
            }
        }
    }

    esp_vfs_fat_sdcard_unmount(BYOK_SD_MOUNT_POINT, card);

    if (installed && tar_cleared) {
        ESP_LOGW(TAG, "SD UPDATE: rebooting into the newly-installed image");
        if (status_cb != NULL) {
            status_cb(status_ctx, "rebooting");
        }
        vTaskDelay(pdMS_TO_TICKS(200)); /* let the log line above actually flush */
        esp_restart();
        /* not reached */
    }

    return result;
}

#else /* !CONFIG_BYOK_ENABLE_SD_UPDATER */

esp_err_t byok_sd_updater_check_and_run(byok_sd_updater_status_cb_t status_cb, void *status_ctx)
{
    (void)status_cb;
    (void)status_ctx;
    return ESP_OK;
}

#endif
