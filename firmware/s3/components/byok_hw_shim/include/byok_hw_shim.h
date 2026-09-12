/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_hw_shim.h — resolves firmware/common/hw_config.h §13 build-blockers
 * ============================================================================
 *
 * hw_config.h ends with a block of `#ifndef ... #error` guards (§13,
 * "UNRESOLVED") for every value the reverse-engineering pass could not pin
 * down. That is intentional on the RE side — it stops a build from silently
 * assuming a value nobody actually confirmed — but it also means **any**
 * translation unit that does `#include "hw_config.h"` without first defining
 * those macros fails to compile, whether or not it touches the unresolved
 * feature.
 *
 * This header is the one place that resolves them, so hw_config.h itself
 * stays untouched (it is RE evidence, graded CONFIRMED/STRONGLY
 * INDICATED/POSSIBLE/UNKNOWN per SAFETY.md §2 — this project does not edit
 * evidence files to make a build pass). Every value defined below is either:
 *
 *   (a) a functional no-op ("this GPIO/opcode is unresolved and this
 *       firmware does not touch it"), or
 *   (b) a Kconfig-backed placeholder (menuconfig: "BYOK hardware unknowns"),
 *       loudly logged at boot by byok_hw_shim_log_warnings() so a placeholder
 *       never quietly passes for a measurement.
 *
 * Include THIS header wherever you would otherwise `#include "hw_config.h"`.
 * Do not include hw_config.h directly from a new translation unit — the
 * guards will fire.
 * ============================================================================
 */
#ifndef BYOK_HW_SHIM_H
#define BYOK_HW_SHIM_H

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

/* -- hw_config.h §13 items with no functional consequence for this firmware,
 * because we never drive the pin / never reinterpret the opcode -- resolved
 * to an inert 0 rather than a Kconfig option; there is nothing for a user to
 * usefully tune here. */
#define BYOK_GPIO39_FUNCTION        0 /* UNKNOWN. This resolves hw_config.h Sec.13's guard on GPIO39's
                                        * semantic PURPOSE only, which is still unresolved. Since
                                        * 0.1.6, CONFIG_BYOK_STOCK_GPIO_PARITY (default y) MAY
                                        * configure and drive the pin itself (OUTPUT, LOW, stock-
                                        * parity) via hw_config.h's BYOK_GPIO39_PIN, without this
                                        * placeholder ever meaning anything more than "we still don't
                                        * know what it does" -- see main/app_main.c and
                                        * docs/pinout.md §5. */
#define BYOK_GPIO48_FUNCTION        0 /* UNKNOWN. This firmware never configures or reads GPIO48. */
#define BYOK_LCD_CMD_C9_MEANING     0 /* UNKNOWN. byok_display replays 0xC9,0xAC verbatim regardless of meaning. */
#define BYOK_LCD_CMD_95_MEANING     0 /* UNKNOWN. byok_display replays 0x95 verbatim regardless of meaning. */
#define BYOK_LCD_CMD_E1_MEANING     0 /* UNKNOWN. byok_display replays 0xE1,0xE2 verbatim regardless of meaning. */

/* -- hw_config.h §13 items that DO have functional consequences and are
 * genuinely guessed. Kconfig-backed (menuconfig: "BYOK hardware unknowns
 * (RE placeholders)", this component's Kconfig) so they can be revised
 * without touching source, and so the guess is visible in sdkconfig. --
 *
 * BYOK_BATT_DIVIDER_RATIO_X1000 and BYOK_CHARGER_STAT_POLARITY used to be
 * guessed here (Kconfig placeholders BYOK_HW_BATT_DIVIDER_RATIO_X1000 /
 * BYOK_HW_CHARGER_STAT_POLARITY). Both are now resolved CONFIRMED values
 * defined directly in hw_config.h §9, where the disassembly evidence for each
 * of them is inlined —
 * defining them again here would just be a second, stale copy fighting the
 * real one, so they are gone from this shim and from this component's
 * Kconfig. */

/** [GUESS] hw_config.h §13 BYOK_LCD_FB_BIT_ORDER: byte-index arithmetic is
 * CONFIRMED ((y>>3)*240+x); which bit of that byte is the top row of the
 * page is only STRONGLY INDICATED (never observed on real pixels). Defaults
 * to the header's own suggested guess (1 = LSB is the top row); flip
 * CONFIG_BYOK_HW_LCD_FB_BIT_ORDER_LSB_TOP off if the panel comes up mirrored
 * vertically within each 8-row page once real hardware is driven. */
#if CONFIG_BYOK_HW_LCD_FB_BIT_ORDER_LSB_TOP
#define BYOK_LCD_FB_BIT_ORDER 1
#else
#define BYOK_LCD_FB_BIT_ORDER 0
#endif

#include "hw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Logs one ESP_LOGW per placeholder above, at boot, so a build that carries
 * an unresolved-hardware guess cannot pass silently. Call once from
 * app_main() after the log system + NVS are up. */
void byok_hw_shim_log_warnings(void);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_HW_SHIM_H */
