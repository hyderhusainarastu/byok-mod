/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_usb_cdc.h — TinyUSB CDC-ACM transport for the BYOK Link protocol
 * ============================================================================
 *
 * Brings up ONE USB CDC-ACM interface on the S3's native USB PHY via the
 * managed component `espressif/esp_tinyusb` (see main/idf_component.yml).
 * This is the sole USB personality this firmware presents in normal
 * operation — never the vendor's Disk-Mode MSC descriptor, never a second
 * CDC/vendor interface, and never anything sharing the USB-Serial-JTAG
 * peripheral (that stays the debug console, per hw_config.h Sec.11 and
 * sdkconfig.defaults' CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y — a
 * completely separate PHY-adjacent-but-distinct block from the OTG PHY
 * TinyUSB drives here; see firmware/s3/README.md "Two USB personalities").
 *
 * Identity (docs/protocol.md Sec.2, matched by host/macos/byok/transport.py's
 * PRODUCT_STRING_MATCH so the host never opens a stock-firmware port):
 *   iManufacturer   "BYOK Lab"
 *   iProduct        "BYOK Mod Display"
 *   iSerialNumber   "BYOKMOD" + 4 hex chars, computed at init -- see
 *                   byok_usb_cdc.c. Prior to 0.1.1 this was 12 hex chars of
 *                   the raw station MAC (ESP_MAC_WIFI_STA); that leaked the
 *                   MAC into every macOS /dev/cu.usbmodem<serial> node name
 *                   (and hence into any log or capture that mentions the
 *                   device path -- see CHANGELOG.md's 0.1.0
 *                   first-boot entry and scripts/serial-listen.sh's
 *                   redaction pass). The 4 hex chars here are the first two
 *                   bytes of SHA-256(MAC) -- deterministic per device (so
 *                   the same unit always enumerates with the same serial,
 *                   and two units plugged in together still get visibly
 *                   different ones) but NOT reversible back to the MAC.
 *                   Taken from hash offset [8..9], deliberately outside the
 *                   [0..7] range HELLO_ACK.device_id truncates
 *                   SHA-256(MAC) to (docs/protocol.md Sec.6.1) -- a
 *                   genuinely non-overlapping byte range of the same
 *                   non-reversible hash, not just a different-length
 *                   prefix of it, and never the raw MAC itself, on either
 *                   wire.
 *   idVendor        0x303A (Espressif; CONFIG_TINYUSB_DESC_USE_ESPRESSIF_VID)
 *   idProduct       CONFIG_TINYUSB_DESC_CUSTOM_PID -- see
 *                   firmware/s3/README.md "USB PID choice" for why and how
 *                   this value was picked; it deliberately does not collide
 *                   with 0x1001 (USB-Serial-JTAG) or 0x4002 (stock Disk Mode).
 *
 * USB re-attach (2026-09-03): byok_usb_cdc_init() also starts a small task
 * that polls the TUSB320 CC-chip (I2C BUS 1, hw_config.h Sec.2, shared via
 * byok_sysbus.h) for VBUS attach/detach and forces a fresh CDC enumeration
 * (tud_disconnect()/tud_connect()) on an attach transition, and a clean
 * tud_disconnect() on a detach -- TinyUSB's own S3 dcd port does not notice
 * a VBUS cycle on a self-powered device by itself. See byok_usb_cdc.c's own
 * module comment above tusb320_reattach_task() for the full reasoning.
 *
 * Only the string descriptors are built at runtime (for the hashed serial);
 * the device and configuration descriptors are esp_tinyusb's own
 * Kconfig-driven CDC-only defaults (sdkconfig.defaults sets the VID/PID/
 * manufacturer/product Kconfig symbols; the manufacturer/product Kconfig
 * strings are superseded by the runtime string array below, which is the
 * single source of truth for what actually enumerates). host/macos/byok/
 * transport.py's device discovery matches on iProduct ("BYOK Mod Display"),
 * never on iSerialNumber, so this change does not affect host discovery.
 */
#ifndef BYOK_USB_CDC_H
#define BYOK_USB_CDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Invoked from the TinyUSB task's CDC RX path with newly-arrived bytes.
 * `data` is only valid for the duration of the call -- copy out anything you
 * need to keep (byok_proto's parser does this itself: it copies into its own
 * internal buffer via byok_parser_feed()). Called from the TinyUSB task
 * context, not a fresh task of its own -- keep it fast and non-blocking. */
typedef void (*byok_usb_cdc_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

/** Installs the TinyUSB driver and one CDC-ACM interface, and starts the
 * TinyUSB task (per esp_tinyusb, priority 5, one 4 KiB stack, per
 * TINYUSB_DEFAULT_CONFIG()'s own defaults). Call once from app_main, after
 * NVS init (esp_tinyusb reads nothing from NVS itself, but the overall boot
 * sequence in app_main.c does NVS first per this firmware's own ordering).
 * `rx_cb`/`ctx` may be NULL to install the transport without a receiver
 * (nothing will happen with the bytes; not useful in practice, but not an
 * error either). */
esp_err_t byok_usb_cdc_init(byok_usb_cdc_rx_cb_t rx_cb, void *ctx);

/** Queues `len` bytes for transmission and flushes them out the CDC IN
 * endpoint, blocking up to ~100 ms for the flush. Safe to call with a whole
 * encoded byok_proto frame (BYOK_MAX_FRAME = 4109 B) in one call --
 * CONFIG_TINYUSB_CDC_TX_BUFSIZE in sdkconfig.defaults is sized to hold one
 * whole frame so this never partially queues a well-formed reply. Returns
 * ESP_ERR_INVALID_STATE if the CDC interface is not yet initialized. */
esp_err_t byok_usb_cdc_send(const uint8_t *data, size_t len);

/** Current DTR (Data Terminal Ready) line state, last reported by
 * CDC_EVENT_LINE_STATE_CHANGED. Not authoritative for "is a host actually
 * running our protocol" (docs/protocol.md never requires DTR handling and
 * explicitly says the host "must not assert DTR-toggle reset behaviour") --
 * useful only as a cheap hint for STATUS.flags bit0 (host-connected). */
bool byok_usb_cdc_dtr(void);

/** Hands the ESP32-S3's internal full-speed USB PHY back to the
 * USB-Serial-JTAG controller, if (and only if) it is currently muxed to the
 * USB-OTG controller instead.
 *
 * WHY THIS EXISTS (docs/recovery.md Sec.1-2, all CONFIRMED from
 * ESP-IDF v5.5.3 source):
 *
 *   1. tinyusb_driver_install() -> usb_new_phy(USB_PHY_CTRL_OTG,
 *      USB_PHY_TARGET_INT) sets RTC_CNTL_USB_CONF_REG bit 20
 *      (SW_HW_USB_PHY_SEL) and bit 19 (SW_USB_PHY_SEL) to 1, i.e. "internal
 *      FSLS PHY -> USB Wrap; USJ -> external PHY". That is what closes the
 *      USJ/esptool window at t=CONFIG_BYOK_USB_CDC_MIN_UPTIME_MS.
 *   2. Those two bits live in the RTC domain. esp_restart() on this chip is
 *      a CPU-level reset (observed on-device as reset reason rst:0xc
 *      RTC_SW_CPU_RST -- see docs/recovery.md Sec.6), which resets
 *      neither the RTC sub-system nor the USB peripherals:
 *      esp_system_reset_modules_on_exit() (esp_system/port/soc/esp32s3/
 *      system_internal.c) deliberately omits SYSTEM_USB_RST and
 *      SYSTEM_USB_DEVICE_RST.
 *   3. usb_del_phy()/tinyusb_driver_uninstall() do NOT restore the mux --
 *      usb_del_phy() only calls usb_wrap_hal_phy_disable_pull_override().
 *
 * Net effect without this function: after ANY esp_restart() that happens
 * once TinyUSB has run (the SD updater's own install reboot, either
 * BOOT_ORIGINAL path), the ROM and bootloader come up with the USJ muxed to
 * a PHY that does not exist on this board, and NO USB-Serial-JTAG device
 * ever enumerates again until a power cycle -- destroying the
 * esptool-over-USJ recovery net that docs/bootloader-analysis.md Sec.8.3
 * identifies as the ONLY recovery from a crash-looping image on this
 * bootloader.
 *
 * Safe and cheap to call unconditionally: it reads the two RTC mux bits
 * first and returns immediately if the PHY is already on the USJ (the
 * normal cold-boot case), so it never disturbs a working USJ console or an
 * in-progress esptool session. Touches no GPIO. */
void byok_usb_cdc_restore_usj_phy(void);

/** Tears the CDC-ACM link down and hands the PHY back to the USJ.
 *
 * tinyusb_driver_uninstall() on its own is NOT enough (see above): it stops
 * the TinyUSB task and deletes the PHY handle, but leaves the RTC-domain
 * mux pointing at the USB-OTG controller, so the console/esptool path stays
 * dead. This wraps the two together in the only order that actually works.
 *
 * No caller today -- byok_usb_cdc_init() is one-way in this build. Exists so
 * that a future "drop back to console mode" command cannot get the ordering
 * wrong. Returns whatever tinyusb_driver_uninstall() returned; the USJ
 * restore runs either way. */
esp_err_t byok_usb_cdc_deinit(void);

/** Is the board attached to a USB host right now?
 *
 * Performs a FRESH TUSB320 status read (register 0x09, ATTACHED_STATE bits
 * [7:6]) on I2C BUS 1 -- deliberately not the reattach watcher's cached
 * `prev_state`, which can be up to TUSB320_REATTACH_POLL_MS (250 ms) stale
 * and is task-local. The one caller that matters (byok_power's shutdown
 * path, 2026-09-03) is about to cut the system rail; it must read the wire,
 * not a cache.
 *
 * Mirrors stock's own is_usb_powered() (BYOK.bin 1.1.0 file 0x0DB9E0,
 * called from the WAKE-hold handler at file 0x0B75AF and branched on at
 * 0x0B75B4 -> "On USB power, can't power off") -- see
 * docs/troubleshooting.md §4.
 *
 * On ESP_OK, *out_attached is true iff ATTACHED_STATE == attached.SNK.
 * Returns ESP_ERR_INVALID_ARG if out_attached is NULL,
 * ESP_ERR_INVALID_STATE if the watcher never came up (no TUSB320 device
 * handle -- byok_usb_cdc_init() never ran, or BUS 1/device-add failed), or
 * the underlying I2C error otherwise. Callers MUST decide explicitly what
 * an error means for them -- this function does not guess (byok_power's
 * caller fails OPEN: an unreadable state does not block a power-off). */
esp_err_t byok_usb_cdc_usb_attached(bool *out_attached);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_USB_CDC_H */
