/* SPDX-License-Identifier: MIT */
#include "byok_battery.h"

#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "byok_hw_shim.h" /* hw_config.h Sec.9 BYOK_BATT_.../BYOK_CHARGER_... -- CONFIRMED constants */

static const char *TAG = "byok_battery";

/* Not a vendor constant (hw_config.h has none for this) -- an
 * implementation convenience for byok_battery_get_mv()'s early-boot
 * synchronous fallback only, so a caller before the task's first pass gets
 * a reasonably fresh number without waiting for a full 200-sample read on
 * the calling task's own stack/time budget. The periodic task always uses
 * the full BYOK_BATT_SAMPLES (200), matching readBatteryVoltage() exactly. */
#define BYOK_BATT_SYNC_FALLBACK_SAMPLES 32

/* ==========================================================================
 * State
 * ========================================================================== */

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_adc_ready;   /* ADC unit + channel configured */
static bool s_cali_ready;  /* calibration handle usable */
static bool s_gpio_ready;  /* GPIO12/13 configured */

/* Ring filter + EMA state -- touched only by battery_task() (single
 * writer), so no lock needed on these. Filter shape and constants:
 * hw_config.h §9 "Filtering". */
static float    s_ring[BYOK_BATT_RING_LEN];
static int      s_ring_count;      /* how many of s_ring[] are populated (0..RING_LEN) */
static int      s_ring_idx;        /* next slot to write */
static float    s_ring_mean;       /* mean of the populated slots */
static bool     s_ring_primed;     /* s_ring_mean holds a real value */
static TickType_t s_ring_last_update_tick;

/* EMA + charging status -- read from other tasks via the getters below, so
 * these three are guarded by a portMUX critical section (cheap, never held
 * across a blocking call). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float   s_ema_mv;
static bool    s_ema_valid;
static uint8_t s_charging = BYOK_CHARGE_STATUS_NONE;

/* ==========================================================================
 * ADC front end + one raw->battery-mV measurement
 * ========================================================================== */

static esp_err_t init_adc(void)
{
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        /* hw_config.h §9 BYOK_BATT_ADC_UNIT is labelled "1" for human-
         * readable "ADC1", but the actual esp_adc_cal_characterize()
         * argument the vendor image passes (read straight off that call site
         * in the stock image, CONFIRMED) is 0 --
         * IDF v5's ADC_UNIT_1 enum value. Use the enum directly rather than
         * the macro's non-enum numeric value, to avoid that mismatch. */
        .unit_id = ADC_UNIT_1,
        .clk_src = 0, /* ADC_RTC_CLK_SRC_DEFAULT / ADC_DIGI_CLK_SRC_DEFAULT == 0 on this target */
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = (adc_atten_t)BYOK_BATT_ADC_ATTEN,       /* ADC_ATTEN_DB_12, CONFIRMED */
        .bitwidth = (adc_bitwidth_t)BYOK_BATT_ADC_WIDTH_BITS, /* 12, CONFIRMED */
    };
    err = adc_oneshot_config_channel(s_adc_handle, (adc_channel_t)BYOK_BATT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel(ch%d): %s", (int)BYOK_BATT_ADC_CHANNEL, esp_err_to_name(err));
        return err;
    }
    s_adc_ready = true;

    /* Calibration: curve fitting is the (only) scheme this chip's own
     * adc_cali_schemes.h advertises (ADC_CALI_SCHEME_CURVE_FITTING_
     * SUPPORTED); line fitting is compiled in as a fallback purely for
     * portability to a target where only it exists (ADC_CALI_SCHEME_LINE_
     * FITTING_SUPPORTED, per that target's own headers) -- hw_config.h §9's
     * own note is that there is NO fixed raw->mV constant anywhere in the
     * vendor image (it's per-chip eFuse calibration read at runtime), so a
     * hard-coded fallback constant is not an option here at all. */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        const adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .chan = (adc_channel_t)BYOK_BATT_ADC_CHANNEL,
            .atten = (adc_atten_t)BYOK_BATT_ADC_ATTEN,
            .bitwidth = (adc_bitwidth_t)BYOK_BATT_ADC_WIDTH_BITS,
        };
        err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
        if (err == ESP_OK) {
            s_cali_ready = true;
            ESP_LOGI(TAG, "ADC calibration: curve fitting scheme");
        } else {
            ESP_LOGW(TAG, "adc_cali_create_scheme_curve_fitting: %s", esp_err_to_name(err));
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!s_cali_ready) {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .atten = (adc_atten_t)BYOK_BATT_ADC_ATTEN,
            .bitwidth = (adc_bitwidth_t)BYOK_BATT_ADC_WIDTH_BITS,
        };
        err = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle);
        if (err == ESP_OK) {
            s_cali_ready = true;
            ESP_LOGI(TAG, "ADC calibration: line fitting scheme (curve fitting unavailable)");
        } else {
            ESP_LOGW(TAG, "adc_cali_create_scheme_line_fitting: %s", esp_err_to_name(err));
        }
    }
#endif

    if (!s_cali_ready) {
        ESP_LOGE(TAG, "no ADC calibration scheme available -- battery mV readings disabled "
                      "(never falling back to a hard-coded raw->mV constant, hw_config.h Sec.9)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t init_charger_gpio(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BYOK_CHARGER_CHRG_GPIO) | (1ULL << BYOK_CHARGER_STDBY_GPIO),
        .mode = GPIO_MODE_INPUT,
        /* hw_config.h §9 BYOK_CHARGER_STAT_INTERNAL_PU == 1: the vendor's
         * own gpio_config mask 0x00003000 configures both lines INPUT with
         * the internal pull-up (hw_config.h §9, CONFIRMED), matching the
         * open-drain CHRG/STDBY outputs this class of charger IC uses. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE, /* polled, matching the vendor's own power_monitor task */
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(CHRG/STDBY GPIO%d,%d): %s", (int)BYOK_CHARGER_CHRG_GPIO,
                 (int)BYOK_CHARGER_STDBY_GPIO, esp_err_to_name(err));
        return err;
    }
    s_gpio_ready = true;
    return ESP_OK;
}

/* One raw->battery-mV measurement: n_samples raw adc1 reads (trimmed mean
 * if the peak-to-peak spread exceeds BYOK_BATT_TRIM_SPREAD_PCT of the
 * plain mean), one calibration lookup on the trimmed-mean raw count, then
 * the divider formula -- all three steps as hw_config.h §9 "Battery voltage
 * maths" records them, CONFIRMED. Does NOT
 * touch ring/EMA state -- callers decide what to do with the result. */
static esp_err_t measure_battery_mv(int n_samples, uint16_t *out_mv)
{
    if (!s_adc_ready || !s_cali_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (n_samples <= 0 || n_samples > BYOK_BATT_SAMPLES) {
        n_samples = BYOK_BATT_SAMPLES;
    }

    /* Only the running sum/min/max are needed (§3.1's trim only ever drops
     * the single min and single max, never any other individual sample) --
     * no need to keep all n_samples raw counts around, so this function's
     * own locals are a handful of scalars regardless of n_samples, trivial
     * on any caller's stack (the task's periodic 200-sample pass and the
     * reduced 32-sample synchronous fallback in byok_battery_get_mv() both
     * included). */
    int64_t sum = 0;
    int rmin = INT32_MAX;
    int rmax = INT32_MIN;
    for (int i = 0; i < n_samples; i++) {
        int r = 0;
        esp_err_t e = adc_oneshot_read(s_adc_handle, (adc_channel_t)BYOK_BATT_ADC_CHANNEL, &r);
        if (e != ESP_OK) {
            return e;
        }
        sum += r;
        if (r < rmin) rmin = r;
        if (r > rmax) rmax = r;
        taskYIELD(); /* matches the vendor's vTaskDelay(0) between samples, analysis §3.1 */
    }

    int64_t mean = sum / n_samples;
    int64_t spread = (int64_t)rmax - (int64_t)rmin;
    int64_t trimmed_mean = mean;
    /* spread > 0.1*mean, done in integer without float: spread*100 >
     * mean*BYOK_BATT_TRIM_SPREAD_PCT. Needs at least 3 samples to drop one
     * min and one max and still have something left to average. */
    if (n_samples > 2 && spread * 100 > mean * BYOK_BATT_TRIM_SPREAD_PCT) {
        int64_t sum_trimmed = sum - rmin - rmax;
        trimmed_mean = sum_trimmed / (n_samples - 2);
    }

    int pin_mv = 0;
    esp_err_t e = adc_cali_raw_to_voltage(s_cali_handle, (int)trimmed_mean, &pin_mv);
    if (e != ESP_OK) {
        return e;
    }

    int batt_mv = BYOK_BATT_PIN_MV_TO_BATT_MV(pin_mv);
    if (batt_mv < 0) {
        batt_mv = 0;
    } else if (batt_mv > UINT16_MAX) {
        batt_mv = UINT16_MAX;
    }
    *out_mv = (uint16_t)batt_mv;
    return ESP_OK;
}

/* ==========================================================================
 * Charger status
 * ========================================================================== */

static uint8_t read_charger_status(void)
{
    if (!s_gpio_ready) {
        return BYOK_CHARGE_STATUS_NONE;
    }
    bool chrg_active = gpio_get_level(BYOK_CHARGER_CHRG_GPIO) == BYOK_CHARGER_CHRG_ACTIVE_LEVEL;
    bool stdby_active = gpio_get_level(BYOK_CHARGER_STDBY_GPIO) == BYOK_CHARGER_STDBY_ACTIVE_LEVEL;
    /* Exactly the vendor's three-way test (hw_config.h §9 CHRG/STDBY table,
     * CONFIRMED from three independent open-codings in the stock image). */
    if (chrg_active && !stdby_active) {
        return BYOK_CHARGE_STATUS_CHARGING;
    }
    if (!chrg_active && stdby_active) {
        return BYOK_CHARGE_STATUS_COMPLETE;
    }
    return BYOK_CHARGE_STATUS_NONE;
}

/* ==========================================================================
 * Ring filter + EMA (hw_config.h §9 "Filtering")
 * ========================================================================== */

static void reset_filter_state(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_count = 0;
    s_ring_idx = 0;
    s_ring_mean = 0.0f;
    s_ring_primed = false;

    portENTER_CRITICAL(&s_mux);
    s_ema_mv = 0.0f;
    s_ema_valid = false;
    portEXIT_CRITICAL(&s_mux);
}

/* Feeds one fresh battery-mV reading through the ring filter (gated to one
 * real update per BYOK_BATT_RING_INTERVAL_MS) and then the EMA, updating
 * s_ema_mv/s_ema_valid.
 *
 * DEVIATION FROM VENDOR, DELIBERATE (see byok_battery.h's top comment, which
 * quotes the dead comparison this refers to): the vendor's own reject-branch tail is a
 * degenerate always-same-result comparison (a vendor bug), not reproduced
 * here. This implementation's reject branch does what the surrounding code
 * clearly intends instead: on a >BYOK_BATT_RING_DROP_STEP_MV single-step
 * DROP relative to the current ring mean, treat it as a load transient and
 * skip writing it into the ring for that sample (the ring mean is held), rather
 * than let one noisy low sample drag the ring average down. A rise is never
 * rejected -- only a drop -- matching the vendor's own asymmetric intent
 * (the comment at the vendor call site is literally about rejecting a
 * transient SAG under load, not a rise). Between ring-gated updates, the
 * raw reading passes straight through to the EMA unfiltered by the ring,
 * matching the vendor's own passthrough on that path. */
static void update_ring_and_ema(uint16_t new_mv)
{
    float v = (float)new_mv;
    TickType_t now = xTaskGetTickCount();
    bool ring_due = !s_ring_primed ||
                    (now - s_ring_last_update_tick) >= pdMS_TO_TICKS(BYOK_BATT_RING_INTERVAL_MS);

    float filtered = v;
    if (ring_due) {
        bool reject = s_ring_primed && ((s_ring_mean - v) > (float)BYOK_BATT_RING_DROP_STEP_MV);
        if (!reject) {
            s_ring[s_ring_idx] = v;
            s_ring_idx = (s_ring_idx + 1) % BYOK_BATT_RING_LEN;
            if (s_ring_count < BYOK_BATT_RING_LEN) {
                s_ring_count++;
            }
            float sum = 0.0f;
            for (int i = 0; i < s_ring_count; i++) {
                sum += s_ring[i];
            }
            s_ring_mean = sum / (float)s_ring_count;
            s_ring_primed = true;
            s_ring_last_update_tick = now;
            filtered = s_ring_mean;
        } else {
            ESP_LOGD(TAG, "ring filter: rejected a %.0f mV single-step drop (ring mean %.0f mV held)",
                     (double)(s_ring_mean - v), (double)s_ring_mean);
            filtered = s_ring_mean; /* hold, don't let the rejected sample reach the EMA either */
        }
    }

    portENTER_CRITICAL(&s_mux);
    if (!s_ema_valid) {
        s_ema_mv = filtered;
        s_ema_valid = true;
    } else {
        /* ema = 0.75*ema + 0.25*new, hw_config.h §9 BYOK_BATT_EMA_ALPHA_X100 */
        float alpha = (float)BYOK_BATT_EMA_ALPHA_X100 / 100.0f;
        s_ema_mv = s_ema_mv * (1.0f - alpha) + filtered * alpha;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ==========================================================================
 * Monitor task
 * ========================================================================== */

static void battery_task(void *arg)
{
    (void)arg;
    uint8_t prev_status = BYOK_CHARGE_STATUS_NONE;
    bool have_prev = false;

    for (;;) {
        uint8_t status = read_charger_status();

        portENTER_CRITICAL(&s_mux);
        s_charging = status;
        portEXIT_CRITICAL(&s_mux);

        /* Coming OFF the charger: wipe ring+EMA state, analysis §4.3
         * ("Clearing voltage samples - coming off charger"). */
        if (have_prev && prev_status != BYOK_CHARGE_STATUS_NONE && status == BYOK_CHARGE_STATUS_NONE) {
            ESP_LOGI(TAG, "coming off charger -- clearing battery filter state");
            reset_filter_state();
        }
        prev_status = status;
        have_prev = true;

        /* "reads the battery ONLY when the charger status is 0", §4.3. */
        if (status == BYOK_CHARGE_STATUS_NONE) {
            uint16_t mv = 0;
            esp_err_t e = measure_battery_mv(BYOK_BATT_SAMPLES, &mv);
            if (e == ESP_OK) {
                update_ring_and_ema(mv);
            } else {
                ESP_LOGW(TAG, "measure_battery_mv: %s", esp_err_to_name(e));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BYOK_BATT_SAMPLE_INTERVAL_MS));
    }
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

esp_err_t byok_battery_init(void)
{
    esp_err_t err = init_adc();
    if (err != ESP_OK) {
        return err;
    }
    err = init_charger_gpio();
    if (err != ESP_OK) {
        return err;
    }

    /* Stack budget: this task's own locals are trivial (a couple of
     * uint8_t/bool/TickType_t) plus one call deep into measure_battery_mv()
     * (a handful of int64_t/int scalars only -- see that function's own
     * comment on why it does not keep a full raw[n_samples] array) one more
     * call deep into adc_oneshot_read()/adc_cali_raw_to_voltage() (esp_adc's
     * own driver frames, not sized here, same class of call depth as every
     * other driver call this project already budgets 4096 B for --
     * byok_power's WAKE task, sized on this project's I2C/display call
     * chains, is the closest comparison and uses the same figure). 4096 B
     * leaves generous margin over this component's genuinely small locals. */
    BaseType_t created = xTaskCreate(battery_task, "byok_battery", 4096, NULL, 3, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(byok_battery) failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint16_t byok_battery_get_mv(void)
{
    portENTER_CRITICAL(&s_mux);
    bool valid = s_ema_valid;
    float ema = s_ema_mv;
    portEXIT_CRITICAL(&s_mux);
    if (valid) {
        return (uint16_t)(ema + 0.5f);
    }

    uint16_t mv = 0;
    if (measure_battery_mv(BYOK_BATT_SYNC_FALLBACK_SAMPLES, &mv) != ESP_OK) {
        return 0;
    }
    return mv;
}

uint8_t byok_battery_get_pct(void)
{
    uint16_t mv = byok_battery_get_mv();
    if (mv >= BYOK_BATT_PCT_CLAMP_HI_MV) {
        return 100;
    }
    if (mv <= BYOK_BATT_PCT_CLAMP_LO_MV) {
        return 0;
    }
    static const struct { uint16_t mv; uint8_t pct; } table[BYOK_BATT_PCT_TABLE_LEN] = {
        BYOK_BATT_PCT_TABLE
    };
    /* Table is descending by mV (hw_config.h §9); find the bracketing pair
     * and linearly interpolate, matching voltageToPercent() exactly
     * (hw_config.h §9 "Voltage -> percent", CONFIRMED). */
    for (int i = 0; i < BYOK_BATT_PCT_TABLE_LEN - 1; i++) {
        if (mv <= table[i].mv && table[i + 1].mv <= mv) {
            int32_t dv = (int32_t)table[i + 1].mv - (int32_t)table[i].mv; /* negative */
            int32_t dp = (int32_t)table[i + 1].pct - (int32_t)table[i].pct;
            /* Interpolate in float, exactly as the vendor's
             * voltageToPercent() does (hw_config.h §9's
             * `p = e[i].p + (Δp/Δv)*(v-e[i].v)`), and round the *final*
             * result to the nearest integer. Doing this arithmetic in plain
             * C integers and dividing once (the previous implementation)
             * truncates toward zero; since the intermediate term is always
             * <= 0 here, that truncation always rounds the reported
             * percentage UP by up to just under 1 point relative to the
             * true interpolated value. pfloat is always >= 0 (it lies
             * between two non-negative table percentages), so a plain
             * floor(pfloat + 0.5f) is correct round-half-up. */
            float pfloat = (float)table[i].pct +
                           ((float)dp / (float)dv) * (float)((int32_t)mv - (int32_t)table[i].mv);
            int32_t p = (int32_t)(pfloat + 0.5f);
            if (p < 0) p = 0;
            if (p > 100) p = 100;
            return (uint8_t)p;
        }
    }
    return 0; /* unreached given the clamp checks above, kept for safety */
}

uint8_t byok_battery_get_charging(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t status = s_charging;
    portEXIT_CRITICAL(&s_mux);
    return status;
}
