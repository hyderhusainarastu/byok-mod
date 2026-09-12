/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_rtc.h — PCF8563 real-time clock driver (BYOK v2.1, I2C BUS 1)
 * ============================================================================
 *
 * hw_config.h Sec.2 (BYOK_PCF8563_I2C_ADDR 0x51, 400 kHz) is CONFIRMED from
 * the vendor disassembly -- PCF8563::init(bus_handle) is called on I2C BUS 1
 * (hw_config.h Sec.2, file offset 0x117354) right after the TUSB320. That
 * analysis went no further than init(): no read/write register access from the
 * vendor's own RTC driver was disassembled, so there is no CONFIRMED
 * evidence here for which registers the vendor reads or in what order.
 *
 * This driver's REGISTER MAP is therefore NOT reverse-engineered evidence --
 * it is NXP's own public PCF8563 datasheet register layout (a single,
 * unambiguous part: this chip has exactly one register map, the same one
 * every PCF8563 on the market implements), applied to a CONFIRMED address
 * (0x51) and CONFIRMED bus (BUS 1, byok_sysbus). Graded per SAFETY.md §2
 * as STRONGLY INDICATED, not CONFIRMED: strong evidence (the part is
 * CONFIRMED to be a PCF8563, and PCF8563s do not have alternate register
 * maps), but not itself read out of the vendor's instruction stream.
 *
 *   Reg  Name              Bits of interest
 *   0x00 Control/status 1   (not used by this driver -- STOP/TEST1 bits)
 *   0x01 Control/status 2   (not used by this driver -- alarm/timer IE/IF)
 *   0x02 VL_seconds          bit7 VL (voltage-low / time-may-be-invalid),
 *                            bits0-6 seconds, BCD 00-59
 *   0x03 Minutes             bits0-6 BCD 00-59
 *   0x04 Hours                bits0-5 BCD 00-23
 *   0x05 Days                  bits0-5 BCD 01-31
 *   0x06 Weekdays               bits0-2, 0-6 (chip does not fix which day is
 *                                0 -- this driver treats it as an opaque
 *                                round-tripped value, never rendering a day
 *                                name from it)
 *   0x07 Century_months          bit7 century (toggles 0<->1 each time the
 *                                 Years register rolls 99->00; this driver
 *                                 assumes bit7=0 means the 2000s, matching
 *                                 every PCF8563 shipped from its factory
 *                                 default and this device's real-world era),
 *                                 bits0-4 BCD month 01-12
 *   0x08 Years                    bits0-7 BCD 00-99 (+ century bit above)
 *
 * Registers 0x02-0x08 auto-increment on both read and write when addressed
 * as one burst (a documented PCF8563 feature, not vendor-specific) -- this
 * driver always reads/writes all seven in a single I2C transaction pair
 * (one write of the start register, one read/write of the 7-byte block),
 * the same i2c_master_transmit_receive() shape byok_usb_cdc.c already uses
 * for the TUSB320 on this same bus.
 *
 * Bus ownership: this driver adds its OWN i2c_master_dev_handle_t against
 * byok_sysbus's shared BUS 1 handle (byok_sysbus.h) -- it does not create a
 * bus of its own and does not touch the TUSB320's device handle. Call
 * byok_rtc_init() any time after byok_sysbus_init() has been called at least
 * once (byok_rtc_init() also calls it itself, idempotently, so callers don't
 * need to sequence around byok_usb_cdc's own call to it).
 *
 * Thread-safety: none of its own. byok_rtc_read()/byok_rtc_write() issue a
 * single blocking I2C transaction each (i2c_master_transmit_receive() /
 * i2c_master_transmit()); ESP-IDF's own i2c_master driver serialises
 * concurrent callers against the same i2c_master_dev_handle_t internally
 * (see byok_sysbus.h's own thread-safety note), so two tasks calling this
 * driver at once cannot tear a single register burst -- but nothing here
 * makes a READ-then-decide-then-WRITE sequence atomic across two calls. This
 * project's only caller (byok_clock, one task) never needs that.
 * ============================================================================
 */
#ifndef BYOK_RTC_H
#define BYOK_RTC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One point in time, decoded to plain binary (never BCD) at this driver's
 * public boundary -- BCD is a wire/register detail of the PCF8563 itself,
 * not something a caller should ever have to know about. `year` is the
 * FULL year (e.g. 2026), not the chip's 2-digit register value -- see this
 * header's own register-map comment for how the century bit folds in.
 * `weekday` is opaque (0-6, whatever the chip already holds / whatever the
 * caller last wrote) -- this driver never assigns it a Sunday-is-0 or
 * Monday-is-0 meaning of its own. */
typedef struct {
    uint16_t year;     /*!< full year, e.g. 2026 (2000-2199, this driver's supported span --
                         *   see byok_rtc_write()'s own range check) */
    uint8_t  month;    /*!< 1-12 */
    uint8_t  day;      /*!< 1-31 */
    uint8_t  weekday;  /*!< 0-6, opaque -- see struct comment above */
    uint8_t  hour;     /*!< 0-23 */
    uint8_t  minute;   /*!< 0-59 */
    uint8_t  second;   /*!< 0-59 */
    bool     voltage_low; /*!< read-only: the chip's own VL flag (register 0x02 bit7) --
                            *   true means the RTC lost power at some point since this
                            *   flag was last cleared and the time it now reports may be
                            *   wrong. byok_rtc_write() always clears it (every real
                            *   write of the seconds register does, on this chip family --
                            *   not something this driver has to do as a separate step).
                            *   Ignored (not read) on write; always set by read. */
} byok_rtc_time_t;

/** Adds this driver's device handle to byok_sysbus's shared BUS 1 (calling
 * byok_sysbus_init() itself first, idempotently, if it has not already run).
 * Safe to call more than once (a second call is a no-op returning ESP_OK).
 * Does NOT read or write the chip -- byok_rtc_read()/_write() are the first
 * calls that actually touch I2C. */
esp_err_t byok_rtc_init(void);

/** True once byok_rtc_init() has succeeded; false before it is called, or
 * if it failed (no bus, or i2c_master_bus_add_device() failed). Callers
 * (byok_clock) use this to decide whether to even attempt a read rather
 * than reading ESP_ERR_INVALID_STATE off every call in a loop. */
bool byok_rtc_is_ready(void);

/** Reads registers 0x02-0x08 in one burst and decodes them into `*out`.
 * Returns ESP_ERR_INVALID_STATE if byok_rtc_init() has not succeeded,
 * ESP_ERR_INVALID_ARG if `out` is NULL, otherwise whatever the I2C
 * transaction returned (ESP_OK on success). Does not itself validate the
 * decoded fields against `voltage_low` -- a caller that cares whether the
 * result is trustworthy checks `out->voltage_low` itself; this function
 * still fills in whatever the chip reports either way (a stopped/reset
 * PCF8563 typically reads back all-zero-ish rather than garbage, so "some
 * value, flagged untrustworthy" is more useful to a caller than a hard
 * failure). */
esp_err_t byok_rtc_read(byok_rtc_time_t *out);

/** Encodes `*t` to BCD and writes registers 0x02-0x08 in one burst,
 * clearing the chip's VL flag as a side effect (this chip family always
 * clears VL on a real write to the seconds register -- not a separate step
 * this driver has to perform). Returns ESP_ERR_INVALID_STATE if
 * byok_rtc_init() has not succeeded, ESP_ERR_INVALID_ARG if `t` is NULL or
 * any field is out of its documented range (month 1-12, day 1-31, weekday
 * 0-6, hour 0-23, minute/second 0-59, year 2000-2199 -- the one span this
 * driver's century-bit handling supports, see the struct comment), otherwise
 * whatever the I2C transaction returned. Does NOT validate day-of-month
 * against month/leap-year (e.g. day=31 for April is accepted and written
 * verbatim) -- the chip itself does no such validation either, and this is a
 * device-side write path with no host-side caller yet (docs/protocol.md
 * Sec.6.4 SET_TIME, v1.1) to have already done that check. */
esp_err_t byok_rtc_write(const byok_rtc_time_t *t);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_RTC_H */
