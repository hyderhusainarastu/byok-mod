/* SPDX-License-Identifier: MIT
 * ============================================================================
 * app_main.c — BYOK Mod S3 firmware: boot sequence + BYOK Link dispatcher
 * ============================================================================
 *
 * Boot: the EXECUTE (GPIO16) hold check (docs/protocol.md Sec.6.4
 * BOOT_ORIGINAL, D-011) -> components/byok_power's WAKE (GPIO6) hold-to-
 * power-off task starts (runs for the rest of this device's life, see
 * byok_power.h) -> identity/warning log lines -> the SD updater check
 * (components/byok_sd_updater: DOWN-held skip, then
 * /SDCARD/Updates/BYOK.tar if present -- reboots immediately on a
 * successful install, otherwise falls through) -> byok_display_init() ->
 * the boot self-test pattern sequence (checkerboard -> border+
 * banner+version -> h/v line test -> BOOT_ORIGINAL gate-state message ->
 * back to the border+banner screen, which is left up as the resting
 * state) -> byok_usb_cdc (CDC-ACM) -> a dedicated dispatch task that owns
 * the byok_proto parser and every protocol handler.
 *
 * Two independent otadata-write gates (docs/design-rationale.md D-017): the SD
 * updater's own OTA install (esp_ota_begin/write/end +
 * esp_ota_set_boot_partition, components/byok_sd_updater) is armed by
 * CONFIG_BYOK_SD_UPDATER_ARMED; BOOT_ORIGINAL, the boot-time EXECUTE hold
 * below, and esp_ota_mark_app_valid_cancel_rollback() near the bottom of
 * app_main() are armed by CONFIG_BYOK_ALLOW_OTADATA_WRITE. They are
 * deliberately not the same flag -- see D-017 and each symbol's own
 * Kconfig help text.
 *
 * Threading. byok_usb_cdc's RX callback runs on TinyUSB's own task; it must
 * stay fast and must never block on a TinyUSB TX flush (that would
 * deadlock: waiting for the very task the flush needs to run). So it does
 * nothing but copy bytes into a FreeRTOS stream buffer. ALL protocol work --
 * byok_parser_feed(), every dispatch handler, every byok_usb_cdc_send() --
 * runs on "byok_dispatch", the task this file creates -- the CDC transport
 * task the architecture calls for.
 *
 * Scope. This wires up every handler the v1.0/v1.1 command set defines (HELLO,
 * GET_INFO, PING, CLEAR, DRAW_TEXT, DRAW_RECT, DRAW_BITMAP, FRAME_*,
 * PARTIAL_REFRESH/FULL_REFRESH, SET_BACKLIGHT, GET_STATUS, REBOOT,
 * BOOT_ORIGINAL) plus two cheap extras that fell out of byok_display's own
 * API for free (SET_MODE bookkeeping, SET_CONTRAST). GET_STATUS/STATUS now
 * carries real battery_mv/battery_pct/charging (0.1.11, components/
 * byok_battery -- hw_config.h Sec.9's battery-divider ratio and GPIO12/13
 * polarity are CONFIRMED, no longer unresolved guesses). It deliberately
 * does NOT implement: GET_BATTERY itself (protocol.md Sec.6.3's dedicated
 * command, which additionally reports current_ma -- there is no current
 * sensor anywhere in this design, vendor or ours, so that field could only
 * ever be the "unknown" sentinel; STATUS already carries everything this
 * firmware can actually measure, and adding a second endpoint that repeats
 * it plus one guaranteed-unknown field is deferred, not blocked on
 * anything -- NACKs E_UNSUPPORTED), RLE payload decoding (FLAGS.RLE always
 * NACKs E_UNSUPPORTED), the v1
 * partial-frame FRAME_BEGIN path (protocol.md Sec.6.3 calls this "ugly"
 * itself), outbound EVT_BUTTON/EVT_STATUS/EVT_LOG events, and SEQ-window
 * duplicate replay of a TYPED response (a duplicate ACK_REQ frame gets a
 * bare ACK resent, not the original typed reply -- see byok_on_frame()).
 * Every one of these is a documented simplification, not an oversight; see
 * firmware/s3/README.md "Guesses and things to verify".
 *
 * 0.1.13 additions: new handler BYOK_TYPE_SET_NOTE (stores/renders the
 * device's STATIC NOTE idle screen, byok_note component); SET_MODE gains
 * explicit values 4/5/6 (force the CLOCK/STATIC_NOTE/BLANK idle submode
 * regardless of the current mode, byok_idle component) and mode=0 (IDLE)
 * now resolves to whichever of those three was last selected instead of
 * hard-coding CLOCK. See docs/protocol.md Sec.6.4 and byok_idle.h/byok_
 * note.h for the full design.
 *
 * 0.1.14 additions (docs/protocol.md v1.2 note, Sec.13): new handlers
 * BYOK_TYPE_SET_PRESETS/GET_DOCSTATS/DISPLAY_CFG (Sec.6.4b) and a new
 * SET_MODE value 7 (MENU); the preset-menu overlay itself (BYOK_MODE_MENU,
 * byok_menu component) gets its own display-ownership gate in every
 * drawing/refresh case below (menu_owns_display(), mirroring how mode=0
 * IDLE already meant "host frames ignored"). Two new outbound events this
 * dispatcher now actually SENDS (previously documented but never emitted,
 * see the 0.1.13-era "Scope" paragraph above): EVT_BUTTON (components/
 * byok_idle, host-active + menu-closed UP/DOWN/BRIGHTNESS/EXECUTE-short-
 * press) and EVT_PRESET_CHANGED (byok_menu, on a completed menu
 * selection) -- both via this file's own new send_event()/s_device_seq
 * (the device's own SEQ counter, distinct from every reply's echoed
 * request SEQ). STATUS's reserved flag bits (4-6, 7) now carry the
 * selected preset index and whether the menu is open (build_status_
 * payload()) -- see docs/protocol.md Sec.6.1's updated table. GET_DOCSTATS
 * is answered from components/byok_docstats' own background-task
 * snapshot, never by blocking dispatch_task on SD-card I/O. DISPLAY_CFG
 * flips components/byok_display's write path (byok_display_set_bulk_
 * writes()) at runtime; see that component's own updated header comment.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "mbedtls/sha256.h"

#include "sdkconfig.h"

#include "byok_crc32.h"
#include "byok_proto.h"

#include "byok_battery.h"
#include "byok_clock.h"
#include "byok_display.h"
#include "byok_docstats.h" /* 0.1.14: GET_DOCSTATS/DOCSTATS -- bounded /SDCARD/Projects (recursive) *.txt scan */
#include "byok_hw_shim.h" /* hw_config.h with Sec.13 unknowns resolved -- BTN_EXECUTE_GPIO, SD_DETECT_GPIO, etc. */
#include "byok_idle.h"  /* 0.1.13: UP/DOWN/EXECUTE/BRIGHTNESS idle-submode + backlight cycling;
                          * 0.1.14: short/long press + host EVT_BUTTON emission */
#include "byok_menu.h"  /* 0.1.14: SET_PRESETS + preset-menu overlay (BYOK_MODE_MENU) */
#include "byok_modes.h"
#include "byok_note.h"  /* 0.1.13: SET_NOTE storage + BYOK_MODE_STATIC_NOTE rendering */
#include "byok_nvs.h"   /* 0.1.13: our own NVS namespace "byokmod", gated -- see byok_nvs.h */
#include "byok_power.h"
#include "byok_rtc.h"
#include "byok_sd_updater.h"
#include "byok_usb_cdc.h"

static const char *TAG = "byok_app";

/* ==========================================================================
 * State
 * ========================================================================== */

static byok_parser_t s_rx_parser;
static uint8_t s_tx_buf[BYOK_MAX_FRAME]; /* reused for every outgoing frame --
                                           * dispatch is single-task, no lock needed */
static uint8_t s_device_id[8];           /* SHA-256(MAC) truncated, HELLO_ACK.device_id */
static StreamBufferHandle_t s_rx_stream;

/* docs/protocol.md Sec.6.4 SET_MODE value (IDLE/HOST/MIRROR/SLEEP) --
 * bookkeeping only in this firmware: nothing gates drawing commands on it,
 * and SLEEP does not actually turn off the panel/backlight. It is NOT the
 * same enum as byok_app_mode_t (byok_modes.h) -- see that header's own
 * comment on the two mode concepts. */
static uint8_t s_link_mode = 1; /* HOST */

typedef struct {
    bool     open;
    uint16_t w, h;
    uint8_t  bpp;
    uint8_t *buf;
    size_t   buf_len;
} frame_txn_t;
/* s_frame is owned exclusively by "byok_dispatch" (the task dispatch() and
 * frame_check_timeout() both run on). frame_timeout_cb() runs on the
 * esp_timer task and must NEVER read or write s_frame directly -- it only
 * sets s_frame_timeout_pending, which byok_dispatch polls and acts on. This
 * is what keeps FRAME_DATA's memcpy (which can run concurrently with an
 * expiring timer) from racing a free() of the same buffer. */
static frame_txn_t s_frame;
static esp_timer_handle_t s_frame_timer; /* Sec.7.6: 3000 ms stall aborts the transaction */
static volatile bool s_frame_timeout_pending;

typedef enum { STOCK_FOUND, STOCK_NOT_FOUND, STOCK_AMBIGUOUS } stock_lookup_t;

/* ==========================================================================
 * Little-endian payload helpers (docs/protocol.md Sec.6: "All integers
 * little-endian"; byok_proto.h is deliberately payload-schema-agnostic, see
 * its own header comment, so this file owns every field layout).
 * ========================================================================== */

static inline uint16_t rd_u16(const uint8_t *p, size_t off)
{
    return (uint16_t)((uint16_t)p[off] | ((uint16_t)p[off + 1] << 8));
}
static inline uint32_t rd_u32(const uint8_t *p, size_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}
static inline void wr_u16(uint8_t *p, size_t off, uint16_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
}
static inline void wr_u32(uint8_t *p, size_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
}

/* ==========================================================================
 * Outgoing frames
 * ========================================================================== */

static void send_reply(uint8_t type, uint8_t flags, uint16_t seq,
                        const uint8_t *payload, uint16_t len)
{
    size_t n = byok_frame_encode(type, flags, seq, payload, len, s_tx_buf, sizeof(s_tx_buf));
    if (n == 0) {
        ESP_LOGE(TAG, "byok_frame_encode failed: type=0x%02X len=%u", type, (unsigned)len);
        return;
    }
    esp_err_t err = byok_usb_cdc_send(s_tx_buf, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_usb_cdc_send failed: %s", esp_err_to_name(err));
    }
}

static void send_ack(uint16_t seq)
{
    send_reply(BYOK_TYPE_ACK, BYOK_FLAG_IS_REPLY, seq, NULL, 0);
}

/* 0.1.14: this dispatcher's first unsolicited device->host traffic
 * (docs/protocol.md Sec.6.5/Sec.7.5) -- s_device_seq is this device's own
 * SEQ counter, independent of the reply-echoing `seq` every send_reply()
 * call above this point has always used (those echo the REQUEST's SEQ;
 * an event has no request to echo, so it needs its own, incrementing,
 * sender-scoped counter, exactly what Sec.7.5 specifies: "host and device
 * each own an independent u16 counter... incremented per originated
 * frame"). Wraps 0xFFFF -> 0x0000 for free (uint16_t arithmetic). */
static uint16_t s_device_seq;

static void send_event(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint16_t seq = s_device_seq++;
    send_reply(type, BYOK_FLAG_EVENT, seq, payload, len);
}

/* Wired to byok_idle_set_button_event_cb() from app_main(), below. */
static void on_button_event(uint8_t button, uint8_t state, uint32_t t_ms)
{
    uint8_t payload[6];
    payload[0] = button;
    payload[1] = state;
    wr_u32(payload, 2, t_ms);
    send_event(BYOK_TYPE_EVT_BUTTON, payload, sizeof(payload));
}

/* Wired to byok_menu_set_preset_changed_cb() from app_main(), below. */
static void on_preset_changed(uint8_t index)
{
    send_event(BYOK_TYPE_EVT_PRESET_CHANGED, &index, 1);
}

static void maybe_ack(const byok_frame_t *frame)
{
    if (frame->flags & BYOK_FLAG_ACK_REQ) {
        send_ack(frame->seq);
    }
}

static void send_nack_raw(uint16_t seq_echo, uint8_t code, uint8_t detail)
{
    uint8_t payload[4];
    payload[0] = code;
    payload[1] = detail;
    wr_u16(payload, 2, seq_echo);
    send_reply(BYOK_TYPE_NACK, BYOK_FLAG_IS_REPLY, seq_echo, payload, sizeof(payload));
}

/* Sends a NACK echoing `fr`'s own SEQ. Only for use inside dispatch(), where
 * `fr` is a validly-decoded frame (framing-level rejects, which have no
 * trustworthy SEQ, go through byok_on_error() / send_nack_raw() directly). */
#define NACK(fr, code, detail) send_nack_raw((fr)->seq, (code), (uint8_t)(detail))

/* ==========================================================================
 * BOOT_ORIGINAL support -- find the stock app partition
 * ========================================================================== */

#if CONFIG_BYOK_ALLOW_OTADATA_WRITE
/* Only referenced from the three CONFIG_BYOK_ALLOW_OTADATA_WRITE call sites
 * below; guarded the same way so it isn't defined-but-unused when the flag
 * is off (the default) -- this does not change what the flag gates, only
 * whether this helper is compiled in at all. */
static stock_lookup_t byok_find_stock_partition(const esp_partition_t **out)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *found = NULL;
    int count = 0;

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *part = esp_partition_get(it);
        if (part == running) {
            continue;
        }
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
            continue; /* not a valid/flashed app image -- skip, not an error */
        }
        /* project_name is free-text the image itself supplies -- not an
         * authenticated identity -- so also require the candidate to sit
         * where the device's own (SHA-verified, docs/recovery.md) partition
         * table says stock can legitimately live: `factory` or `ota_0`.
         * Our own image does NOT always land in `ota_1` -- on a virgin
         * `otadata` (all-0xFF, no OTA has ever run) esp_ota_get_next_
         * update_partition() from `factory` returns `ota_0`, so a freshly
         * first-installed BYOK image can itself be sitting in `ota_0`. The
         * `part != running` check above is what actually excludes our own
         * image from this scan, in whichever slot it happens to be
         * running from -- this subtype whitelist alone does not. */
        bool valid_stock_slot = (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) ||
                                 (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0);
        if (valid_stock_slot && strncmp(desc.project_name, "BYOK", sizeof(desc.project_name)) == 0) {
            found = part;
            count++;
        }
    }
    esp_partition_iterator_release(it);

    if (count == 0) {
        *out = NULL;
        return STOCK_NOT_FOUND;
    }
    if (count > 1) {
        /* Two non-running slots both look like stock -- e.g. D-016's Stage A
         * (stock 1.1.3 in ota_0 + the original stock image still in
         * factory). `factory` is the one slot that is verifiably stock by
         * construction (never written by this project, never an OTA
         * target), so prefer it over refusing outright. Re-walk rather
         * than tracking a second pointer above, to keep the common
         * (count<=1) path allocation-free and simple. */
        esp_partition_iterator_t it2 = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        const esp_partition_t *factory = (it2 != NULL) ? esp_partition_get(it2) : NULL;
        if (it2 != NULL) {
            esp_partition_iterator_release(it2);
        }
        if (factory != NULL && factory != running) {
            esp_app_desc_t fdesc;
            if (esp_ota_get_partition_description(factory, &fdesc) == ESP_OK &&
                strncmp(fdesc.project_name, "BYOK", sizeof(fdesc.project_name)) == 0) {
                *out = factory;
                return STOCK_FOUND;
            }
        }
        *out = NULL;
        return STOCK_AMBIGUOUS;
    }
    *out = found;
    return STOCK_FOUND;
}
#endif /* CONFIG_BYOK_ALLOW_OTADATA_WRITE */

/* HELLO_ACK/INFO capability bit 8: honestly reflects "found AND permitted",
 * not just "found" -- with the safety flag off, BOOT_ORIGINAL always NACKs
 * E_NOT_PERMITTED regardless of what's in the other slot, and a host UI that
 * checked only "found" would offer a button that always fails. */
static bool byok_boot_original_available(void)
{
#if CONFIG_BYOK_ALLOW_OTADATA_WRITE
    const esp_partition_t *p = NULL;
    return byok_find_stock_partition(&p) == STOCK_FOUND;
#else
    return false;
#endif
}

static uint8_t compute_boot_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return 0xFF;
    }
    switch (running->subtype) {
    case ESP_PARTITION_SUBTYPE_APP_FACTORY: return 0;
    case ESP_PARTITION_SUBTYPE_APP_OTA_0:   return 1;
    case ESP_PARTITION_SUBTYPE_APP_OTA_1:   return 2;
    default:                                return 0xFF;
    }
}

/* docs/protocol.md Sec.6.6 */
static uint32_t compute_caps(void)
{
    uint32_t caps = 0;
    caps |= 0x00000001u; /* bit0: 1bpp accepted */
    /* bit1 (2bpp native): NOT set -- panel is 1bpp native (hw_config.h Sec.3);
     * DRAW_BITMAP accepts 2bpp on the wire but down-converts it. */
    /* bit2 (RLE): NOT set -- not implemented in this firmware. */
    caps |= 0x00000008u; /* bit3: partial refresh supported */
    caps |= 0x00000010u; /* bit4: backlight controllable */
    caps |= 0x00000020u; /* bit5: contrast controllable */
    caps |= 0x00000040u; /* bit6: battery readable -- STATUS.battery_mv/_pct/charging are real
                           * (components/byok_battery, 0.1.11); GET_BATTERY itself still NACKs,
                           * see this file's header comment, but this bit is generic "battery
                           * readable", not "GET_BATTERY implemented", and STATUS is the primary
                           * channel (docs/protocol.md Sec.6.1). */
    caps |= 0x00000080u; /* bit7: button events emitted -- 0.1.14, components/byok_idle
                           * (UP/DOWN/BRIGHTNESS/EXECUTE-short-press while host-active and
                           * the menu is closed; see docs/protocol.md Sec.6.5). */
    if (byok_boot_original_available()) {
        caps |= 0x00000100u; /* bit8 */
    }
    caps |= 0x00000200u; /* bit9: built-in fonts present (byok_font8x8, 1 font) */
    /* bit10 (Wi-Fi), bit11 (PICO link): NOT set -- neither exists in this tree yet. */
    if (byok_rtc_is_ready()) {
        caps |= 0x00001000u; /* bit12 (0.1.12, docs/protocol.md Sec.6.6 v1.1 note): RTC present +
                               * CLOCK mode (byok_modes BYOK_MODE_CLOCK) implemented. Gated on the
                               * RTC actually having come up, same "found AND working" spirit as
                               * bit8's own comment above -- a host should not offer a CLOCK/SET_TIME
                               * UI control for a device whose PCF8563 read failed at boot. */
    }
    return caps;
}

/* ==========================================================================
 * Typed replies
 * ========================================================================== */

static void send_hello_ack(uint16_t seq)
{
    uint8_t p[32];
    memset(p, 0, sizeof(p));
    p[0] = BYOK_VERSION;
    p[1] = 0; /* fw_major */
    p[2] = 1; /* fw_minor -- matches CONFIG_APP_PROJECT_VER "byok-mod 0.1.15" */
    p[3] = 15; /* fw_patch */
    wr_u16(p, 4, BYOK_DISPLAY_W);
    wr_u16(p, 6, BYOK_DISPLAY_H);
    p[8] = BYOK_DISPLAY_BPP_NATIVE;
    p[9] = 2; /* display_bpp_max: DRAW_BITMAP accepts 1 or 2 bpp, down-converting 2 */
    wr_u16(p, 10, BYOK_MAX_PAYLOAD);
    wr_u32(p, 12, compute_caps());
    p[16] = compute_boot_slot();
    p[17] = 1; /* is_our_firmware */
    wr_u16(p, 18, BYOK_DISPLAY_FONT_COUNT);
    wr_u32(p, 20, (uint32_t)(esp_timer_get_time() / 1000));
    memcpy(p + 24, s_device_id, 8);
    send_reply(BYOK_TYPE_HELLO_ACK, BYOK_FLAG_IS_REPLY, seq, p, sizeof(p));
}

/* "Mon DD YYYY" + "HH:MM:SS" (esp_app_desc_t's compiler __DATE__/__TIME__
 * format) -> "YYYY-MM-DDThh:mm", per docs/protocol.md Sec.6.1 INFO. */
static void format_build_date_iso(const char *cdate, const char *ctime, char *out, size_t out_len)
{
    static const char *const months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char mon[4] = { 0 };
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(cdate, "%3s %d %d", mon, &day, &year) == 3 &&
        sscanf(ctime, "%d:%d:%d", &hh, &mm, &ss) == 3) {
        int mnum = 0;
        for (int i = 0; i < 12; i++) {
            if (strncmp(mon, months[i], 3) == 0) {
                mnum = i + 1;
                break;
            }
        }
        snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d", year, mnum, day, hh, mm);
    } else {
        snprintf(out, out_len, "unknown");
    }
    (void)ss;
}

static void send_info(uint16_t seq)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    uint8_t p[64];
    memset(p, 0, sizeof(p));

    strncpy((char *)p + 0, desc->version, 15);
    strncpy((char *)p + 16, desc->idf_ver, 15);

    char iso[24];
    format_build_date_iso(desc->date, desc->time, iso, sizeof(iso));
    strncpy((char *)p + 32, iso, 15);

    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    wr_u32(p, 48, flash_bytes);

    uint32_t psram_bytes = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    wr_u32(p, 52, psram_bytes);

    /* Compile-time, not a runtime clock query (esp_clk_cpu_freq() lives under
     * esp_hw_support's esp_private/ headers, not meant for app code) --
     * accurate as long as nothing calls esp_pm/DFS to change frequency at
     * runtime, which this firmware never does. */
    wr_u32(p, 56, (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u);

    wr_u32(p, 60, compute_caps());

    send_reply(BYOK_TYPE_INFO, BYOK_FLAG_IS_REPLY, seq, p, sizeof(p));
}

static bool sd_present(void)
{
    return gpio_get_level(BYOK_SD_DETECT_GPIO) == BYOK_SD_DETECT_PRESENT_LEVEL;
}

static void build_status_payload(uint8_t *out)
{
    memset(out, 0, 20);
    /* components/byok_battery (0.1.11) -- EMA-filtered mV/pct + fresh
     * GPIO12/13 charger status, docs/protocol.md Sec.6.1's own 0/1/2/0xFF
     * encoding (BYOK_CHARGE_STATUS_NONE/_CHARGING/_COMPLETE match it
     * exactly, hw_config.h Sec.9). No 0xFF "unknown" sentinel needed here:
     * byok_battery_get_mv()/_get_pct() always return a real reading (with
     * an early-boot synchronous fallback, see byok_battery.h), and
     * byok_battery_get_charging() defaults to NONE (0), never 0xFF, when
     * the component failed to init -- 0 is a legitimate, indistinguishable
     * "no charger" value in that case, which is the same fail-safe this
     * project already applies to every other "driver didn't come up"
     * condition (logged, not surfaced as a fake distinct wire value). */
    wr_u16(out, 0, byok_battery_get_mv());
    out[2] = byok_battery_get_pct();
    out[3] = byok_battery_get_charging();
    out[4] = s_link_mode;
    out[5] = byok_display_get_backlight();
    out[6] = byok_display_get_contrast();

    uint8_t flags = 0;
    if (byok_usb_cdc_dtr()) {
        flags |= 0x01; /* host-connected */
    }
    if (sd_present()) {
        flags |= 0x02; /* SD present */
    }
    /* bit2 (USB attached) and bit3 (display sleeping) intentionally left 0 --
     * neither the TUSB320 driver nor a real SLEEP mode exists in this
     * firmware. See README "Guesses and things to verify".
     *
     * 0.1.14: bits 4-6 (mask 0x70) and bit 7 (mask 0x80) of this SAME byte
     * are the "reserved byte" docs/protocol.md Sec.6.1 asked for -- STATUS
     * (6.1) is 20 bytes, fully allocated to existing fields (no spare
     * byte), so this is that section's own fallback ("else reserved bits
     * of the flags byte"), same as `mode`/`backlight`/`contrast` already
     * share this payload with `flags`. bits 4-6: the CURRENTLY SELECTED
     * (persisted) preset index, 0-7 (byok_menu_get_selected_index(),
     * BYOK_PRESET_MAX_COUNT == 8 fits exactly in 3 bits) -- NOT the
     * highlight row while the menu is open and not yet confirmed. bit 7:
     * whether the preset menu is showing right now (byok_menu_is_open()).
     * See docs/protocol.md Sec.6.1's updated STATUS table. */
    flags |= (uint8_t)((byok_menu_get_selected_index() & 0x07u) << 4);
    if (byok_menu_is_open()) {
        flags |= 0x80u;
    }
    out[7] = flags;

    wr_u32(out, 8, (uint32_t)(esp_timer_get_time() / 1000));
    wr_u32(out, 12, (uint32_t)esp_get_free_heap_size());
    wr_u32(out, 16, (uint32_t)esp_get_minimum_free_heap_size());
}

/* ==========================================================================
 * Frame-transaction (FRAME_BEGIN/DATA/END) helpers
 * ========================================================================== */

static void free_frame_txn(void)
{
    free(s_frame.buf);
    memset(&s_frame, 0, sizeof(s_frame));
}

/* Runs on the esp_timer task. Touches nothing but this one flag -- see the
 * s_frame comment above. */
static void frame_timeout_cb(void *arg)
{
    (void)arg;
    s_frame_timeout_pending = true;
}

/* Called only from "byok_dispatch", between frames (never mid-parse), so it
 * can never race a FRAME_DATA memcpy or any other s_frame access. */
static void frame_check_timeout(void)
{
    if (!s_frame_timeout_pending) {
        return;
    }
    s_frame_timeout_pending = false;
    if (s_frame.open) {
        ESP_LOGW(TAG, "frame transaction stalled (docs/protocol.md Sec.7.6, 3000 ms) -- aborting");
        free_frame_txn();
    }
}

/* 0.1.14: while the preset menu (byok_menu, BYOK_MODE_MENU) is showing,
 * every drawing/refresh handler below still validates its payload and
 * ACKs/NACKs exactly as documented, but skips the actual byok_display_*
 * call -- the same "device shows its own screen; host frames [accepted
 * but] ignored" posture docs/protocol.md Sec.6.4 already documents for
 * link-level SET_MODE(IDLE), applied here to the menu overlay so a host
 * that keeps drawing (e.g. a MIRROR stream) cannot visually clobber the
 * menu. See byok_menu.h's own header comment ("Display ownership while
 * open") and each call site below tagged with this function's name. */
static inline bool menu_owns_display(void)
{
    return byok_modes_get() == BYOK_MODE_MENU;
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

static void dispatch(const byok_frame_t *frame)
{
    const uint8_t *p = frame->payload;

    switch (frame->type) {

    case BYOK_TYPE_HELLO: {
        if (frame->len != 8) { NACK(frame, BYOK_E_BAD_LENGTH, 8); break; }
        /* HELLO is an explicit new-session signal (protocol.md Sec.7.5): a
         * reconnecting host may restart its SEQ counter anywhere, and that
         * must not look like a stale peer's SEQ gap or a duplicate. This
         * frame's own track_sequence() already ran (byok_proto.c calls it
         * before on_frame), so resetting here only affects frames after
         * this HELLO.
         *
         * Deliberately does NOT touch the display (device observation,
         * 2026-09-03: the boot resting screen -- border + "BYOK LAB" +
         * version + RST reason + "USB: waiting for host", see
         * byok_display_selftest.c -- must stay up across a bare HELLO, not
         * go blank the instant a host says hello before it has actually
         * drawn anything). The only handlers in this switch that ever call
         * a byok_display_x function are CLEAR, DRAW_x, FRAME_x,
         * PARTIAL_REFRESH and FULL_REFRESH -- if you're adding a new one,
         * don't make it a display-clearing side effect of connecting. */
        byok_parser_new_session(&s_rx_parser);
        send_hello_ack(frame->seq);
        break;
    }

    case BYOK_TYPE_GET_INFO: {
        if (frame->len != 0) { NACK(frame, BYOK_E_BAD_LENGTH, 0); break; }
        send_info(frame->seq);
        break;
    }

    case BYOK_TYPE_GET_STATUS: {
        if (frame->len != 0) { NACK(frame, BYOK_E_BAD_LENGTH, 0); break; }
        uint8_t payload[20];
        build_status_payload(payload);
        send_reply(BYOK_TYPE_STATUS, BYOK_FLAG_IS_REPLY, frame->seq, payload, sizeof(payload));
        break;
    }

    case BYOK_TYPE_PING: {
        if (frame->len != 0 && frame->len != 4) { NACK(frame, BYOK_E_BAD_LENGTH, 0); break; }
        send_reply(BYOK_TYPE_PONG, BYOK_FLAG_IS_REPLY, frame->seq, frame->payload, frame->len);
        break;
    }

    case BYOK_TYPE_CLEAR: {
        if (frame->len != 1) { NACK(frame, BYOK_E_BAD_LENGTH, 1); break; }
        if (!menu_owns_display()) {
            byok_display_clear(p[0] != 0);
        }
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_DRAW_TEXT: {
        if (frame->len < 8) { NACK(frame, BYOK_E_BAD_LENGTH, 8); break; }
        uint16_t x = rd_u16(p, 0), y = rd_u16(p, 2);
        uint8_t font_id = p[4], style = p[5];
        uint16_t text_len = rd_u16(p, 6);
        if ((uint32_t)8 + text_len != frame->len) { NACK(frame, BYOK_E_BAD_LENGTH, 8); break; }
        if (font_id >= BYOK_DISPLAY_FONT_COUNT) { NACK(frame, BYOK_E_BAD_PARAM, 4); break; }
        if (x >= BYOK_DISPLAY_W || y >= BYOK_DISPLAY_H) { NACK(frame, BYOK_E_OUT_OF_RANGE, 0); break; }
        /* ASCII-per-byte, not a real UTF-8 decoder: a multi-byte UTF-8
         * sequence draws one fallback-box glyph per raw byte rather than one
         * box per codepoint. See byok_font8x8.h and README "Guesses and things to
         * verify". */
        if (!menu_owns_display()) {
            byok_display_draw_text(x, y, style, (const char *)(p + 8), text_len);
        }
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_DRAW_RECT: {
        if (frame->len != 10) { NACK(frame, BYOK_E_BAD_LENGTH, 10); break; }
        uint16_t x = rd_u16(p, 0), y = rd_u16(p, 2), w = rd_u16(p, 4), h = rd_u16(p, 6);
        uint8_t op = p[8], value = p[9];
        if (op > BYOK_RECT_OP_CLEAR_REGION) { NACK(frame, BYOK_E_BAD_PARAM, 8); break; }
        /* Validate the FULL rectangle here rather than leaning on
         * byok_display_fill_rect's internal per-axis clipping: the clip is
         * correct today, but the protocol's own E_OUT_OF_RANGE contract
         * should be enforced at the dispatch layer so a future display-side
         * refactor that drops a clip can't turn a NACK-able frame into an
         * out-of-bounds framebuffer write. uint32_t math so x+w can't wrap. */
        if (w == 0 || h == 0 ||
            (uint32_t)x + w > BYOK_DISPLAY_W || (uint32_t)y + h > BYOK_DISPLAY_H) {
            NACK(frame, BYOK_E_OUT_OF_RANGE, 0);
            break;
        }
        if (!menu_owns_display()) {
            byok_display_fill_rect(x, y, w, h, op, value != 0);
        }
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_DRAW_BITMAP: {
        if (frame->len < 10) { NACK(frame, BYOK_E_BAD_LENGTH, 10); break; }
        uint16_t x = rd_u16(p, 0), y = rd_u16(p, 2), w = rd_u16(p, 4), h = rd_u16(p, 6);
        uint8_t bpp = p[8], op = p[9];
        if (frame->flags & BYOK_FLAG_RLE) { NACK(frame, BYOK_E_UNSUPPORTED, 2); break; }
        if (bpp != 1 && bpp != 2) { NACK(frame, BYOK_E_BAD_PARAM, 8); break; }
        if (x >= BYOK_DISPLAY_W || y >= BYOK_DISPLAY_H) { NACK(frame, BYOK_E_OUT_OF_RANGE, 0); break; }
        if (menu_owns_display()) {
            /* Validate the same way byok_display_draw_bitmap()'s own
             * draw_bitmap_impl() would (docs/protocol.md Sec.6.2: "n must
             * equal ceil(w * bpp / 8) * h"), without actually drawing --
             * see this file's own menu_owns_display() comment. */
            size_t stride = ((size_t)w * bpp + 7) / 8;
            size_t expected = stride * (size_t)h;
            if (w == 0 || h == 0 || (size_t)(frame->len - 10) != expected) {
                NACK(frame, BYOK_E_BAD_LENGTH, 10);
                break;
            }
            maybe_ack(frame);
            break;
        }
        bool ok = byok_display_draw_bitmap(x, y, w, h, bpp, op, p + 10, (size_t)(frame->len - 10));
        if (!ok) { NACK(frame, BYOK_E_BAD_LENGTH, 10); break; }
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_FRAME_BEGIN: {
        if (frame->len != 8) { NACK(frame, BYOK_E_BAD_LENGTH, 8); break; }
        if (s_frame.open) { NACK(frame, BYOK_E_STATE, 0); break; } /* Sec.6.3: "a second while one is open is NACK/E_STATE" */
        uint16_t w = rd_u16(p, 0), h = rd_u16(p, 2);
        uint8_t bpp = p[4], fflags = p[5];
        if (fflags & 0x01) {
            ESP_LOGW(TAG, "FRAME_BEGIN: partial-frame flag set -- the v1 origin-in-FRAME_BEGIN "
                          "path (Sec.6.3 'v1 note') is not implemented in this firmware");
            NACK(frame, BYOK_E_UNSUPPORTED, 0);
            break;
        }
        if (bpp != 1) { NACK(frame, BYOK_E_UNSUPPORTED, 1); break; } /* cap bit1: no native 2bpp */
        if (w != BYOK_DISPLAY_W || h != BYOK_DISPLAY_H) { NACK(frame, BYOK_E_OUT_OF_RANGE, 0); break; }
        size_t decl = (((size_t)w + 7) / 8) * h;
        if (decl > BYOK_MAX_DECLARED_FRAME_BYTES) { NACK(frame, BYOK_E_OUT_OF_RANGE, 0); break; }
        uint8_t *buf = (uint8_t *)malloc(decl);
        if (buf == NULL) { NACK(frame, BYOK_E_NO_MEM, (uint8_t)((decl + 1023) / 1024)); break; }
        memset(buf, 0, decl);
        s_frame.open = true;
        s_frame.w = w;
        s_frame.h = h;
        s_frame.bpp = bpp;
        s_frame.buf = buf;
        s_frame.buf_len = decl;
        if (fflags & 0x02) {
            byok_display_clear(false);
        }
        esp_timer_start_once(s_frame_timer, 3000000ULL);
        send_ack(frame->seq); /* FRAME_BEGIN always ACKed */
        break;
    }

    case BYOK_TYPE_FRAME_DATA: {
        if (frame->len < 4) { NACK(frame, BYOK_E_BAD_LENGTH, 4); break; }
        if (!s_frame.open) { NACK(frame, BYOK_E_STATE, 0); break; }
        if (frame->flags & BYOK_FLAG_RLE) { NACK(frame, BYOK_E_UNSUPPORTED, 2); break; }
        uint32_t offset = rd_u32(p, 0);
        size_t n = (size_t)frame->len - 4;
        if ((uint64_t)offset + n > s_frame.buf_len) { NACK(frame, BYOK_E_OUT_OF_RANGE, 0); break; }
        memcpy(s_frame.buf + offset, p + 4, n);
        esp_timer_stop(s_frame_timer);
        /* Clear any timeout that fired in the (tiny) window before stop()
         * took effect, so it can't be misapplied to the timer we're about
         * to (re)start below. */
        s_frame_timeout_pending = false;
        esp_timer_start_once(s_frame_timer, 3000000ULL);
        if (frame->flags & BYOK_FLAG_ACK_REQ) {
            send_ack(frame->seq);
        }
        break;
    }

    case BYOK_TYPE_FRAME_END: {
        if (frame->len != 5) { NACK(frame, BYOK_E_BAD_LENGTH, 5); break; }
        if (!s_frame.open) { NACK(frame, BYOK_E_STATE, 0); break; }
        uint8_t refresh = p[4];
        if (refresh > 3) { NACK(frame, BYOK_E_BAD_PARAM, 4); break; } /* transaction stays open */
        esp_timer_stop(s_frame_timer);
        s_frame_timeout_pending = false; /* see the FRAME_DATA comment above */

        uint32_t want_crc = rd_u32(p, 0);
        uint32_t got_crc = byok_crc32(s_frame.buf, s_frame.buf_len);
        if (got_crc != want_crc) {
            NACK(frame, BYOK_E_FRAME_CRC, 0);
            free_frame_txn();
            break;
        }
        if (menu_owns_display()) {
            /* The frame is CRC-valid and fully accounted for -- ACK it,
             * exactly as documented -- but leave the menu on the glass
             * rather than loading it into the (menu-owned) framebuffer.
             * See this file's own menu_owns_display() comment. */
            free_frame_txn();
            send_ack(frame->seq);
            break;
        }

        bool ok = byok_display_load_full_raster_1bpp(s_frame.buf, s_frame.buf_len);
        free_frame_txn();
        if (!ok) { NACK(frame, BYOK_E_INTERNAL, 0); break; }

        esp_err_t rerr = ESP_OK;
        if (refresh != 0) {
            /* 1 (partial-changed-region), 2 (full) and 3 (device-chooses) all
             * become a full refresh -- this firmware does not track a dirty
             * sub-rectangle across a FRAME_* transaction. */
            rerr = byok_display_full_refresh(false);
        }
        if (rerr != ESP_OK) { NACK(frame, BYOK_E_INTERNAL, 0); break; }
        send_ack(frame->seq);
        break;
    }

    case BYOK_TYPE_PARTIAL_REFRESH: {
        if (frame->len != 8) { NACK(frame, BYOK_E_BAD_LENGTH, 8); break; }
        uint16_t x = rd_u16(p, 0), y = rd_u16(p, 2), w = rd_u16(p, 4), h = rd_u16(p, 6);
        /* Full-rectangle check, not just the origin -- see the DRAW_RECT
         * comment above. */
        if (w == 0 || h == 0 ||
            (uint32_t)x + w > BYOK_DISPLAY_W || (uint32_t)y + h > BYOK_DISPLAY_H) {
            NACK(frame, BYOK_E_OUT_OF_RANGE, 0);
            break;
        }
        if (!menu_owns_display()) {
            esp_err_t e = byok_display_partial_refresh(x, y, w, h);
            if (e != ESP_OK) { NACK(frame, BYOK_E_INTERNAL, 0); break; }
        }
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_FULL_REFRESH: {
        if (frame->len != 1) { NACK(frame, BYOK_E_BAD_LENGTH, 1); break; }
        if (!menu_owns_display()) {
            esp_err_t e = byok_display_full_refresh((p[0] & 0x01) != 0);
            if (e != ESP_OK) { NACK(frame, BYOK_E_INTERNAL, 0); break; }
        }
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_SET_MODE: {
        if (frame->len != 2) { NACK(frame, BYOK_E_BAD_LENGTH, 2); break; }
        uint8_t mode = p[0]; /* p[1] persist-flag: still accepted-and-ignored, see below */
        /* 0.1.13: values 4/5/6 (CLOCK/STATIC_NOTE/BLANK) added -- the enum
         * had room (a full u8, only 0-3 used through 0.1.12) and
         * docs/protocol.md Sec.13's compatibility rule treats a new legal
         * value for an EXISTING field the same as a new capability bit or
         * flag bit: additive within v1, a v1.0-only host simply never sends
         * it, a v1.0-only... there is no "v1.0-only device" distinction here
         * since this firmware IS the only device-side implementation, but a
         * host that doesn't know about 4/5/6 continues to work exactly as
         * before (it never sends them). See docs/protocol.md Sec.6.4. */
        /* 0.1.14: value 7 (MENU) added -- see docs/protocol.md Sec.6.4's
         * updated table and byok_menu.h. Whenever an explicit mode OTHER
         * than 7 arrives while the preset menu happens to be open (button-
         * triggered, e.g.), byok_menu_abort() resets byok_menu's own
         * open/deadline bookkeeping WITHOUT itself touching byok_modes --
         * this switch is about to do that right below. Without this, a
         * later byok_menu_task() timeout tick would try to "restore" the
         * mode this SET_MODE just explicitly overrode. See byok_menu.h's
         * own header comment ("byok_menu_abort()") for the full rationale. */
        if (mode > 7) { NACK(frame, BYOK_E_BAD_PARAM, 0); break; }
        if (mode != 7) {
            byok_menu_abort();
        }
        s_link_mode = mode;
        /* 0.1.12: drive byok_modes' own device-level mode from the link's
         * SET_MODE, per byok_modes.h's own "rough correspondence" comment --
         * IDLE (0) is the device's own screen, HOST (1) and MIRROR (2) both
         * mean "the host owns the display" (DASHBOARD_USB, the
         * host-renders path this firmware already implements for every
         * DRAW_x/FRAME_x handler below). SLEEP (3) is left as pure
         * bookkeeping, unchanged from every prior release (s_link_mode above
         * still records it for STATUS/EVT_STATUS) -- this firmware has no
         * real display-off SLEEP behaviour to switch byok_modes into.
         * 0.1.13: IDLE (0) now enters whichever idle submode the owner last
         * selected (byok_idle_enter_selected_submode(), byok_idle.h) instead
         * of hard-coding CLOCK; 4/5/6 explicitly force one of the three
         * regardless of what was previously selected (byok_idle_force_
         * submode()), which ALSO becomes the new selection for a later
         * IDLE/idle-timeout/UP/DOWN. A byok_modes_set() failure inside
         * either byok_idle call can only be ESP_ERR_INVALID_ARG for a mode
         * value outside byok_app_mode_t's range, which byok_idle.c's own
         * fixed CLOCK/STATIC_NOTE/BLANK table never produces -- intentionally
         * not NACKed on that basis, same as every case here before this
         * pass. */
        switch (mode) {
        case 0: byok_idle_enter_selected_submode(); break;
        case 1:
        case 2: byok_modes_set(BYOK_MODE_DASHBOARD_USB); break;
        case 4: byok_idle_force_submode(BYOK_MODE_CLOCK); break;
        case 5: byok_idle_force_submode(BYOK_MODE_STATIC_NOTE); break;
        case 6: byok_idle_force_submode(BYOK_MODE_BLANK); break;
        case 7: byok_menu_open_default(); break; /* 0.1.14: MENU, docs/protocol.md Sec.6.4 */
        default: break; /* 3 (SLEEP) */
        }
        send_ack(frame->seq); /* SET_MODE always ACKed */
        break;
    }

    case BYOK_TYPE_SET_BACKLIGHT: {
        if (frame->len != 3) { NACK(frame, BYOK_E_BAD_LENGTH, 3); break; }
        uint8_t level = p[0];
        uint16_t fade_ms = rd_u16(p, 1);
        if (fade_ms > 5000) { NACK(frame, BYOK_E_BAD_PARAM, 1); break; }
        esp_err_t e = byok_display_set_backlight(level, fade_ms);
        if (e != ESP_OK) { NACK(frame, BYOK_E_INTERNAL, 0); break; }
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_SET_CONTRAST: {
        if (frame->len != 1) { NACK(frame, BYOK_E_BAD_LENGTH, 1); break; }
        esp_err_t e = byok_display_set_contrast(p[0]);
        if (e != ESP_OK) { NACK(frame, BYOK_E_INTERNAL, 0); break; }
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_GET_BATTERY: {
        if (frame->len != 0) { NACK(frame, BYOK_E_BAD_LENGTH, 0); break; }
        /* Deliberately unimplemented, not blocked on anything -- see this
         * file's header comment. mv/pct/charging are real (components/
         * byok_battery) and already on the wire via GET_STATUS/STATUS;
         * current_ma is the one field only this dedicated command would
         * add, and there is no current sensor to report it with. */
        NACK(frame, BYOK_E_UNSUPPORTED, 6);
        break;
    }

    case BYOK_TYPE_REBOOT: {
        if (frame->len != 2) { NACK(frame, BYOK_E_BAD_LENGTH, 2); break; }
        uint16_t magic = rd_u16(p, 0);
        if (magic != 0x5245u) { NACK(frame, BYOK_E_BAD_PARAM, 0); break; }
        send_ack(frame->seq);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        break; /* unreached */
    }

    case BYOK_TYPE_BOOT_ORIGINAL: {
        if (frame->len != 4) { NACK(frame, BYOK_E_BAD_LENGTH, 4); break; }
        uint32_t magic = rd_u32(p, 0);
        if (magic != 0x424B5354u) { NACK(frame, BYOK_E_BAD_PARAM, 0); break; }
#if CONFIG_BYOK_ALLOW_OTADATA_WRITE
        const esp_partition_t *stock = NULL;
        stock_lookup_t r = byok_find_stock_partition(&stock);
        if (r == STOCK_NOT_FOUND) { NACK(frame, BYOK_E_NOT_FOUND, 0); break; }
        if (r == STOCK_AMBIGUOUS) { NACK(frame, BYOK_E_NOT_PERMITTED, 0); break; }
        esp_err_t e = esp_ota_set_boot_partition(stock);
        if (e != ESP_OK) { NACK(frame, BYOK_E_INTERNAL, 0); break; }
        byok_modes_set(BYOK_MODE_ORIGINAL);
        send_ack(frame->seq);
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
#else
        ESP_LOGW(TAG, "BOOT_ORIGINAL requested over the link but BYOK_ALLOW_OTADATA_WRITE "
                      "is OFF (default) -- see main/Kconfig.projbuild");
        NACK(frame, BYOK_E_NOT_PERMITTED, 0);
#endif
        break;
    }

    /* 0.1.12: docs/protocol.md Sec.6.4 v1.1 note, byok_rtc component. Sets
     * the PCF8563 (hw_config.h Sec.2). Device-side only this release -- no
     * host sends this yet (host/macos/byok/proto.py is maintained
     * separately, see this file's own module comment / SAFETY.md
     * Sec.1); the wire format is fixed and documented so that host change
     * can follow later without a device-side revision. Not blocking on it:
     * the RTC already keeps correct time across power-off on its own
     * (hw_config.h Sec.2) and stock firmware sets it from the network, so
     * CLOCK mode (byok_clock) only needs to READ it -- this handler exists
     * for completeness / a future re-sync path, not because reading alone
     * needs it. */
    case BYOK_TYPE_SET_TIME: {
        if (frame->len != 8) { NACK(frame, BYOK_E_BAD_LENGTH, 8); break; }
        byok_rtc_time_t t = {
            .year    = rd_u16(p, 0),
            .month   = p[2],
            .day     = p[3],
            .weekday = p[4],
            .hour    = p[5],
            .minute  = p[6],
            .second  = p[7],
        };
        /* Validated field-by-field (not left to byok_rtc_write()'s own
         * single generic ESP_ERR_INVALID_ARG) so E_BAD_PARAM's `detail` can
         * be the actual byte offset of the bad field, per docs/protocol.md
         * Sec.9's own convention for that code -- byok_rtc_write() would
         * reject the same values but can't tell dispatch() which one. */
        if (t.year < 2000 || t.year > 2199) { NACK(frame, BYOK_E_BAD_PARAM, 0); break; }
        if (t.month < 1 || t.month > 12)    { NACK(frame, BYOK_E_BAD_PARAM, 2); break; }
        if (t.day < 1 || t.day > 31)        { NACK(frame, BYOK_E_BAD_PARAM, 3); break; }
        if (t.weekday > 6)                  { NACK(frame, BYOK_E_BAD_PARAM, 4); break; }
        if (t.hour > 23)                    { NACK(frame, BYOK_E_BAD_PARAM, 5); break; }
        if (t.minute > 59)                  { NACK(frame, BYOK_E_BAD_PARAM, 6); break; }
        if (t.second > 59)                  { NACK(frame, BYOK_E_BAD_PARAM, 7); break; }
        esp_err_t e = byok_rtc_write(&t);
        if (e == ESP_ERR_INVALID_STATE) { NACK(frame, BYOK_E_UNSUPPORTED, 0); break; }
        if (e != ESP_OK) { NACK(frame, BYOK_E_INTERNAL, 0); break; }
        maybe_ack(frame);
        break;
    }

    /* 0.1.13: docs/protocol.md Sec.6.4 v1.1 note, byok_note component. No
     * fixed-field header, unlike DRAW_TEXT -- the whole payload is the raw
     * UTF-8/ASCII note text (0-200 bytes), word-wrapped device-side by
     * byok_note_render() using this firmware's own actual panel/font
     * geometry (30 cols x 10 rows at 8x8, see byok_note.h). Replaces the
     * in-RAM note immediately and re-renders live if BYOK_MODE_STATIC_NOTE
     * is the current idle submode; persists it too when CONFIG_BYOK_NVS_
     * PERSIST_ARMED is on (byok_nvs.h) -- off by default, see that
     * component's Kconfig for why. */
    case BYOK_TYPE_SET_NOTE: {
        if (frame->len > BYOK_NVS_NOTE_MAX_LEN) {
            NACK(frame, BYOK_E_BAD_LENGTH, (uint8_t)BYOK_NVS_NOTE_MAX_LEN);
            break;
        }
        esp_err_t e = byok_note_set((const char *)p, frame->len);
        if (e != ESP_OK) { NACK(frame, BYOK_E_INTERNAL, 0); break; }
        maybe_ack(frame);
        break;
    }

    /* 0.1.14: docs/protocol.md Sec.6.4b, byok_menu component. Payload is
     * `count` (u8) followed by `count` 20-byte NUL-padded names -- no
     * fixed header beyond the count byte, same "device owns the layout"
     * shape SET_NOTE already established. LEN is checked against the
     * EXACT expected size for `count` (not merely >=) since, unlike
     * SET_NOTE's free-form text, a short/long trailing name here would
     * silently misalign every name after it. */
    case BYOK_TYPE_SET_PRESETS: {
        if (frame->len < 1) { NACK(frame, BYOK_E_BAD_LENGTH, 1); break; }
        uint8_t count = p[0];
        if (count > BYOK_PRESET_MAX_COUNT) { NACK(frame, BYOK_E_BAD_PARAM, 0); break; }
        uint32_t expected = 1u + (uint32_t)count * BYOK_PRESET_NAME_LEN;
        if ((uint32_t)frame->len != expected) {
            NACK(frame, BYOK_E_BAD_LENGTH, (uint8_t)(expected > 0xFFu ? 0xFFu : expected));
            break;
        }
        esp_err_t e = byok_menu_set_presets(count, p + 1);
        if (e != ESP_OK) { NACK(frame, BYOK_E_BAD_PARAM, 0); break; }
        maybe_ack(frame);
        break;
    }

    /* 0.1.14: docs/protocol.md Sec.6.4b, byok_docstats component. Always
     * replies with the most recently COMPLETED background scan's
     * snapshot -- never blocks on, or triggers, a fresh SD-card scan
     * itself (that would stall the dispatch task on FATFS/SDMMC I/O,
     * which every other handler in this switch is written never to do). */
    case BYOK_TYPE_GET_DOCSTATS: {
        if (frame->len != 0) { NACK(frame, BYOK_E_BAD_LENGTH, 0); break; }
        byok_docstats_t st;
        byok_docstats_get(&st);
        uint8_t payload[20];
        wr_u32(payload, 0, st.files);
        wr_u32(payload, 4, st.words);
        wr_u32(payload, 8, st.bytes);
        wr_u32(payload, 12, st.newest_epoch);
        wr_u32(payload, 16, st.words_today);
        send_reply(BYOK_TYPE_DOCSTATS, BYOK_FLAG_IS_REPLY, frame->seq, payload, sizeof(payload));
        break;
    }

    /* 0.1.14: docs/protocol.md Sec.6.4b, byok_display's runtime bulk/
     * per-byte toggle. Takes effect on the panel's NEXT refresh -- does
     * not itself trigger one. */
    case BYOK_TYPE_DISPLAY_CFG: {
        if (frame->len != 1) { NACK(frame, BYOK_E_BAD_LENGTH, 1); break; }
        byok_display_set_bulk_writes((p[0] & 0x01) != 0);
        maybe_ack(frame);
        break;
    }

    case BYOK_TYPE_ACK:
    case BYOK_TYPE_NACK:
    case BYOK_TYPE_PONG:
        ESP_LOGD(TAG, "unsolicited reply-type frame 0x%02X from host, ignoring", frame->type);
        break;

    default:
        NACK(frame, BYOK_E_UNKNOWN_TYPE, frame->type);
        break;
    }
}

/* ==========================================================================
 * byok_proto callbacks
 * ========================================================================== */

static void byok_on_error(void *ctx, byok_parse_err_t err, const uint8_t *header, size_t header_len)
{
    (void)ctx;
    uint8_t code, detail = 0;
    switch (err) {
    case BYOK_ERR_BAD_VERSION: code = BYOK_E_BAD_VERSION; detail = BYOK_VERSION; break;
    case BYOK_ERR_BAD_LENGTH:  code = BYOK_E_BAD_LENGTH;  break;
    case BYOK_ERR_BAD_CRC:     code = BYOK_E_BAD_CRC;     break;
    default:                   code = BYOK_E_INTERNAL;    break;
    }
    uint16_t seq_echo = BYOK_SEQ_ECHO_UNKNOWN;
    if (header_len >= 7) {
        seq_echo = rd_u16(header, 5);
    }
    send_nack_raw(seq_echo, code, detail);
}

static void byok_on_frame(void *ctx, const byok_frame_t *frame)
{
    (void)ctx;

    /* 0.1.12: byok_modes.h BYOK_MODE_CLOCK / byok_clock component. "Any
     * host frame switches back to DASHBOARD_USB" (docs/protocol.md Sec.6.4
     * v1.1 note) is read literally here -- every frame this dispatcher
     * sees at all, including a duplicate or a seq-gap one, counts as "the
     * host is there" and resets byok_clock's idle timer / forces the mode
     * back if CLOCK is currently showing. Deliberately BEFORE the
     * seq_gap/duplicate handling below, not after: those are still real
     * evidence of a live host on the wire, just not ones dispatch() itself
     * needs to act on. */
    byok_clock_notify_host_frame();

    if (frame->seq_gap != 0) {
        /* Sec.7.5: informational NACK, THEN process the frame normally --
         * two replies for one incoming frame is intentional here. */
        send_nack_raw(frame->seq, BYOK_E_SEQ_GAP, frame->seq_gap);
    }

    if (frame->is_duplicate) {
        /* Sec.7.5 asks for "the previous reply... re-sent". This firmware
         * keeps no last-reply cache, so it approximates with a bare ACK --
         * correct for CLEAR/DRAW_* /SET_*, not byte-identical for a duplicate
         * HELLO/GET_* whose original reply carried a typed payload. See
         * README "Guesses and things to verify". */
        if (frame->flags & BYOK_FLAG_ACK_REQ) {
            send_ack(frame->seq);
        }
        return;
    }

    dispatch(frame);
}

/* ==========================================================================
 * Transport plumbing
 * ========================================================================== */

static void cdc_rx_bridge(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (s_rx_stream == NULL) {
        return;
    }
    size_t n = xStreamBufferSend(s_rx_stream, data, len, 0);
    if (n < len) {
        ESP_LOGW(TAG, "RX stream buffer full, dropped %u of %u bytes", (unsigned)(len - n), (unsigned)len);
    }
}

static void dispatch_task(void *arg)
{
    (void)arg;
    uint8_t chunk[256];
    for (;;) {
        /* Bounded wait (not portMAX_DELAY) so a stalled frame transaction
         * gets its timeout applied promptly even when the host has gone
         * silent and no more bytes are arriving to wake this task. */
        size_t n = xStreamBufferReceive(s_rx_stream, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        frame_check_timeout();
        if (n > 0) {
            byok_parser_feed(&s_rx_parser, chunk, n);
            frame_check_timeout();
        }
    }
}

/* ==========================================================================
 * Boot-time helpers
 * ========================================================================== */

static void compute_device_id(uint8_t out[8])
{
    uint8_t mac[6] = { 0 };
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed (%s); device_id will hash an all-zero MAC", esp_err_to_name(err));
    }

    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0 /* SHA-256, not SHA-224 */);
    mbedtls_sha256_update(&ctx, mac, sizeof(mac));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    memcpy(out, hash, 8); /* docs/protocol.md Sec.6.1: first 8 bytes of SHA-256(MAC) */
}

/* Power latch. [CONFIRMED] firmware/common/hw_config.h Sec.9
 * (disassembly of both 1.1.0 and 1.1.3): GPIO 42 is configured OUTPUT and
 * driven LOW (BYOK_POWER_ON_LEVEL) by the stock firmware immediately at
 * boot (SYS_APP init, file 0x0B69E4..0x0B6A26) and stays low for the whole
 * time it is running; the shutdown path drives it HIGH
 * (BYOK_POWER_OFF_LEVEL, file 0x0B68AA) and then spins forever, which is
 * what actually cuts power via the board's external power-hold circuit —
 * on USB power the stock firmware refuses to do this at all ("On USB
 * power, can't power off"), which is CONFIRMED evidence this pin gates
 * battery power specifically, not just a status LED or similar. Driving it
 * LOW is therefore what keeps the device powered on battery; leaving it
 * floating (its default reset state) risks the external latch circuit
 * reading an undriven/high input as "power off" and cutting the rail
 * before any of the rest of this firmware runs. Set as the very first
 * action in app_main(), before anything else, output level LOW
 * (BYOK_POWER_ON_LEVEL), matching stock's own boot-time configuration and
 * polarity exactly -- never driven HIGH anywhere in this firmware. Fail-
 * safe if this analysis is somehow wrong: worst case on battery the
 * device powers off (same as an unconfigured/floating pin might already
 * do); on USB power the stock firmware's own logic (external to this
 * MCU pin, in the power circuit) additionally refuses to let this signal
 * cut power at all, so there is no failure mode that reaches beyond a
 * power cycle. Touches no other GPIO. */
static void init_power_hold_gpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BYOK_POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(BYOK_POWER_HOLD_GPIO, BYOK_POWER_ON_LEVEL);
}

/* Stock GPIO parity (0.1.6, CONFIG_BYOK_STOCK_GPIO_PARITY, default y) --
 * see docs/pinout.md §5 for the full evidence table this reproduces.
 * Stock's own SYS_APP init configures GPIOs 17/35/39/42/47 as OUTPUT in one
 * gpio_config() call and then drives all five LOW, back-to-back, in the
 * order 17, 35, 47, 39, 42 (docs/pinout.md §5 records the order) -- as the
 * very first GPIO action of boot, before any I2C bus or the display
 * (docs/pinout.md §5). GPIO 42 is already reproduced above by
 * init_power_hold_gpio() (same polarity, same "first action of boot"
 * placement) and GPIO 17 is reserved for a future PICO-reset
 * implementation (BYOK_PICO_RESET_GPIO, not yet driven by anything in this
 * firmware), so this function covers only the remaining three: 35
 * (STRONGLY INDICATED PICO REQ line, BYOK_PICO_REQ_GPIO), 47 (STRONGLY
 * INDICATED PICO STROBE line, BYOK_PICO_STROBE_GPIO), and 39 (UNKNOWN
 * function, BYOK_GPIO39_PIN -- the 2026-09-03 blank-display incident's
 * panel-supply/level-shifter hypothesis is POSSIBLE, not CONFIRMED,
 * docs/recovery.md §6 and docs/pinout.md §5).
 *
 * Configures all three as OUTPUT in one gpio_config() call (mirroring
 * stock's own single call covering multiple pins at once), then drives
 * each LOW individually in stock's own relative order for this subset --
 * 35, then 47, then 39 -- with no delay between the three
 * gpio_set_level() calls, matching the disassembly exactly (back-to-back
 * instructions, no vTaskDelay anywhere in that block). Touches no pin
 * outside {35, 39, 47} -- 17 and 42 are deliberately out of scope, see
 * above. By construction this can never do anything the stock firmware
 * does not already do on every boot of this exact board; it is a
 * stock-parity change, not a claimed display fix (Kconfig help text has
 * the full reasoning). Logged-and-continue on failure, like every other
 * non-essential bring-up step in this file. */
static void init_stock_gpio_parity(void)
{
#if CONFIG_BYOK_STOCK_GPIO_PARITY
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BYOK_PICO_REQ_GPIO) | (1ULL << BYOK_PICO_STROBE_GPIO) |
                        (1ULL << BYOK_GPIO39_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BYOK_STOCK_GPIO_PARITY: gpio_config(35/39/47) failed: %s (continuing, "
                      "GPIO 35/39/47 left unconfigured)", esp_err_to_name(err));
        return;
    }

    /* Stock order for this subset (docs/pinout.md §5): 35, 47, 39.
     * No delay between these three calls -- stock has none either. */
    gpio_set_level(BYOK_PICO_REQ_GPIO, 0);
    ESP_LOGI(TAG, "BYOK_STOCK_GPIO_PARITY: GPIO%d (PICO REQ) OUTPUT, LOW", (int)BYOK_PICO_REQ_GPIO);
    gpio_set_level(BYOK_PICO_STROBE_GPIO, 0);
    ESP_LOGI(TAG, "BYOK_STOCK_GPIO_PARITY: GPIO%d (PICO STROBE) OUTPUT, LOW", (int)BYOK_PICO_STROBE_GPIO);
    gpio_set_level(BYOK_GPIO39_PIN, 0);
    ESP_LOGI(TAG, "BYOK_STOCK_GPIO_PARITY: GPIO%d (unknown function, stock-parity only) OUTPUT, LOW",
             (int)BYOK_GPIO39_PIN);
#else
    ESP_LOGI(TAG, "BYOK_STOCK_GPIO_PARITY off -- GPIO 35/39/47 left unconfigured (0.1.0-0.1.5 behaviour)");
#endif
}

static void init_sd_detect_gpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BYOK_SD_DETECT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BYOK_SD_DETECT_INTERNAL_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

/* Boot-time EXECUTE (GPIO16) hold -> BOOT_ORIGINAL, mirroring the
 * BOOT_ORIGINAL protocol handler's own safety gate (docs/protocol.md
 * Sec.6.4, D-011: "the same action is available without a host by holding
 * EXECUTE (GPIO16) for 3 s at boot"). */
static void check_boot_button(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BYOK_BTN_EXECUTE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* defensive; hw_config.h: board supplies its own external pull-ups */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    if (gpio_get_level(BYOK_BTN_EXECUTE_GPIO) != 0) { /* active-low: 0 = pressed */
        return;
    }

    ESP_LOGI(TAG, "EXECUTE held at boot -- watching for a 3 s hold (BOOT_ORIGINAL)");
    const int step_ms = 100, need_ms = 3000;
    int held = 0;
    while (held < need_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        if (gpio_get_level(BYOK_BTN_EXECUTE_GPIO) != 0) {
            ESP_LOGI(TAG, "EXECUTE released early -- continuing normal boot");
            return;
        }
        held += step_ms;
    }
    ESP_LOGW(TAG, "EXECUTE held 3 s -- BOOT_ORIGINAL requested from hardware");

#if CONFIG_BYOK_ALLOW_OTADATA_WRITE
    const esp_partition_t *stock = NULL;
    if (byok_find_stock_partition(&stock) == STOCK_FOUND) {
        esp_ota_set_boot_partition(stock);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "BOOT_ORIGINAL (button): no unambiguous stock partition found");
    }
#else
    ESP_LOGW(TAG, "BYOK_ALLOW_OTADATA_WRITE is OFF (default) -- ignoring the button request. "
                  "See main/Kconfig.projbuild.");
#endif
}

/* ==========================================================================
 * SD updater on-glass status line
 * ========================================================================== */

/* Set true only after byok_display_init() returns ESP_OK, below. The SD
 * updater runs BEFORE that call (see app_main's own boot-order comment at
 * the top of this file and components/byok_sd_updater's header comment --
 * it must run first so a found-and-installed update reboots before any
 * display/USB bring-up), so in this build sd_updater_status_cb() always
 * takes the log-only branch: the display is never yet initialised at the
 * point the SD updater calls it. The check is still real, not dead code --
 * it is what makes this safe to call from wherever the updater runs,
 * without assuming today's boot order forever, and without ever touching
 * the display driver before byok_display_init() has actually succeeded
 * (which would be undefined behaviour, not just a missed status line). */
static bool s_display_ready = false;

static void sd_updater_status_cb(void *ctx, const char *step)
{
    (void)ctx;
    if (!s_display_ready) {
        ESP_LOGI(TAG, "SD UPDATE: %s", step);
        return;
    }
    /* Bottom text row (y=72..79, the panel's last 8-row page) so this never
     * collides with whatever byok_display_run_boot_selftest() draws later --
     * not reachable in this build (see comment above), but kept consistent
     * with that layout for whenever it is. */
    char line[40];
    int n = snprintf(line, sizeof(line), "SD UPDATE: %s", step);
    if (n < 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    byok_display_fill_rect(0, 72, BYOK_DISPLAY_W, 8, BYOK_RECT_OP_CLEAR_REGION, false);
    byok_display_draw_text(0, 72, 0, line, len);
    byok_display_full_refresh(false);
}

/* ==========================================================================
 * SD updater task wrapper -- 2026-09-03 stack-overflow fix (0.1.3)
 * ========================================================================== */

/* byok_sd_updater_check_and_run() (components/byok_sd_updater/byok_sd_
 * updater.c) used to declare a 512 B tar-header buffer in walk_tar() plus,
 * when CONFIG_BYOK_SD_UPDATER_ARMED=y, a 4096 B streaming buffer in
 * install_member() -- 4608 B of locals alone, before the esp_ota_write() /
 * esp_partition_write() / mbedtls-SHA-256 / FATFS / sdmmc call frames
 * underneath them. Called directly from here (i.e. on task "main", which
 * had only CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584 B), that did not fit and
 * panicked with "A stack overflow in task main has been detected" the
 * instant an update actually started installing, in every build from
 * 0.1.0 through 0.1.2 -- see docs/troubleshooting.md.
 *
 * Two independent layers fix this (belt-and-braces, not either/or):
 *
 *  1. byok_sd_updater.c's own two buffers are no longer task-stack locals
 *     at all -- they are now heap_caps_malloc(MALLOC_CAP_INTERNAL |
 *     MALLOC_CAP_8BIT) allocations, freed on every return path. See that
 *     file's own comments at each allocation site. This alone would have
 *     been enough to fit in "main"'s original 3584 B.
 *  2. The updater still runs on its OWN short-lived task
 *     (SD_UPDATER_TASK_STACK_BYTES below) sized for the FATFS/sdmmc/
 *     esp_ota/esp_partition/mbedtls-SHA-256 call chain that runs under
 *     those now-heap buffers, rather than borrowing "main"'s -- so a
 *     future buffer added to that call chain overflows a task whose only
 *     job is this scan, not the task everything else in this file also
 *     depends on. app_main() blocks on a task notification until the
 *     dedicated task completes, so the documented boot order (updater
 *     must finish before display/USB bring-up) is unchanged. Plain
 *     xTaskCreate() (not xTaskCreateStatic with a caller-supplied buffer)
 *     -- ESP-IDF's FreeRTOS port (freertos/heap_idf.c, pvPortMalloc)
 *     unconditionally services every FreeRTOS allocation, task stacks
 *     included, from MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT regardless of
 *     CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY (that Kconfig only matters
 *     for a task the app explicitly creates with its own PSRAM-backed
 *     static buffer) -- so this task's stack is internal RAM by
 *     construction, not by any extra code here.
 *
 * CONFIG_ESP_MAIN_TASK_STACK_SIZE is ALSO raised 3584 -> 8192
 * (sdkconfig.defaults) as defence in depth, independent of either of the
 * above -- see that file's own comment. 12288 B here is margin-by-
 * inspection (largest known call chain underneath the now-heap buffers,
 * never measured on real hardware, never claimed otherwise);
 * sd_updater_task() below logs uxTaskGetStackHighWaterMark() every run so
 * a real install captures the real number and this can be tightened with
 * data instead of margin. tests/static/check_stack_budgets.py enforces
 * that this constant exists and stays >= 8192, and that
 * byok_sd_updater_check_and_run( is never again called directly from
 * app_main()'s own body. */
#define SD_UPDATER_TASK_STACK_BYTES 12288
#define SD_UPDATER_TASK_PRIORITY    5

typedef struct {
    byok_sd_updater_status_cb_t status_cb;
    void *status_ctx;
    esp_err_t result;
    TaskHandle_t caller;
} sd_updater_task_args_t;

static void sd_updater_task(void *arg)
{
    sd_updater_task_args_t *a = (sd_updater_task_args_t *)arg;
    a->result = byok_sd_updater_check_and_run(a->status_cb, a->status_ctx);
    /* Real, on-device evidence for whether SD_UPDATER_TASK_STACK_BYTES is
     * actually right, replacing the margin-by-inspection estimate above --
     * logged unconditionally (not just on a near-miss) so the very next
     * real install captures this exactly once. uxTaskGetStackHighWaterMark()
     * returns words of stack that were NEVER touched, i.e. still-unused
     * headroom, not bytes consumed -- multiply by sizeof(StackType_t) (4 on
     * this target) for bytes. A small number here (not zero -- that would
     * already have panicked) means this constant should grow; a large one
     * means it can eventually shrink towards the real number instead of
     * this estimate. */
    UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "sd_updater_task: stack high-water mark %u B free of %u B (never-touched headroom)",
             (unsigned)(hwm_words * sizeof(StackType_t)), (unsigned)SD_UPDATER_TASK_STACK_BYTES);
    xTaskNotifyGive(a->caller);
    vTaskDelete(NULL);
}

/* Runs byok_sd_updater_check_and_run() on a dedicated task and blocks the
 * calling task (app_main(), i.e. "main") until it finishes. `args` lives on
 * main's own stack frame for the whole call -- safe, because
 * ulTaskNotifyTake() below does not return until sd_updater_task() has
 * already read every field it needs and is on its way to self-delete. */
static esp_err_t run_sd_updater_in_dedicated_task(byok_sd_updater_status_cb_t status_cb, void *status_ctx)
{
    sd_updater_task_args_t args = {
        .status_cb = status_cb,
        .status_ctx = status_ctx,
        .result = ESP_FAIL,
        .caller = xTaskGetCurrentTaskHandle(),
    };
    BaseType_t created = xTaskCreate(sd_updater_task, "byok_sd_upd",
                                      SD_UPDATER_TASK_STACK_BYTES, &args,
                                      SD_UPDATER_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(byok_sd_upd) failed -- skipping SD updater check this boot");
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return args.result;
}

/* esp_reset_reason_t -> a short string, for the boot banner and for the
 * on-glass line in app_main(). Deliberately short: the panel's text row is
 * 30 columns at 8x8 (BYOK_DISPLAY_W / 8), and "RST:" plus the numeric value
 * already costs several of them. docs/recovery.md §3: this
 * is the discriminator for whether the RESET S3 button (switch S1) actually
 * pulls EN/CHIP_PU -- which the repo has only ever inferred from board
 * layout, never measured -- since that decides whether the USB PHY mux and
 * the display controller are reset by it at all. ESP_RST_POWERON here after
 * a RESET S3 press means chip-level (EN); ESP_RST_SW / ESP_RST_INT_WDT /
 * anything else means it is not, and the byok_usb_cdc_restore_usj_phy()
 * call in app_main() is then load-bearing rather than belt-and-braces. */
static const char *byok_reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON/EN";  /* chip reset: POR, brownout, or the EN pin */
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";           /* esp_restart() -- CPU reset, RTC survives */
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    case ESP_RST_EFUSE:     return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWRGLITCH";
    case ESP_RST_CPU_LOCKUP: return "LOCKUP";
    default:                return "UNMAPPED";
    }
}

/* ==========================================================================
 * app_main
 * ========================================================================== */

void app_main(void)
{
    /* Power latch, first action of all -- see init_power_hold_gpio()'s own
     * comment for the full CONFIRMED evidence and polarity. This must run
     * before anything else, including the NVS/boot-button notes below,
     * because it is the one action standing between "reset default GPIO
     * state" and whatever the external power-hold circuit does with an
     * undriven pin. */
    init_power_hold_gpio();

    /* Stock GPIO parity, second action of all (0.1.6, CONFIG_BYOK_STOCK_
     * GPIO_PARITY, default y) -- immediately after the power latch above,
     * matching how close together stock's own equivalent five-pin block
     * sits at the very start of its SYS_APP init, well before anything
     * else in this function (docs/pinout.md §5: no evidence of
     * any deliberate delay, and unconditionally before display init on
     * every boot path). See init_stock_gpio_parity()'s own comment for the
     * full evidence and scope (GPIO 35/39/47 only). */
    init_stock_gpio_parity();

    /* USB-Serial-JTAG PHY hand-back, third action of all -- after the power
     * latch and the stock GPIO parity block (both of which must stay first/
     * second, see above) and before literally everything else, so the
     * USJ/esptool recovery window is open for the whole of this boot rather
     * than only from wherever this happened to be called.
     *
     * On this chip esp_restart() is only a CPU-level reset (observed rst:0xc
     * RTC_SW_CPU_RST), which resets neither the RTC sub-system that holds the
     * USB PHY mux nor the USB peripherals themselves -- ESP-IDF's own
     * esp_system_reset_modules_on_exit() deliberately omits SYSTEM_USB_RST and
     * SYSTEM_USB_DEVICE_RST. So a reboot taken any time after
     * byok_usb_cdc_init() (the SD updater's post-install esp_restart, either
     * BOOT_ORIGINAL path) would otherwise come back with the internal PHY
     * still handed to USB-OTG and the USJ pointed at a non-existent external
     * PHY: no console, and no esptool recovery, until a power cycle. See
     * docs/recovery.md Sec.2 and byok_usb_cdc.h.
     *
     * Reads the mux first and no-ops when the PHY is already the USJ's, so
     * this costs nothing and disturbs nothing on a normal cold boot. */
    byok_usb_cdc_restore_usj_phy();

    /* NOTE (updated 0.1.13, see components/byok_nvs/include/byok_nvs.h for
     * the full reasoning): this firmware still never touches, opens, or
     * writes the vendor's OWN namespace ("BYOK") or any other vendor
     * namespace inside the shared "nvs" partition (0x9000) -- those hold
     * the stock Wi-Fi credentials, BT pairing and DJWT device token, and
     * remain read-never/write-never, exactly as before. What changed: this
     * project no longer has "nothing in NVS" as a blanket statement --
     * byok_nvs_init() (called from app_main(), below), gated behind
     * CONFIG_BYOK_NVS_PERSIST_ARMED (default OFF), CAN open this SAME
     * physical partition under OUR OWN namespace ("byokmod") for SET_NOTE
     * text / idle-mode / backlight-level persistence. This is a deliberate,
     * narrower reading of the rule above than "never nvs_flash_init() at
     * all here": `firmware/s3/partitions.csv`'s own header forbids adding a
     * partition-table entry for a dedicated store (it must mirror the
     * device's actual, fixed table), so there is no dedicated partition to
     * use instead -- namespace isolation within the one partition that
     * exists is the only mechanism available, and byok_nvs.c never calls
     * nvs_flash_erase() under any condition. See SAFETY.md's hard rules,
     * docs/recovery.md, and docs/design-rationale.md D-003 for the safety gate
     * this stays behind (armed OFF by default, pending a future decision
     * record -- not a single release's say-so). */

    /* Boot-button escape hatch first, before anything that can abort via
     * ESP_ERROR_CHECK (display/USB bring-up below) -- see docs/design-rationale.md
     * D-011's boot-time button hold: this is the mechanism that saves us
     * when firmware is too broken to answer USB, so it must run before the
     * code it exists to escape from. */
    check_boot_button();

    /* WAKE (GPIO6) hold-to-power-off handler -- started as early as
     * possible (before the SD updater's own, potentially slow, mount/scan/
     * install work below) so a held WAKE button remains an escape hatch
     * even if boot stalls somewhere later. byok_power_init() only touches
     * GPIO6 and, on a 3+ s hold, GPIO42 (BYOK_POWER_HOLD_GPIO) -- see
     * byok_power.h for exactly what it does and does not reproduce from
     * stock. Logged-and-continue on failure, matching every other
     * non-essential bring-up step in this function: a device that can't
     * button-power-off still boots and works over USB. */
    esp_err_t power_err = byok_power_init();
    if (power_err != ESP_OK) {
        ESP_LOGE(TAG, "byok_power_init: %s (continuing without WAKE power-off handling)",
                 esp_err_to_name(power_err));
    }

    /* Battery/charger monitor (0.1.11, components/byok_battery) -- started
     * here, before the display self-test sequence below (~8-10 s of held
     * phases), so its 2 s monitor task has already produced a real EMA
     * reading by the time the self-test's resting screen draws the "BAT"
     * line (byok_display_selftest.c's draw_battery_line()) and before
     * GET_STATUS/STATUS can be asked for battery_mv/_pct/charging. Logged-
     * and-continue on failure, same convention as every other non-essential
     * bring-up step here: byok_battery's own getters keep returning safe
     * defaults (0 mV / 0% / not-charging) if this fails, rather than this
     * firmware refusing to boot over an unmeasurable battery. */
    esp_err_t battery_err = byok_battery_init();
    if (battery_err != ESP_OK) {
        ESP_LOGE(TAG, "byok_battery_init: %s (continuing without battery/charger telemetry)",
                 esp_err_to_name(battery_err));
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const char *reset_reason_str = byok_reset_reason_str(reset_reason);
    ESP_LOGI(TAG, "================================================================");
    ESP_LOGI(TAG, " BYOK Mod -- ESP32-S3 firmware");
    ESP_LOGI(TAG, "   version : %s", app_desc->version);
    ESP_LOGI(TAG, "   idf     : %s", app_desc->idf_ver);
    ESP_LOGI(TAG, "   built   : %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "   running : %s", running != NULL ? running->label : "?");
    /* Reset reason is the discriminator for every warm-reset question this
     * firmware has open (docs/recovery.md §3) -- see
     * byok_reset_reason_str()'s own comment above for what each value means
     * here. Also drawn on the glass below, in byok_display_run_boot_selftest()
     * -- the only channel that survives a dead USJ. */
    ESP_LOGI(TAG, "   reset   : %d (%s)", (int)reset_reason, reset_reason_str);
    ESP_LOGI(TAG, "   gates   : SD_UPDATER_ARMED=%s ALLOW_OTADATA_WRITE=%s NVS_PERSIST_ARMED=%s",
#if CONFIG_BYOK_SD_UPDATER_ARMED
             "on",
#else
             "off",
#endif
#if CONFIG_BYOK_ALLOW_OTADATA_WRITE
             "on",
#else
             "off",
#endif
#if CONFIG_BYOK_NVS_PERSIST_ARMED
             "on"
#else
             "off"
#endif
             );
    ESP_LOGI(TAG, "================================================================");

    byok_hw_shim_log_warnings();

    /* SD updater check, still early (before display/USB bring-up) so a
     * found-and-installed update reboots without ever drawing anything --
     * see components/byok_sd_updater's own header comment. Not fatal to
     * booting if it fails (mount trouble, corrupt tar, etc.): logged, and
     * boot continues normally either way. */
    esp_err_t sd_updater_err = run_sd_updater_in_dedicated_task(sd_updater_status_cb, NULL);
    if (sd_updater_err != ESP_OK) {
        ESP_LOGW(TAG, "byok_sd_updater_check_and_run: %s (continuing boot)", esp_err_to_name(sd_updater_err));
    }

    compute_device_id(s_device_id);
    init_sd_detect_gpio();
    ESP_ERROR_CHECK(byok_modes_init(BYOK_MODE_DASHBOARD_USB));

    /* Logged-and-continue, not ESP_ERROR_CHECK: with no on-device rollback
     * (docs/bootloader-analysis.md), a panic here is a permanent boot loop
     * with no escape, whereas a device with a dead panel but a live CDC
     * link (or vice versa) is diagnosable over USB. */
    esp_err_t display_err = byok_display_init();
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "byok_display_init: %s (continuing without display)", esp_err_to_name(display_err));
    } else {
        s_display_ready = true; /* only now is it safe for sd_updater_status_cb() (or anything
                                  * else) to call into the display driver -- see its own comment.
                                  * The SD updater has already finished by this point in this
                                  * build's boot order, so this has no effect on it today. */
        byok_power_set_display_ready(true); /* same gate, for byok_power's "POWERING OFF" message */
    }
#if CONFIG_BYOK_ALLOW_OTADATA_WRITE
    const bool boot_original_gate_open = true;
#else
    const bool boot_original_gate_open = false;
#endif
    if (display_err == ESP_OK) {
#if CONFIG_BYOK_DISPLAY_POLARITY_EXPERIMENT
        /* 0.1.7: docs/display.md Sec.6 / components/byok_display/Kconfig
         * BYOK_DISPLAY_POLARITY_EXPERIMENT -- REPLACES the normal boot
         * self-test with the A/B/C command/data polarity experiment while
         * this option is on (default OFF again since 0.1.8 -- the vendor
         * transport disassembly in docs/display.md Sec.6 found this driver
         * already vendor-exact; see that Kconfig entry's own updated help
         * text). See that Kconfig entry's help text for the
         * pass list. */
        (void)boot_original_gate_open; /* only consumed by the normal self-test, below */
        byok_display_run_polarity_experiment(app_desc->version);
#else
        byok_display_run_boot_selftest(app_desc->version, boot_original_gate_open, reset_reason_str);
#endif
        esp_err_t backlight_err = byok_display_set_backlight(200, 0);
        if (backlight_err != ESP_OK) {
            ESP_LOGE(TAG, "byok_display_set_backlight: %s (continuing)", esp_err_to_name(backlight_err));
        }
    }

    /* 0.1.12: PCF8563 RTC (hw_config.h Sec.2, byok_rtc component) + CLOCK
     * mode (byok_modes.h BYOK_MODE_CLOCK, byok_clock component). Logged-and-
     * continue like every other optional peripheral in this boot sequence
     * (byok_display above, byok_battery elsewhere) -- a device that can't
     * read its RTC still boots and works over USB, just without a working
     * on-glass clock; byok_clock's own init/task/callback all already
     * handle a failed byok_rtc_init() or a failing byok_rtc_read() without
     * crashing (see byok_clock.c). Placed after the boot self-test above
     * (not before) so the border+banner resting screen is what's on the
     * glass immediately after boot, exactly as before this release -- CLOCK
     * mode only takes over once CONFIG_BYOK_CLOCK_IDLE_TIMEOUT_MS elapses
     * with no host frame, same as it would for a host that never connects
     * at all (byok_clock.h's own "Refresh cadence"/Kconfig comments). */
    /* 0.1.13: our own NVS namespace ("byokmod", gated behind CONFIG_BYOK_
     * NVS_PERSIST_ARMED, default OFF -- see byok_nvs.h for the full D-003
     * rationale), the STATIC NOTE store, and the back-button idle-submode/
     * backlight cycler -- all logged-and-continue like every other optional
     * bring-up step in this function; a device with persistence off or a
     * button GPIO that failed to configure still boots and works over USB,
     * just without that one feature (byok_nvs_init()/byok_note_init()/
     * byok_idle_init() all degrade gracefully on their own, see each
     * header). byok_idle_init() MUST run before byok_clock_init() below:
     * byok_clock's own idle-timeout path calls byok_idle_enter_selected_
     * submode(), so the selected submode needs to already be loaded (from
     * NVS, or the CLOCK/100% defaults) before that 30 s timeout can first
     * fire -- see byok_idle.h's own ordering note.
     *
     * 0.1.14: byok_menu_init() (preset list + selection, byok_menu.h) MUST
     * run before byok_idle_init() for the same reason -- byok_idle's
     * button task calls byok_menu_is_open()/_move()/_select() from its
     * very first poll, so the menu's own state must already be loaded.
     * The button-event/preset-changed callbacks are wired here too (this
     * file's own on_button_event()/on_preset_changed(), defined above) --
     * before byok_idle_init() starts the poll task that could otherwise
     * fire one before a callback is registered (harmless either way, since
     * both callbacks are simple NULL-checked function-pointer calls, but
     * wiring them first is the more obviously-correct order). */
    byok_nvs_init();
    byok_note_init();
    byok_menu_init();
    byok_menu_set_preset_changed_cb(on_preset_changed);
    byok_idle_set_button_event_cb(on_button_event);
    byok_idle_init();

    /* 0.1.14: "shown at boot for 5 s before the idle/clock screen (skipped
     * if no presets stored)" -- docs/protocol.md Sec.6.4b. Placed here,
     * right after byok_idle_init() (so UP/DOWN/EXECUTE already work against
     * it) and before byok_rtc_init()/byok_clock_init() below, matching the
     * boot self-test's own "resting screen is up, THEN the next thing takes
     * over" shape. Entirely non-blocking -- byok_menu_open_boot() renders
     * once and returns immediately; its own 5 s auto-close timer runs on
     * byok_menu's own background task (byok_menu_init(), above), not here,
     * so USB/dispatch bring-up below is not delayed by it. A no-op if no
     * preset was ever persisted (CONFIG_BYOK_NVS_PERSIST_ARMED off, or on
     * but SET_PRESETS has never been sent) -- see byok_menu_has_presets(). */
    byok_menu_open_boot();

    esp_err_t rtc_err = byok_rtc_init();
    if (rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "byok_rtc_init: %s (CLOCK mode will show an error screen instead of the time)",
                 esp_err_to_name(rtc_err));
    }
    byok_clock_init();

    /* 0.1.14: docs/protocol.md Sec.6.4b GET_DOCSTATS, components/
     * byok_docstats. Placed after byok_rtc_init() (so a first boot scan,
     * moments later on its own task, can already derive words_today if the
     * RTC came up) -- logged-and-continue like every other optional
     * bring-up step in this function; a failed heap_caps_malloc() or
     * xTaskCreate() here just means GET_DOCSTATS keeps answering an
     * all-zero, never-ready snapshot, not a boot failure. */
    byok_docstats_init();

    const esp_timer_create_args_t frame_timer_args = {
        .callback = frame_timeout_cb,
        .name = "byok_frame_to",
    };
    ESP_ERROR_CHECK(esp_timer_create(&frame_timer_args, &s_frame_timer));

    byok_parser_init(&s_rx_parser, byok_on_frame, byok_on_error, NULL);

    s_rx_stream = xStreamBufferCreate(8192, 1);
    if (s_rx_stream == NULL) {
        ESP_LOGE(TAG, "xStreamBufferCreate failed -- CDC RX will be dropped");
    }

    /* USJ/esptool recovery window: with no on-device rollback, esptool over
     * USB-Serial-JTAG is the ONLY recovery from a crash-looping image
     * (docs/bootloader-analysis.md Sec.8.3), so the point at which TinyUSB
     * claims the USB-OTG PHY must be an explicit, measured minimum uptime --
     * not an undocumented side effect of how long the boot self-test happens
     * to hold each frame. This spin is independent of CONFIG_BYOK_DISPLAY_*
     * and of whether byok_display_init() above even succeeded. */
    int64_t usb_gate_until_us = (int64_t)CONFIG_BYOK_USB_CDC_MIN_UPTIME_MS * 1000;
    while (esp_timer_get_time() < usb_gate_until_us) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "USJ/esptool window closes now: claiming USB-OTG PHY at t=%" PRId64 " ms",
             esp_timer_get_time() / 1000);

    /* Logged-and-continue, not ESP_ERROR_CHECK: TinyUSB may already have
     * taken the PHY off the USB-Serial-JTAG controller by the time this can
     * fail, so aborting here would leave that hand-over in an unreasoned-
     * about state on the next boot. A booted device with no CDC link is
     * still diagnosable (display, and a future re-plug); a panic loop is
     * not. */
    esp_err_t usb_cdc_err = byok_usb_cdc_init(cdc_rx_bridge, NULL);
    if (usb_cdc_err != ESP_OK) {
        ESP_LOGE(TAG, "byok_usb_cdc_init: %s (continuing without CDC link)", esp_err_to_name(usb_cdc_err));
    }

    /* Stack audit, 2026-09-03 (docs/troubleshooting.md
     * prompted a pass over every task this file creates, not just the
     * updater's). Largest locals actually on this task's own frames:
     * dispatch_task()'s chunk[256] plus, one call deep in dispatch(), the
     * largest single typed-reply payload (send_info()'s p[64]) -- these are
     * never live simultaneously with each other beyond a couple of stack
     * frames, so well under 512 B total. Deepest call chain: dispatch_task
     * -> dispatch() -> a BYOK_TYPE_* handler -> byok_display_* (no large
     * locals of its own, see byok_display.c: s_fb is `static`, not a stack
     * array) or byok_usb_cdc_send() -> tinyusb_cdcacm_write_queue/_flush
     * (esp_tinyusb's own call frames, not sized here). No FreeRTOS-heap
     * (task-stack) allocation of libc I2C/USB driver internals runs deeper
     * than a handful of small frames past this task's own locals. 6144 B
     * leaves several KiB of margin over that estimate -- not marginal,
     * left unchanged. (Margin-by-inspection only, like every other number
     * in this comment block -- no uxTaskGetStackHighWaterMark() data exists
     * for this task yet; unlike the updater task above, nothing here logs
     * it automatically, since this task runs for the device's entire
     * uptime rather than once at boot.) */
    BaseType_t created = xTaskCreate(dispatch_task, "byok_dispatch", 6144, NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(byok_dispatch) failed");
    }

    /* esp_ota_mark_app_valid_cancel_rollback() is NOT compiled out by
     * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n: it is unconditional IDF code
     * that, on this bootloader (docs/bootloader-analysis.md Sec.3, Sec.8.2),
     * performs a real 4 KiB erase + 32-byte write on `otadata` (0x910000) --
     * a write outside our own OTA slot -- for zero behavioural benefit,
     * because the stock bootloader treats ota_state UNDEFINED and VALID
     * identically. It must therefore stay behind the same
     * CONFIG_BYOK_ALLOW_OTADATA_WRITE gate as every other otadata write in
     * this file (BOOT_ORIGINAL, the EXECUTE-hold path) -- see
     * main/Kconfig.projbuild and SAFETY.md §1. There is no automatic
     * revert on this device either way; the real net for a valid-but-
     * crashing image is host-esptool-over-USJ (docs/bootloader-analysis.md
     * Sec.8.3, Sec.6 scenario 1). */
#if CONFIG_BYOK_ALLOW_OTADATA_WRITE
    esp_err_t rollback_err = esp_ota_mark_app_valid_cancel_rollback();
    if (rollback_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(rollback_err));
    }
#else
    ESP_LOGD(TAG, "esp_ota_mark_app_valid_cancel_rollback skipped -- "
                  "CONFIG_BYOK_ALLOW_OTADATA_WRITE is OFF (default)");
#endif

    ESP_LOGI(TAG, "ready.");
}
