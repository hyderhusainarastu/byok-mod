/* SPDX-License-Identifier: MIT */
#include "byok_docstats.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

#include "driver/gpio.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "byok_hw_shim.h" /* BYOK_SD_*, hw_config.h pins/mount config */
#include "byok_modes.h"
#include "byok_rtc.h"

static const char *TAG = "byok_docstats";

#define PROJECTS_DIR (BYOK_SD_MOUNT_POINT "/Projects")

static byok_docstats_t s_stats;
static bool             s_ready;
static bool             s_inited;
static uint8_t          *s_scratch; /* BYOK_DOCSTATS_MAX_READ_BYTES, heap_caps_malloc'd once --
                                      * never a task-stack local, same convention
                                      * byok_sd_updater.c/byok_tar.c already established for a
                                      * buffer this size (docs/troubleshooting.md, "Boot loops
                                      * when installing an update from the SD card"); reused for
                                      * every file in every scan. */

/* ==========================================================================
 * Mount / unmount -- mirrors byok_sd_updater.c's mount_sd() exactly
 * (format_if_mount_failed forced off, same slot/pin/frequency config from
 * hw_config.h via byok_hw_shim.h). Duplicated rather than shared: the two
 * components run at different times for different purposes and neither
 * depends on the other; sharing a single mount_sd() would add a cross-
 * component dependency for four lines of struct literal.
 * ========================================================================== */

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

static esp_err_t mount_sd(sdmmc_card_t **out_card)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false, /* never format the owner's card, ever -- SAFETY.md */
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

    return esp_vfs_fat_sdmmc_mount(BYOK_SD_MOUNT_POINT, &host, &slot_config, &mount_config, out_card);
}

/* ==========================================================================
 * Scan
 * ========================================================================== */

static bool has_txt_suffix(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }
    const char *suf = name + len - 4;
    return (suf[0] == '.') && (tolower((unsigned char)suf[1]) == 't') &&
           (tolower((unsigned char)suf[2]) == 'x') && (tolower((unsigned char)suf[3]) == 't');
}

static uint32_t count_words(const uint8_t *buf, size_t len)
{
    uint32_t words = 0;
    bool in_word = false;
    for (size_t i = 0; i < len; i++) {
        bool ws = isspace(buf[i]) != 0;
        if (!ws && !in_word) {
            words++;
        }
        in_word = !ws;
    }
    return words;
}

/* today_known: whether `ty`/`tm`/`td` (from the RTC, 1-based month/day,
 * full year) are meaningful -- see byok_docstats_run_scan_locked()'s own
 * caller. gmtime_r is used (not localtime_r) purely for a fixed, glibc/
 * newlib-independent-of-TZ-setting conversion of the FAT mtime epoch --
 * both the RTC's own notion of "today" and the FAT timestamp are already
 * whatever wall-clock the owner set/the card's origin computer used, with
 * no timezone information carried by either, so UTC-vs-local here is a
 * consistent (if not provably "correct") choice, not a source of drift
 * between the two sides of the comparison. */
static void scan_file(const char *path, const struct stat *st, byok_docstats_t *out,
                       bool today_known, int ty, int tm, int td)
{
    out->files++;
    out->bytes += (uint32_t)st->st_size;
    if ((uint32_t)st->st_mtime > out->newest_epoch) {
        out->newest_epoch = (uint32_t)st->st_mtime;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "fopen(%s) failed -- counted in files/bytes/mtime only, 0 words", path);
        return;
    }
    size_t to_read = (st->st_size < (off_t)BYOK_DOCSTATS_MAX_READ_BYTES)
                          ? (size_t)st->st_size
                          : BYOK_DOCSTATS_MAX_READ_BYTES;
    size_t n = fread(s_scratch, 1, to_read, f);
    fclose(f);

    uint32_t words = count_words(s_scratch, n);
    out->words += words;

    if (today_known) {
        struct tm tmv;
        time_t mt = st->st_mtime;
        if (gmtime_r(&mt, &tmv) != NULL &&
            (tmv.tm_year + 1900) == ty && (tmv.tm_mon + 1) == tm && tmv.tm_mday == td) {
            out->words_today += words;
        }
    }
}

/* Iterative-per-level recursion (a plain recursive call per subdirectory,
 * depth-capped at BYOK_DOCSTATS_MAX_DEPTH) -- each frame's own locals are a
 * DIR* handle, one `struct dirent`-sized read result and one path buffer,
 * all small; the 64 KiB word-counting buffer (the one genuinely large
 * object here) is s_scratch, heap-allocated once, never a per-frame stack
 * local. Runs on byok_docstats_task's own dedicated stack (see
 * byok_docstats_init()), not any other component's task. */
static void scan_dir(const char *path, unsigned depth, byok_docstats_t *out,
                      bool today_known, int ty, int tm, int td)
{
    if (depth > BYOK_DOCSTATS_MAX_DEPTH || out->files >= BYOK_DOCSTATS_MAX_FILES) {
        return;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return; /* e.g. Projects/ itself doesn't exist -- nothing to scan, not an error */
    }

    struct dirent *ent;
    char child[300];
    while (out->files < BYOK_DOCSTATS_MAX_FILES && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            ESP_LOGW(TAG, "path too long under %s/%s -- skipped", path, ent->d_name);
            continue;
        }

        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            scan_dir(child, depth + 1, out, today_known, ty, tm, td);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !has_txt_suffix(ent->d_name)) {
            continue;
        }
        scan_file(child, &st, out, today_known, ty, tm, td);
    }
    closedir(d);
}

static void run_scan(void)
{
    byok_docstats_t result;
    memset(&result, 0, sizeof(result));

    if (!sd_card_present()) {
        ESP_LOGD(TAG, "no SD card present -- publishing an all-zero snapshot");
        s_stats = result;
        s_ready = true;
        return;
    }

    sdmmc_card_t *card = NULL;
    esp_err_t err = mount_sd(&card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) -- publishing an all-zero snapshot", esp_err_to_name(err));
        s_stats = result;
        s_ready = true;
        return;
    }

    bool today_known = false;
    int ty = 0, tm = 0, td = 0;
    if (byok_rtc_is_ready()) {
        byok_rtc_time_t t;
        if (byok_rtc_read(&t) == ESP_OK && !t.voltage_low) {
            today_known = true;
            ty = (int)t.year;
            tm = (int)t.month;
            td = (int)t.day;
        }
    }

    int64_t t0 = esp_timer_get_time();
    scan_dir(PROJECTS_DIR, 0, &result, today_known, ty, tm, td);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    esp_vfs_fat_sdcard_unmount(BYOK_SD_MOUNT_POINT, card);

    ESP_LOGI(TAG, "scan: %u file(s), %u word(s) (%u today), %u byte(s), newest=%u epoch, %lld ms",
             (unsigned)result.files, (unsigned)result.words, (unsigned)result.words_today,
             (unsigned)result.bytes, (unsigned)result.newest_epoch, (long long)elapsed_ms);

    s_stats = result;
    s_ready = true;
}

/* ==========================================================================
 * Task
 * ========================================================================== */

static void docstats_task(void *arg)
{
    (void)arg;

    run_scan(); /* "at boot", unconditional */

    TickType_t last_scan = xTaskGetTickCount();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60u * 1000u)); /* 1-minute poll, matching the granularity of
                                                  * the 10-minute period this checks against */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_scan) < pdMS_TO_TICKS(BYOK_DOCSTATS_PERIOD_MS)) {
            continue;
        }
        /* "while idle... never during host frames" -- byok_modes_get() !=
         * BYOK_MODE_DASHBOARD_USB is this firmware's own established
         * "no host is active" primitive (byok_idle.c uses the same check
         * for the same reason); BYOK_MODE_MENU is also excluded so a scan
         * never contends for CPU/SD while the owner is mid-selection. */
        byok_app_mode_t mode = byok_modes_get();
        if (mode == BYOK_MODE_DASHBOARD_USB || mode == BYOK_MODE_MENU) {
            continue; /* re-checked every minute until it's actually idle */
        }
        run_scan();
        last_scan = now;
    }
}

void byok_docstats_init(void)
{
    if (s_inited) {
        return;
    }

    s_scratch = (uint8_t *)heap_caps_malloc(BYOK_DOCSTATS_MAX_READ_BYTES,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_scratch == NULL) {
        ESP_LOGE(TAG, "heap_caps_malloc(%u) failed -- GET_DOCSTATS scanning disabled this boot",
                 (unsigned)BYOK_DOCSTATS_MAX_READ_BYTES);
        return;
    }

    /* Stack: scan_dir()'s own per-level frame (a DIR*, a 300 B path
     * buffer, a struct stat) times BYOK_DOCSTATS_MAX_DEPTH (8) is well
     * under 4 KiB; the FATFS/sdmmc call chain underneath opendir/readdir/
     * stat/fopen/fread is the same one byok_sd_updater's own dedicated
     * task (SD_UPDATER_TASK_STACK_BYTES = 12288) is sized for -- matching
     * that figure here rather than a smaller guess. */
    BaseType_t created = xTaskCreate(docstats_task, "byok_docstats", 12288, NULL, 2, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(byok_docstats) failed -- GET_DOCSTATS will report an all-zero, "
                      "never-ready snapshot this boot");
        heap_caps_free(s_scratch);
        s_scratch = NULL;
        return;
    }
    s_inited = true;
}

void byok_docstats_get(byok_docstats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stats;
}

bool byok_docstats_is_ready(void)
{
    return s_ready;
}
