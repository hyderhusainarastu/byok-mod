/* SPDX-License-Identifier: MIT */
#include "byok_usb_cdc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/sha256.h"

#include "esp_private/periph_ctrl.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "soc/rtc_cntl_reg.h"

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

#include "byok_hw_shim.h" /* BYOK_TUSB320_* -- hw_config.h Sec.2 */
#include "byok_sysbus.h"  /* shared I2C BUS 1 handle (TUSB320 + PCF8563) */

static const char *TAG = "byok_usb_cdc";

static byok_usb_cdc_rx_cb_t s_rx_cb;
static void *s_rx_ctx;
static volatile bool s_dtr;
static bool s_ready;

/* String descriptor array handed to tinyusb_driver_install(). Must live for
 * the lifetime of the USB device -- static storage, not stack. Layout
 * mirrors esp_tinyusb's own descriptor_str_default[] (usb_descriptors.c):
 * index 0 is the raw 2-byte LANGID (English, 0x0409, little-endian bytes as
 * TinyUSB's descriptor callback expects, NOT a display string), 1
 * Manufacturer, 2 Product, 3 Serial, 4 the CDC interface name. */
static const char s_lang_id[2] = { 0x09, 0x04 };
static char s_serial[16]; /* "BYOKMOD" + 4 hex chars + NUL (12 chars used, margin kept),
                            * filled in byok_usb_cdc_init() -- see byok_usb_cdc.h's
                            * "Identity" comment for why this is a hash prefix, not the MAC. */
static const char *s_strings[5];

/* ==========================================================================
 * USB-Serial-JTAG PHY restore -- see byok_usb_cdc.h for the full CONFIRMED
 * reasoning. Register-level summary, with the IDF v5.5.3 sources each line
 * comes from:
 *
 *   usb_wrap_ll_reset_register()                 SYSTEM.perip_rst_en0.usb_rst pulse --
 *                                                clears ALL of USB_WRAP.otg_conf
 *                                                (pad_enable, pad_pull_override, dp_pullup,
 *                                                dm_pullup, test_conf.test_enable), i.e. drops
 *                                                anything the OTG side was holding on D+/D-.
 *                                                hal/esp32s3/include/hal/usb_wrap_ll.h RCC section
 *   usb_serial_jtag_ll_phy_enable_external(false)
 *                                                USB_SERIAL_JTAG.conf0.phy_sel      = 0
 *                                                RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1
 *                                                RTCCNTL.usb_conf.sw_usb_phy_sel    = 0
 *                                                => internal FSLS PHY -> USJ.
 *                                                usb_serial_jtag_ll.h:206-217
 *   usb_serial_jtag_ll_phy_enable_pad(true)      USB_SERIAL_JTAG.conf0.usb_pad_enable = 1
 *                                                usb_serial_jtag_ll.h:303-306
 *   usb_serial_jtag_ll_phy_disable_pull_override()
 *                                                USB_SERIAL_JTAG.conf0.pad_pull_override = 0
 *                                                usb_serial_jtag_ll.h:273-276
 *
 * NOTE the asymmetry that makes this easy to get wrong: usb_wrap_ll.h's own
 * usb_wrap_ll_phy_enable_external(hw, enable) writes sw_usb_phy_sel = !enable,
 * so calling it with `false` would hand the PHY TO the OTG controller -- the
 * exact opposite of what is wanted here. Use the USJ LL call instead, which
 * writes sw_usb_phy_sel = enable directly (usb_serial_jtag_ll.h:206-217) --
 * confirmed against ~/esp/esp-idf v5.5.3 before writing this.
 *
 * Ordering is load-bearing: reset USB_WRAP first (OTG side lets go of the
 * bus), only then move the mux and enable the USJ pads. Factored into its
 * own function (docs/recovery.md Sec.4.2's "known gap" note:
 * the shutdown-handler path needs the unconditional tail below, not the
 * RTC-read-and-maybe-skip wrapper). */
static void usj_phy_takeover(void)
{
    PERIPH_RCC_ATOMIC() {
        usb_wrap_ll_enable_bus_clock(true);
        usb_wrap_ll_reset_register();
        /* Leave the USB_WRAP clock off again: usb_wrap_hal_init(), called
         * from usb_new_phy() if/when byok_usb_cdc_init() runs later this
         * boot, re-enables and re-resets it itself. */
        usb_wrap_ll_enable_bus_clock(false);
    }

    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
    usb_serial_jtag_ll_phy_disable_pull_override();
}

void byok_usb_cdc_restore_usj_phy(void)
{
    /* Read the RTC-domain mux before touching anything. On a clean cold boot
     * both bits are 0 and there is nothing to repair -- returning here is
     * what makes this safe to call unconditionally from app_main() without
     * ever disturbing a live USJ console or an in-progress esptool session
     * (a USJ peripheral reset would drop that connection). */
    const uint32_t conf   = REG_READ(RTC_CNTL_USB_CONF_REG);
    const bool sw_ctrl = (conf & RTC_CNTL_SW_HW_USB_PHY_SEL) != 0;
    const bool on_otg  = (conf & RTC_CNTL_SW_USB_PHY_SEL) != 0;
    if (!sw_ctrl || !on_otg) {
        return; /* PHY is already the USJ's (or under hardware control) */
    }

    ESP_LOGW(TAG, "internal USB PHY was left muxed to USB-OTG by a previous run "
                  "(RTC_CNTL_USB_CONF_REG=0x%08" PRIx32 ") -- restoring it to USB-Serial-JTAG",
             conf);

    usj_phy_takeover();
}

/* Registered with esp_register_shutdown_handler() from byok_usb_cdc_init(),
 * so every esp_restart() in this firmware (byok_sd_updater's post-install
 * reboot, both BOOT_ORIGINAL paths) hands the PHY back before the CPU reset
 * -- which both preserves the USJ/esptool net across the reboot AND gives
 * the USB host a clean CDC detach before the device disappears, instead of
 * a same-port device swap mid-teardown. Unconditional here (not gated on the
 * RTC read) because by definition we did claim the PHY if this is
 * registered. Runs with interrupts and the scheduler still up (it is
 * invoked from esp_restart() before esp_restart_noos()), so this is safe to
 * call directly. */
static void restore_usj_phy_on_shutdown(void)
{
    usj_phy_takeover();
}

/* Stack audit, 2026-09-03 (docs/troubleshooting.md
 * prompted a pass over every task/callback that could carry a large stack
 * local, not just the SD updater's). buf[256] below is on the CALLING
 * task's own stack -- and that task is TinyUSB's own "tusb" task
 * (managed_components/espressif__esp_tinyusb/tinyusb_task.c,
 * xTaskCreatePinnedToCore, default TINYUSB_DEFAULT_TASK_SIZE=4096 B, see
 * include/tinyusb_default_config.h), NOT a task this project creates --
 * this file only registers cdc_rx_handler as a callback TinyUSB invokes
 * from that task. Out of scope to resize here even if it were marginal
 * (a managed_component, re-fetched by the component manager, not owned
 * source), and 256 B out of a 4096 B budget -- Espressif's own tuned
 * default for a task that also has to run tud_task()'s full USB stack
 * underneath it -- is not marginal: comfortable headroom, no change
 * needed or possible from this file. */
static void cdc_rx_handler(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[256];
    for (;;) {
        size_t rx_size = 0;
        esp_err_t err = tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof(buf), &rx_size);
        if (err != ESP_OK || rx_size == 0) {
            break;
        }
        if (s_rx_cb != NULL) {
            s_rx_cb(buf, rx_size, s_rx_ctx);
        }
        if (rx_size < sizeof(buf)) {
            break; /* short read -- FIFO drained for this event */
        }
    }
}

static void cdc_line_state_handler(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_dtr = event->line_state_changed_data.dtr;
}

/* ==========================================================================
 * USB re-attach watcher -- 2026-09-03
 * ==========================================================================
 * Device observation: with the device running on battery, unplugging and
 * re-plugging the USB cable leaves the Mac seeing NO device at all -- no
 * CDC re-enumeration, nothing in `ioreg`/`system_profiler`. TinyUSB on the
 * S3's native USB-OTG PHY does not, on its own, notice a VBUS cycle on a
 * self-powered device (this firmware never wired VBUS_SENSE -- hw_config.h
 * Sec.13, BYOK_USB_VBUS_SENSE_GPIO == -1 -- and the S3's OTG controller has
 * no other way to learn "the cable came back" by itself): the peripheral is
 * still sitting there presented as attached from the FIRST enumeration,
 * long after the host has physically gone away and come back, so the host
 * never sees a fresh set-configuration handshake.
 *
 * The board DOES have an independent, external witness of the physical USB
 * connection: the TUSB320 CC-chip on I2C BUS 1 (hw_config.h Sec.2 -- the
 * SAME chip stock's own "On USB power, can't power off" check reads, see
 * byok_power.h's own note on why this firmware does not implement that
 * refusal). Its status register (BYOK_TUSB320_REG_STATUS, 0x09) bits [7:6]
 * are ATTACHED_STATE: 0 = not attached, 1 = attached.SRC, 2 = attached.SNK
 * (this device is a sink -- a Mac upstream is exactly this value), 3 =
 * accessory. Polling it (250 ms, cheap and slow enough to be negligible
 * next to the CDC RX/TX path) gives this firmware a VBUS-cycle signal
 * TinyUSB itself does not have, and byok_sysbus.h/byok_sysbus_init() is
 * what lets this share BUS 1 safely with a future PCF8563 user instead of
 * each grabbing the port for itself.
 *
 * On a transition INTO attached.SNK (from any other state, not just from
 * "not attached" -- a bounce through attached.SRC/accessory on the way in
 * counts too), this forces a fresh enumeration with tud_disconnect() / a
 * settle delay / tud_connect() -- soft pull-up cycling on the native PHY,
 * which IS something TinyUSB's own dcd_esp32sx port implements (unlike
 * noticing the re-attach in the first place). On a transition to "not
 * attached", tud_disconnect() alone gives the host a clean, immediate
 * detach rather than leaving a stale device sitting in its device tree
 * until some other timeout notices. Every transition is logged (INFO) so a
 * serial capture shows exactly when and why a re-enumeration was forced.
 *
 * This task starts only after tinyusb_cdcacm_init() has already returned
 * ESP_OK in byok_usb_cdc_init() below ("AFTER TinyUSB is installed" --
 * tud_disconnect()/tud_connect() are meaningless, and the TinyUSB task they
 * talk to does not exist yet, before that). A BUS 1 or TUSB320 problem
 * (sysbus init failure, the device never ACKing) disables ONLY this watcher
 * -- logged and continued, matching this driver's usual policy elsewhere
 * (see byok_usb_cdc_init()'s own comments): the CDC link this firmware
 * exists to provide still comes up and still works for a host that is
 * already attached at boot, it just won't self-heal a later unplug/replug.
 */
#define TUSB320_REATTACH_POLL_MS      250u
#define TUSB320_REATTACH_SETTLE_MS    150u
#define TUSB320_ATTACH_STATE_MASK     0xC0u /* bits [7:6] */
#define TUSB320_ATTACH_STATE_SHIFT    6u
#define TUSB320_ATTACH_NOT_ATTACHED   0u
#define TUSB320_ATTACH_SRC            1u
#define TUSB320_ATTACH_SNK            2u
#define TUSB320_ATTACH_ACCESSORY      3u

static i2c_master_dev_handle_t s_tusb320_dev;

static esp_err_t tusb320_read_attach_state(uint8_t *out_state)
{
    uint8_t reg = BYOK_TUSB320_REG_STATUS;
    uint8_t val = 0;
    esp_err_t err = i2c_master_transmit_receive(s_tusb320_dev, &reg, 1, &val, 1, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        return err;
    }
    *out_state = (uint8_t)((val & TUSB320_ATTACH_STATE_MASK) >> TUSB320_ATTACH_STATE_SHIFT);
    return ESP_OK;
}

/* 2026-09-03: see byok_usb_cdc.h's own doc comment above the declaration.
 * Deliberately does a fresh i2c read via tusb320_read_attach_state() rather
 * than reusing tusb320_reattach_task()'s prev_state -- that state is
 * private to that task and can be stale by up to TUSB320_REATTACH_POLL_MS. */
esp_err_t byok_usb_cdc_usb_attached(bool *out_attached)
{
    if (out_attached == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tusb320_dev == NULL) {
        /* Watcher never started (BUS 1 init or device-add failed, or
         * byok_usb_cdc_init() was never called). No VBUS source to read:
         * say so rather than reporting a fabricated "not attached". */
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t state = 0;
    esp_err_t err = tusb320_read_attach_state(&state);
    if (err != ESP_OK) {
        return err;
    }
    *out_attached = (state == TUSB320_ATTACH_SNK);
    return ESP_OK;
}

/* Stack audit, 2026-09-03. The stack-overflow fault in
 * docs/troubleshooting.md Sec.3 is why every task this project creates
 * gets one of these, this one included. Only locals are a handful of
 * uint8_t/esp_err_t values; the deepest
 * call chain is this function -> tusb320_read_attach_state() ->
 * i2c_master_transmit_receive() (new-style I2C master driver, the same
 * depth byok_display.c's own I2C calls already run at from tasks sized
 * 4096-6144 B elsewhere in this tree) or -> tud_disconnect()/tud_connect()
 * (TinyUSB's own dcd_esp32sx calls, shallow -- register pokes). 3072 B
 * gives comfortable margin over that without matching byok_power_btn's
 * 4096 B, which additionally has to reach through byok_display's own call
 * frames (this task never touches the display). */
#define TUSB320_REATTACH_TASK_STACK_BYTES 3072

static void tusb320_reattach_task(void *arg)
{
    (void)arg;

    uint8_t state = 0;
    esp_err_t err = tusb320_read_attach_state(&state);
    /* -1 (not a valid 2-bit ATTACHED_STATE value): forces the very first
     * successful read to be treated as a baseline, not a "transition" that
     * fires tud_disconnect()/tud_connect() on a device that may already be
     * happily enumerated at boot. */
    int prev_state = (err == ESP_OK) ? (int)state : -1;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tusb320_reattach: initial status read failed: %s -- watcher continuing, will retry",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "tusb320_reattach: watching, baseline ATTACHED_STATE=%d", prev_state);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TUSB320_REATTACH_POLL_MS));

        err = tusb320_read_attach_state(&state);
        if (err != ESP_OK) {
            /* Transient I2C error -- try again next poll. Deliberately does
             * NOT touch prev_state or force a reconnect on a read failure:
             * churning tud_disconnect()/tud_connect() because BUS 1 hiccuped
             * would be worse than just missing one poll. */
            continue;
        }

        if ((int)state == prev_state) {
            continue;
        }

        ESP_LOGI(TAG, "tusb320_reattach: ATTACHED_STATE %d -> %u", prev_state, (unsigned)state);

        if (state == TUSB320_ATTACH_SNK && prev_state != TUSB320_ATTACH_SNK) {
            ESP_LOGI(TAG, "tusb320_reattach: attached as sink -- forcing a fresh CDC enumeration");
            tud_disconnect();
            vTaskDelay(pdMS_TO_TICKS(TUSB320_REATTACH_SETTLE_MS));
            tud_connect();
        } else if (state == TUSB320_ATTACH_NOT_ATTACHED) {
            ESP_LOGI(TAG, "tusb320_reattach: not attached -- signalling a clean detach to the host");
            tud_disconnect();
        }

        prev_state = (int)state;
    }
}

/* Starts the watcher above. Logged-and-continue on any failure (BUS 1 init,
 * adding the TUSB320 device, or task creation) -- see this file's own
 * module comment for why that's the right failure mode here. Call only
 * after tinyusb_cdcacm_init() has returned ESP_OK. */
static void start_tusb320_reattach_watcher(void)
{
    esp_err_t err = byok_sysbus_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_sysbus_init failed: %s -- USB re-attach watcher disabled "
                      "(a later unplug/replug will not self-heal; a host already attached "
                      "at boot is unaffected)",
                 esp_err_to_name(err));
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BYOK_TUSB320_I2C_ADDR,
        .scl_speed_hz = BYOK_TUSB320_I2C_SPEED_HZ,
    };
    err = i2c_master_bus_add_device(byok_sysbus_get_handle(), &dev_cfg, &s_tusb320_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_bus_add_device(TUSB320) failed: %s -- USB re-attach watcher disabled",
                 esp_err_to_name(err));
        return;
    }

    BaseType_t created = xTaskCreate(tusb320_reattach_task, "byok_tusb320", TUSB320_REATTACH_TASK_STACK_BYTES,
                                      NULL, 3, NULL);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "xTaskCreate(byok_tusb320) failed -- USB re-attach watcher disabled");
    }
}

esp_err_t byok_usb_cdc_init(byok_usb_cdc_rx_cb_t rx_cb, void *ctx)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_rx_cb = rx_cb;
    s_rx_ctx = ctx;

    /* Non-identifying, non-reversible serial: "BYOKMOD" + 2 bytes (4 hex
     * chars) taken from SHA-256(MAC) at offset [8..9] -- deliberately
     * outside compute_device_id()'s [0..7] range (app_main.c) so this
     * serial and HELLO_ACK.device_id are genuinely non-overlapping byte
     * ranges of the same hash, not merely different lengths of the same
     * prefix. The MAC itself is read only to feed the hash and is never
     * copied into `s_serial` or logged anywhere in this function -- see
     * byok_usb_cdc.h's "Identity" comment for why (0.1.0 used the raw MAC
     * here and it leaked into every /dev/cu.usbmodem<serial> node name/log
     * line; 0.1.1 fixes that). */
    uint8_t mac[6] = { 0 };
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed (%s); USB serial string will hash an all-zero MAC", esp_err_to_name(err));
    }
    unsigned char hash[32];
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0 /* SHA-256, not SHA-224 */);
    mbedtls_sha256_update(&sha_ctx, mac, sizeof(mac));
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);
    snprintf(s_serial, sizeof(s_serial), "BYOKMOD%02X%02X", hash[8], hash[9]);

    s_strings[0] = s_lang_id;
    s_strings[1] = "BYOK Lab";
    s_strings[2] = "BYOK Mod Display";
    s_strings[3] = s_serial;
    s_strings[4] = "BYOK Link";

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    /* device + full_speed_config stay NULL (from the macro): esp_tinyusb
     * builds its Kconfig-driven default device descriptor (VID/PID from
     * CONFIG_TINYUSB_DESC_*, see sdkconfig.defaults) and its default
     * single-CDC configuration descriptor (CONFIG_TINYUSB_CDC_ENABLED=y).
     * Only the strings are ours, for the MAC-derived serial. */
    tusb_cfg.descriptor.string = s_strings;
    tusb_cfg.descriptor.string_count = (int)(sizeof(s_strings) / sizeof(s_strings[0]));

    /* This device is battery-powered, not bus-powered -- TINYUSB_DEFAULT_
     * CONFIG()'s own default (phy.self_powered = false) describes a device
     * that only ever runs while USB is supplying it, which is not this
     * board (hw_config.h Sec.9: it runs on battery with GPIO42 low, USB or
     * not). USB_PHY_SELF_POWERED_DEVICE(vbus_monitor_io), which this flag
     * feeds into usb_new_phy() (see esp_tinyusb's tinyusb.c), is what makes
     * a self-powered device's descriptor honest; vbus_monitor_io stays -1
     * because no GPIO on this board is wired to sense VBUS (Sec.13,
     * BYOK_USB_VBUS_SENSE_GPIO == -1) -- there is no pin to give it. On the
     * ESP32-S3 specifically this does not, by itself, fix the re-attach
     * problem the watcher below exists for (the P4/S31-only VBUS-GPIO-
     * monitor code path in esp_tinyusb is compiled out on this target, per
     * tinyusb.c's own #if) -- it is a separate, independently-correct
     * descriptor fix, not a substitute for the watcher. */
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = -1;

    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* From this point on the internal PHY belongs to USB-OTG and the
     * RTC-domain mux bits are set -- and they survive esp_restart(), which on
     * this chip is only a CPU-level reset (rst:0xc). Register the hand-back
     * NOW, immediately after the install that created the problem, so there
     * is no window in which a reboot can strand the USJ. Logged-and-continue
     * on failure (the boot-time byok_usb_cdc_restore_usj_phy() call in
     * app_main() is the backstop that repairs it on the following boot). */
    esp_err_t sh_err = esp_register_shutdown_handler(restore_usj_phy_on_shutdown);
    if (sh_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_register_shutdown_handler: %s -- USJ will not be restored before a "
                      "software reset; the next boot's restore_usj_phy() call will repair it",
                 esp_err_to_name(sh_err));
    }

    tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_handler,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_handler,
        .callback_line_coding_changed = NULL,
    };
    err = tinyusb_cdcacm_init(&cdc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_cdcacm_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Safe to log in full: s_serial is a non-reversible hash prefix now
     * (see the identity comment above and byok_usb_cdc.h), not the MAC --
     * SAFETY.md's "never print the full MAC" rule is about the MAC itself,
     * which never appears in this string. */
    ESP_LOGI(TAG, "CDC-ACM up, serial %s", s_serial);
    s_ready = true;

    /* Only now -- TinyUSB (driver + this one CDC interface) is fully
     * installed, so tud_disconnect()/tud_connect() are meaningful and the
     * TinyUSB task they talk to actually exists. See the watcher's own
     * module comment above for what this fixes and why it needs BUS 1. */
    start_tusb320_reattach_watcher();

    return ESP_OK;
}

esp_err_t byok_usb_cdc_send(const uint8_t *data, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_OK;
    }

    size_t sent = 0;
    int stalls = 0;
    while (sent < len) {
        size_t n = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data + sent, len - sent);
        sent += n;
        if (n == 0) {
            if (++stalls > 50) { /* ~100 ms of no progress -- give up */
                ESP_LOGW(TAG, "TX queue stalled, dropping %u of %u bytes",
                         (unsigned)(len - sent), (unsigned)len);
                return ESP_FAIL;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
}

bool byok_usb_cdc_dtr(void)
{
    return s_dtr;
}

esp_err_t byok_usb_cdc_deinit(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* tinyusb_driver_uninstall() -> usb_del_phy() clears the USB_WRAP pull
     * override and frees the handle, but leaves RTC_CNTL_USB_CONF_REG's mux
     * pointing at the OTG controller (components/usb/usb_phy.c:439-459).
     * The restore below is what actually gives the console its PHY back. */
    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_uninstall: %s (restoring USJ anyway)", esp_err_to_name(err));
    }
    s_ready = false;
    s_dtr = false;
    byok_usb_cdc_restore_usj_phy();
    return err;
}
