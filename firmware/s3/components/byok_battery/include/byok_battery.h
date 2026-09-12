/* SPDX-License-Identifier: MIT
 * ============================================================================
 * byok_battery.h — battery voltage + charger status (hw_config.h §9)
 * ============================================================================
 *
 * Reimplements the vendor's `power_monitor` task, disassembled out of the
 * stock 1.1.0 app image and recorded constant-by-constant in hw_config.h §9,
 * using the current ESP-IDF v5.x ADC API — not the legacy `adc1_*` /
 * `esp_adc_cal_*` calls the vendor image itself used, which are what the
 * analysis actually disassembled. Every CONFIRMED constant (ADC front end,
 * divider formula, threshold/table values, GPIO12/13 polarity) is taken
 * verbatim from hw_config.h §9; this file adds no new hardware constants of
 * its own.
 *
 * Pipeline (hw_config.h §9), one cycle per BYOK_BATT_SAMPLE_
 * INTERVAL_MS (2 s):
 *
 *   1. Read GPIO12/13 (CHRG/STDBY, both active LOW, internal pull-up) ->
 *      charger status (BYOK_CHARGE_STATUS_NONE/_CHARGING/_COMPLETE).
 *   2. If (and only if) status == NONE, per §4.3's "reads the battery ONLY
 *      when the charger status is 0": take BYOK_BATT_SAMPLES raw ADC1
 *      samples (trimmed mean per §3.1: drop min+max if the peak-to-peak
 *      spread exceeds BYOK_BATT_TRIM_SPREAD_PCT of the mean), convert the
 *      trimmed-mean raw count to millivolts via the RUNTIME calibration
 *      handle (adc_cali_raw_to_voltage — never a baked raw->mV constant,
 *      §3.2), then apply BYOK_BATT_PIN_MV_TO_BATT_MV() (the 5/3 divider +
 *      two +20 mV offsets, §3.3) to get the cell voltage in mV.
 *   3. Feed that mV value through a 5-entry ring filter gated to one update
 *      per BYOK_BATT_RING_INTERVAL_MS (§4.2): a single-step drop bigger than
 *      BYOK_BATT_RING_DROP_STEP_MV is treated as a load transient and
 *      discarded (the ring mean is held, not updated, for that pass).
 *      DEVIATION FROM VENDOR, DELIBERATE: the vendor's own tail path on that
 *      reject branch is a degenerate/dead comparison that always evaluates
 *      to the same branch (`0.1 < 0.025` is
 *      compile-time-constant-false, so vendor's "clamp" always returns
 *      `v + 0.025` regardless of the actual values compared) — that is a
 *      vendor bug, not intended filtering behaviour, and is NOT reproduced
 *      here. This component's reject branch instead does exactly what the
 *      surrounding logic actually intends: hold the last valid ring mean.
 *   4. Apply an EMA (BYOK_BATT_EMA_ALPHA_X100 = 25, i.e. ema = 0.75*ema +
 *      0.25*new) on top of the ring output (§4.3). Every getter below reads
 *      the EMA, never an instantaneous sample — matching the vendor
 *      (thresholds "are tested against the EMA, never against an
 *      instantaneous reading").
 *   5. Coming OFF the charger (status transitions non-NONE -> NONE), the
 *      whole filter state (ring, index, mean, EMA) is wiped, matching §4.3's
 *      "Clearing voltage samples - coming off charger".
 *
 * Percent is computed from the EMA millivolts via the 21-entry
 * BYOK_BATT_PCT_TABLE + linear interpolation (§4.4), clamped at both ends.
 *
 * ADC calibration: adc_cali_create_scheme_curve_fitting() is the ESP32-S3
 * scheme (the only one this chip's `adc_cali_schemes.h` advertises,
 * ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED) and is tried first; a build
 * targeting a chip where only line fitting exists falls back to
 * adc_cali_create_scheme_line_fitting() (compiled in only when
 * ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED is set by that chip's own headers).
 * If NEITHER scheme is available, byok_battery_init() returns an error
 * rather than ever computing mV from a hard-coded raw->mV constant — see
 * hw_config.h §9's own note that there is no such constant in the vendor
 * image (it is per-chip eFuse calibration, read at runtime).
 *
 * Thread-safety: all mutable state is touched only by the internal
 * "byok_battery" task except for the getters, which take a lightweight
 * critical section (portMUX) around the handful of ints/floats they read —
 * cheap enough not to need a full mutex, and never held across a blocking
 * call. Getters may be called from any task, including before the first
 * measurement pass completes (see byok_battery_get_mv()'s own doc comment
 * for the synchronous fallback that covers that window).
 */
#ifndef BYOK_BATTERY_H
#define BYOK_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configures the ADC1 front end (hw_config.h §9: unit 1 legacy-numbered /
 * ADC_UNIT_1 enum, channel 0 = GPIO1, 12-bit, ADC_ATTEN_DB_12), the
 * calibration scheme (curve fitting, falling back to line fitting), and
 * GPIO12/GPIO13 as inputs with the internal pull-up (hw_config.h §9:
 * BYOK_CHARGER_STAT_INTERNAL_PU == 1, matching the vendor's own
 * gpio_config mask 0x00003000). Starts the 2 s monitor task. Call once from
 * app_main, before the boot self-test draws the resting screen (the task
 * needs a few 2 s passes to produce an EMA reading — see
 * byok_battery_get_mv()'s fallback for what happens before that).
 *
 * Returns the first hard failure from ADC/calibration/GPIO bring-up, or
 * whatever xTaskCreate's failure maps to (ESP_ERR_NO_MEM) if the task
 * cannot be created. Non-fatal to boot by convention in this firmware (see
 * every other *_init() in main/app_main.c) — a caller should log-and-
 * continue, not ESP_ERROR_CHECK. On failure the getters keep returning
 * their unmeasured defaults (0 mV, 0 pct, BYOK_CHARGE_STATUS_NONE) —
 * there is no 0xFF "unknown" sentinel here; see app_main.c's
 * build_status_payload() comment for why none is needed. */
esp_err_t byok_battery_init(void);

/** Latest EMA-filtered battery voltage, in millivolts. If no measurement has
 * completed yet (e.g. called immediately after byok_battery_init() returns,
 * before the task's first 2 s pass has had a chance to run — the boot
 * self-test's resting screen is the main such caller, typically only ~8 s
 * after byok_battery_init() so this is rarely hit in practice), this
 * performs one synchronous, reduced-sample (BYOK_BATT_SYNC_FALLBACK_SAMPLES,
 * not the full BYOK_BATT_SAMPLES) measurement instead of returning a bogus
 * 0 — unconditionally (unlike the periodic task, this one-off fallback does
 * not gate on charger status: a caller needs *some* number in the
 * early-boot window, and taking one ADC reading is harmless regardless of
 * charging). That one-off reading is NOT fed into the ring/EMA state, so
 * the task's own filtering is unaffected by it. Returns 0 only if the ADC/
 * calibration front end itself is unavailable (byok_battery_init() failed
 * or was never called). */
uint16_t byok_battery_get_mv(void);

/** Battery percent, 0-100, from byok_battery_get_mv() via the vendor's
 * 21-entry table + linear interpolation (hw_config.h §9
 * BYOK_BATT_PCT_TABLE), clamped at BYOK_BATT_PCT_CLAMP_HI_MV/_LO_MV. */
uint8_t byok_battery_get_pct(void);

/** Charger status: BYOK_CHARGE_STATUS_NONE (0) / _CHARGING (1) / _COMPLETE
 * (2) — the same encoding docs/protocol.md §6.1 STATUS.charging and §6.3
 * BATTERY.charging use. Read fresh from GPIO12/13 every monitor-task pass
 * (independent of whether that pass also took an ADC reading); 0 (NONE) if
 * byok_battery_init() was never called or its GPIO config failed. */
uint8_t byok_battery_get_charging(void);

#ifdef __cplusplus
}
#endif

#endif /* BYOK_BATTERY_H */
