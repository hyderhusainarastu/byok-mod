/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_sysbus.h — shared I2C BUS 1 handle (TUSB320 CC-chip + PCF8563 RTC)
 * ============================================================================
 *
 * hw_config.h Sec.2: BUS 1 (BYOK_SYS_I2C_PORT, GPIO8 SDA / GPIO9 SCL,
 * external pull-ups) carries TWO devices on the stock board -- the TUSB320
 * USB-C CC chip (status/interrupt only, address 0x60) and the PCF8563 RTC
 * (address 0x51). This component owns the ONE i2c_master_bus_handle_t for
 * that bus so nothing in this tree ever calls i2c_new_master_bus() on
 * BYOK_SYS_I2C_PORT more than once -- ESP-IDF's new-style i2c_master driver
 * does not allow two independent bus handles on the same port, and even if
 * it did, two owners silently racing i2c_master_bus_add_device() /
 * i2c_master_transmit() on the same physical bus would be exactly the kind
 * of bug this project's "one owner per resource" convention (see
 * byok_display.h's own "thread-safety" note for BUS 0) exists to avoid.
 *
 * BUS 0 (the display panel, GPIO18/10) is a completely separate I2C
 * peripheral and is NOT touched by this component -- see byok_display.c's
 * own bus setup, which predates this one and stays exactly as it was.
 *
 * Added 2026-09-03 for the USB re-attach fix (byok_usb_cdc.c polls the
 * TUSB320's status register 0x09 through the handle this component hands
 * out). Nothing in this tree talks to the PCF8563 yet -- byok_display's
 * CLOCK mode (byok_modes.h) is unimplemented -- but this component is where
 * that would get its bus handle from too, when it exists, rather than
 * inventing a second bus owner then.
 *
 * Thread-safety: byok_sysbus_init() itself is not safe to call from two
 * tasks concurrently (idempotent, but the idempotency check has no lock) --
 * call it once, from app_main-adjacent init code, before any task that
 * might call byok_sysbus_get_handle(). i2c_master_bus_handle_t itself is
 * safe for concurrent use by ESP-IDF's own i2c_master driver (each
 * i2c_master_dev_handle_t added to the bus serialises its own transactions
 * internally); this component does not add any device of its own -- callers
 * (byok_usb_cdc.c for TUSB320) add their own device handles against the bus
 * this returns.
 * ============================================================================
 */
#ifndef BYOK_SYSBUS_H
#define BYOK_SYSBUS_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Creates I2C BUS 1 (hw_config.h Sec.2 constants: port 1, SDA GPIO8, SCL
 * GPIO9, XTAL clock source, glitch_ignore_cnt 7, NO internal pull-up -- the
 * board supplies its own external ones) if it does not already exist.
 * Idempotent and safe to call from more than one component's init path (the
 * second and later calls are a no-op returning ESP_OK) as long as calls
 * themselves are not concurrent -- see the thread-safety note above. Returns
 * whatever i2c_new_master_bus() returned on the one call that actually
 * created the bus. */
esp_err_t byok_sysbus_init(void);

/** The BUS 1 handle, or NULL if byok_sysbus_init() has not been called yet
 * (or failed). Callers add their own i2c_master_dev_handle_t against this
 * with i2c_master_bus_add_device() -- this component does not pre-add any
 * device, since it does not know which devices a given build actually
 * talks to. */
i2c_master_bus_handle_t byok_sysbus_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_SYSBUS_H */
