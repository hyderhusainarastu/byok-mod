/* SPDX-License-Identifier: MIT */
#include "byok_idle.h"

#include <stddef.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "driver/gpio.h"

#include "byok_display.h"
#include "byok_hw_shim.h" /* BYOK_BTN_*_GPIO, BYOK_BACKLIGHT_LEVEL_COUNT/_LEVELS_PCT/_FADE_MS_BUTTON */
#include "byok_menu.h"    /* 0.1.14: preset-menu input routing */
#include "byok_note.h"
#include "byok_nvs.h"
#include "byok_proto.h"   /* 0.1.14: BYOK_BUTTON_x / BYOK_BUTTON_STATE_x wire constants */

static const char *TAG = "byok_idle";

#define POLL_MS        20u
#define DEBOUNCE_POLLS 2u
/* 0.1.14: EXECUTE's short-vs-long-press split, at the >= 1 s threshold
 * docs/protocol.md Sec.6.4b specifies. Held continuously for this long
 * fires the long-press action once (menu open); released before that fires
 * the short-press action instead -- see idle_button_task()'s own comment. */
#define LONG_PRESS_MS 1000u

static byok_idle_button_event_cb_t s_button_event_cb;

static void emit_button_event(uint8_t button, uint8_t state)
{
    if (s_button_event_cb != NULL) {
        s_button_event_cb(button, state, (uint32_t)(esp_timer_get_time() / 1000));
    }
}

void byok_idle_set_button_event_cb(byok_idle_button_event_cb_t cb)
{
    s_button_event_cb = cb;
}

/* Cycle order for UP (forward) / DOWN (backward) -- see byok_idle.h's own
 * header comment for why DOWN's direction is this component's own choice
 * rather than an externally defined one. */
static const byok_app_mode_t k_cycle[] = {
    BYOK_MODE_CLOCK,
    BYOK_MODE_STATIC_NOTE,
    BYOK_MODE_BLANK,
};
#define CYCLE_LEN (sizeof(k_cycle) / sizeof(k_cycle[0]))

static const uint8_t k_backlight_pct[BYOK_BACKLIGHT_LEVEL_COUNT] = BYOK_BACKLIGHT_LEVELS_PCT;

static bool    s_inited;
static int     s_cycle_idx;      /* index into k_cycle -- the currently SELECTED idle submode */
static uint8_t s_backlight_idx;  /* index into k_backlight_pct */

/* byok_display_set_backlight() takes 0-255; hw_config.h's own table is in
 * 0-100 percent (the vendor's own domain, disassembly-confirmed). This is
 * the exact inverse of byok_display_set_backlight()'s own pct = level*100/
 * 255 (byok_display.c) -- rounding to nearest rather than truncating so the
 * five stock percentages land as close to their vendor-intended duty as an
 * 8-bit `level` can represent. */
static uint8_t pct_to_level(uint8_t pct)
{
    return (uint8_t)(((uint32_t)pct * 255u + 50u) / 100u);
}

static void backlight_apply_hw(void)
{
    if (!byok_display_is_ready()) {
        return;
    }
    esp_err_t err = byok_display_set_backlight(pct_to_level(k_backlight_pct[s_backlight_idx]),
                                                BYOK_BACKLIGHT_FADE_MS_BUTTON);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_display_set_backlight: %s", esp_err_to_name(err));
    }
}

void byok_idle_restore_backlight(void)
{
    backlight_apply_hw();
}

static void backlight_cycle(void)
{
    s_backlight_idx = (uint8_t)((s_backlight_idx + 1u) % BYOK_BACKLIGHT_LEVEL_COUNT);
    backlight_apply_hw();
    if (byok_nvs_is_armed()) {
        esp_err_t err = byok_nvs_set_backlight_idx(s_backlight_idx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "byok_nvs_set_backlight_idx: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "backlight -> %u%% (index %u)", (unsigned)k_backlight_pct[s_backlight_idx],
             (unsigned)s_backlight_idx);
}

static void apply_cycle_idx(int idx)
{
    s_cycle_idx = ((idx % (int)CYCLE_LEN) + (int)CYCLE_LEN) % (int)CYCLE_LEN;
    byok_app_mode_t m = k_cycle[s_cycle_idx];
    esp_err_t err = byok_modes_set(m);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_modes_set(%s): %s", byok_modes_name(m), esp_err_to_name(err));
        return;
    }
    if (byok_nvs_is_armed()) {
        esp_err_t nerr = byok_nvs_set_idle_mode((uint8_t)m);
        if (nerr != ESP_OK) {
            ESP_LOGW(TAG, "byok_nvs_set_idle_mode: %s", esp_err_to_name(nerr));
        }
    }
    ESP_LOGI(TAG, "idle submode -> %s", byok_modes_name(m));
}

esp_err_t byok_idle_force_submode(byok_app_mode_t submode)
{
    for (size_t i = 0; i < CYCLE_LEN; i++) {
        if (k_cycle[i] == submode) {
            apply_cycle_idx((int)i);
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

void byok_idle_enter_selected_submode(void)
{
    apply_cycle_idx(s_cycle_idx);
}

/* ==========================================================================
 * byok_modes change callback -- STATIC_NOTE/BLANK entry, CLOCK/BLANK
 * backlight handling. byok_clock.c registers its own callback separately
 * for CLOCK's actual rendering; both fire on a CLOCK entry (harmless, one
 * draws the clock face, this one only touches the backlight -- see this
 * file's header comment on why backlight is re-applied unconditionally on
 * every CLOCK/STATIC_NOTE entry, not just after BLANK).
 * ========================================================================== */
static void idle_mode_changed(byok_app_mode_t new_mode, byok_app_mode_t old_mode, void *ctx)
{
    (void)ctx;
    if (new_mode == old_mode) {
        return;
    }
    switch (new_mode) {
    case BYOK_MODE_STATIC_NOTE:
        backlight_apply_hw(); /* undo a prior BLANK's backlight-off, if any */
        byok_note_render();
        break;
    case BYOK_MODE_CLOCK:
        backlight_apply_hw(); /* same -- byok_clock.c's own callback draws the face */
        break;
    case BYOK_MODE_BLANK:
        if (byok_display_is_ready()) {
            byok_display_clear(false);
            byok_display_full_refresh(false);
            esp_err_t err = byok_display_set_backlight(0, BYOK_BACKLIGHT_FADE_MS_BUTTON);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "byok_display_set_backlight(0): %s", esp_err_to_name(err));
            }
        }
        break;
    default:
        break; /* BYOK_MODE_DASHBOARD_USB, BYOK_MODE_ORIGINAL: not this component's concern */
    }
}

/* ==========================================================================
 * Button poll task
 * ========================================================================== */

/* 0.1.14: UP/DOWN/BRIGHTNESS/EXECUTE-short-press share one shape --
 * "menu open? route there. host active? emit an EVT_BUTTON. else (idle):
 * do this button's own idle-time action" -- matching what docs/protocol.md
 * Sec.6.4b and Sec.6.5 specify: while a host owns the display and the menu
 * is closed, UP/DOWN/BRIGHTNESS and an EXECUTE short press all emit
 * EVT_BUTTON. Before 0.1.14, the "host active" branch below did nothing at
 * all (the idle-cycle guard already skipped every button then) -- see
 * byok_idle.h's own updated header comment. */
static void up_short(void)
{
    if (byok_menu_is_open()) {
        byok_menu_move(+1);
        return;
    }
    if (byok_modes_get() == BYOK_MODE_DASHBOARD_USB) {
        emit_button_event(BYOK_BUTTON_UP, BYOK_BUTTON_STATE_PRESSED);
        return;
    }
    apply_cycle_idx(s_cycle_idx + 1);
}

static void down_short(void)
{
    if (byok_menu_is_open()) {
        byok_menu_move(-1);
        return;
    }
    if (byok_modes_get() == BYOK_MODE_DASHBOARD_USB) {
        emit_button_event(BYOK_BUTTON_DOWN, BYOK_BUTTON_STATE_PRESSED);
        return;
    }
    apply_cycle_idx(s_cycle_idx - 1);
}

static void brightness_short(void)
{
    if (byok_menu_is_open()) {
        return; /* not a menu control -- ignored while the menu owns input, see byok_idle.h */
    }
    if (byok_modes_get() == BYOK_MODE_DASHBOARD_USB) {
        emit_button_event(BYOK_BUTTON_BRIGHTNESS, BYOK_BUTTON_STATE_PRESSED);
        return;
    }
    backlight_cycle();
}

/* EXECUTE short press (< LONG_PRESS_MS): menu-select if open, else the
 * same "emit while host-active, else idle-cycle" shape as the other three
 * -- EXECUTE's own idle-time action is (and was, pre-0.1.14) the backlight
 * cycle, same as BRIGHTNESS. Never fires if the long-press action below
 * already did (idle_button_task()'s own long_fired guard). */
static void on_execute_short(void)
{
    if (byok_menu_is_open()) {
        byok_menu_select();
        return;
    }
    if (byok_modes_get() == BYOK_MODE_DASHBOARD_USB) {
        emit_button_event(BYOK_BUTTON_EXECUTE, BYOK_BUTTON_STATE_PRESSED);
        return;
    }
    backlight_cycle();
}

/* EXECUTE long press (>= LONG_PRESS_MS, held): opens the preset menu.
 * docs/protocol.md Sec.6.4b specifies this for a device that is host-active
 * (HOST/MIRROR) or already showing one of its own idle screens -- i.e.
 * unconditionally, unlike the four short-press handlers above, which all
 * branch on byok_modes_get(). A no-op if the menu is already open
 * (byok_menu_open_default() itself is idempotent) -- holding EXECUTE
 * through an already-open menu does not re-trigger anything. This is NOT
 * itself an EVT_BUTTON emission -- opening the menu IS the action; see
 * byok_idle.h's own "long = menu" framing. */
static void on_execute_long(void)
{
    byok_menu_open_default();
}

typedef struct {
    gpio_num_t gpio;
    const char *name;
    void (*on_short_press)(void);
    void (*on_long_press)(void); /* NULL if this button has no long-press action */
    int trusted_level;   /* -1 = unknown until the first debounced read; 0 = pressed (active-low) */
    int raw_level;
    unsigned raw_run;
    uint32_t press_start_ms; /* esp_timer ms at the debounced press edge; meaningful only while trusted_level == 0 */
    bool     long_fired;     /* on_long_press already ran for this hold -- suppresses the matching on_short_press on release */
} btn_t;

static btn_t s_buttons[4]; /* filled in by byok_idle_init() -- BYOK_BTN_*_GPIO
                             * values come from hw_config.h via byok_hw_shim.h,
                             * not available as compile-time initialiser
                             * constants safe to assume before that header is
                             * pulled in, so this is populated at runtime. */

static void configure_button_gpio(gpio_num_t gpio)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   /* hw_config.h Sec.8: board supplies its own */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO%d): %s", (int)gpio, esp_err_to_name(err));
    }
}

/* 0.1.14: extended from a single "fire on_press() at the debounced press
 * edge" shape to three distinct edges/conditions, needed to tell a short
 * press from a long-held one WHILE it is still held (not just at release):
 *
 *   1. Debounced press edge (trusted_level: 1 -> 0): record press_start_ms,
 *      clear long_fired. No handler fires yet -- a press alone is not yet
 *      known to be short or long.
 *   2. Still held, on_long_press set, not yet fired, and held >=
 *      LONG_PRESS_MS: fire on_long_press() once, set long_fired. Checked
 *      every poll while trusted_level == 0, so this fires (with up to
 *      POLL_MS jitter) the instant the hold crosses the threshold, not on
 *      release.
 *   3. Debounced release edge (trusted_level: 0 -> 1): if long_fired is
 *      still false (the hold never reached the threshold, or this button
 *      has no on_long_press at all), fire on_short_press(). If long_fired
 *      is true, the long-press action already ran -- releasing does
 *      nothing further. */
static void idle_button_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        for (size_t i = 0; i < 4; i++) {
            btn_t *b = &s_buttons[i];
            int level = gpio_get_level(b->gpio);

            if (level == b->raw_level) {
                if (b->raw_run < DEBOUNCE_POLLS) {
                    b->raw_run++;
                }
            } else {
                b->raw_level = level;
                b->raw_run = 1;
            }

            if (b->raw_run >= DEBOUNCE_POLLS && b->trusted_level != b->raw_level) {
                int prev = b->trusted_level;
                b->trusted_level = b->raw_level;
                if (b->trusted_level == 0) { /* active-low: 0 = pressed, debounced press edge */
                    ESP_LOGD(TAG, "%s pressed", b->name);
                    b->press_start_ms = now_ms;
                    b->long_fired = false;
                } else if (prev == 0) { /* debounced release edge, and it really was pressed before */
                    ESP_LOGD(TAG, "%s released", b->name);
                    if (!b->long_fired) {
                        b->on_short_press();
                    }
                }
            }

            if (b->trusted_level == 0 && !b->long_fired && b->on_long_press != NULL &&
                (now_ms - b->press_start_ms) >= LONG_PRESS_MS) {
                ESP_LOGD(TAG, "%s long-press (>= %u ms)", b->name, (unsigned)LONG_PRESS_MS);
                b->long_fired = true;
                b->on_long_press();
            }
        }
    }
}

void byok_idle_init(void)
{
    if (s_inited) {
        return;
    }

    s_buttons[0] = (btn_t){ .gpio = (gpio_num_t)BYOK_BTN_UP_GPIO,         .name = "UP",         .on_short_press = up_short,         .on_long_press = NULL,           .trusted_level = -1, .raw_level = -1, .raw_run = 0 };
    s_buttons[1] = (btn_t){ .gpio = (gpio_num_t)BYOK_BTN_DOWN_GPIO,       .name = "DOWN",       .on_short_press = down_short,       .on_long_press = NULL,           .trusted_level = -1, .raw_level = -1, .raw_run = 0 };
    s_buttons[2] = (btn_t){ .gpio = (gpio_num_t)BYOK_BTN_EXECUTE_GPIO,    .name = "EXECUTE",    .on_short_press = on_execute_short, .on_long_press = on_execute_long, .trusted_level = -1, .raw_level = -1, .raw_run = 0 };
    s_buttons[3] = (btn_t){ .gpio = (gpio_num_t)BYOK_BTN_BRIGHTNESS_GPIO, .name = "BRIGHTNESS", .on_short_press = brightness_short, .on_long_press = NULL,           .trusted_level = -1, .raw_level = -1, .raw_run = 0 };
    for (size_t i = 0; i < 4; i++) {
        configure_button_gpio(s_buttons[i].gpio);
    }

    uint8_t persisted_mode;
    s_cycle_idx = 0; /* CLOCK, this firmware's pre-0.1.13 default */
    if (byok_nvs_is_armed() && byok_nvs_get_idle_mode(&persisted_mode) == ESP_OK) {
        for (size_t i = 0; i < CYCLE_LEN; i++) {
            if ((uint8_t)k_cycle[i] == persisted_mode) {
                s_cycle_idx = (int)i;
                break;
            }
        }
    }

    s_backlight_idx = BYOK_BACKLIGHT_LEVEL_COUNT - 1u; /* 100%, this firmware's pre-0.1.13
                                                          * boot-time default (byok_display_
                                                          * set_backlight(200, 0) in app_main.c) */
    uint8_t persisted_idx;
    if (byok_nvs_is_armed() && byok_nvs_get_backlight_idx(&persisted_idx) == ESP_OK &&
        persisted_idx < BYOK_BACKLIGHT_LEVEL_COUNT) {
        s_backlight_idx = persisted_idx;
    }

    esp_err_t reg_err = byok_modes_register_change_cb(idle_mode_changed, NULL);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "byok_modes_register_change_cb: %s -- STATIC_NOTE/BLANK entry rendering "
                      "and BLANK's backlight handling will not run", esp_err_to_name(reg_err));
    }

    BaseType_t created = xTaskCreate(idle_button_task, "byok_idle_btn", 3072, NULL, 3, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(byok_idle_btn) failed -- UP/DOWN/EXECUTE/BRIGHTNESS "
                      "idle-mode-cycling disabled this boot");
        return;
    }
    s_inited = true;
}
