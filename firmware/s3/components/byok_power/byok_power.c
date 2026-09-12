/* SPDX-License-Identifier: MIT */
#include "byok_power.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "byok_display.h"
#include "byok_hw_shim.h" /* BYOK_BTN_WAKE_GPIO, BYOK_POWER_HOLD_GPIO, BYOK_POWER_OFF_LEVEL */
#include "byok_usb_cdc.h" /* byok_usb_cdc_usb_attached() -- stock's USB-power shutdown refusal */

static const char *TAG = "byok_power";

/* 20 ms poll period per the assigned task; DEBOUNCE_POLLS requires the raw
 * level to read the same value on this many consecutive polls (40 ms) before
 * it is trusted as a real press/release edge -- cheap RC-free debounce for a
 * mechanical switch on a floating (externally pulled-up) input. */
#define POLL_MS            20u
#define DEBOUNCE_POLLS     2u
#define HOLD_OFF_MS        3000u /* stock's own threshold, hw_config.h Sec.9 */
/* A release is only logged as a "short press" if it lasted at least this
 * long -- filters a single already-debounced 40 ms blip (contact chatter
 * beyond what DEBOUNCE_POLLS alone catches) from generating a log line for
 * what was arguably not a deliberate press at all. */
#define SHORT_PRESS_MIN_MS 60u
/* Ceiling on byok_power_do_shutdown()'s wait-for-release loop: if WAKE is
 * still reading pressed after this long (stuck switch, debris, wiring
 * fault, or a debounce edge case that never sees two consecutive released
 * polls), cut power anyway rather than spin forever with no escape hatch.
 * Generous relative to a normal finger-lift (well under 1 s in practice) so
 * it never fires on the intended path. */
#define RELEASE_WAIT_TIMEOUT_MS 30000u
/* Stock's own pre-cut backlight fade, reproduced exactly (2026-09-03,
 * docs/troubleshooting.md §4): BYOK.bin 1.1.0
 * file 0x0B6796..0x0B679B calls the 'BACKLIGHT' module's setter with
 * (level=0, fade_ms=500, arg3=0), which lands on ledc_set_fade_with_time()
 * + ledc_fade_start(). Stock does NOT call ledc_stop() and does NOT send
 * the panel any power-down opcode -- the fade is the whole of its display
 * power-down, and byok_display_set_backlight() below is this driver's own
 * equivalent (also an LEDC hardware fade, LEDC_FADE_NO_WAIT). */
#define SHUTDOWN_BACKLIGHT_FADE_MS 500u
/* Stock's settle delay at the end of its pre-cut quiesce routine (file
 * 0x0B6766..0x0B676C: movi a10,100 / vTaskDelay), immediately before
 * gpio_set_level(42,1) at 0x0B68AA. */
#define SHUTDOWN_SETTLE_MS 100u
/* How long the "ON USB POWER" refusal message stays on-glass before this
 * task falls back through to normal polling. Stock does not draw anything
 * for its own refusal (BYOK.bin 1.1.0 file 0x0B75E8..0x0B7636 only logs and
 * posts a "not a power off" event -- no LuaManager/screen call on that
 * path) -- this on-glass message is this project's own addition, so the
 * owner gets feedback even without a serial console attached. 1.5 s is long
 * enough to read, short enough not to feel stuck. */
#define USB_REFUSAL_MSG_MS 1500u

static volatile bool s_display_ready;

/* 0.1.12: framebuffer save/restore around the USB-power-refusal message.
 * On-glass testing found the message stayed on the panel instead of
 * restoring the previous screen -- cosmetic, but worth closing
 * (docs/testing.md Sec.9). A
 * static buffer, not a stack local -- this task's own stack budget
 * (BYOK_DISPLAY_FB_BYTES == 2400 B, see byok_power_init()'s stack-audit
 * comment) was sized without a buffer anywhere near this large, and there
 * is exactly one in-flight USB refusal at a time (this function runs on
 * byok_power_button_task alone, never re-entered -- see that task's own
 * shutdown_triggered latch). `s_usb_refusal_fb_valid` guards the restore:
 * byok_display_save_framebuffer() only fails if the display was never
 * initialised (display_ready already gates the call site) or the size
 * doesn't match (impossible, both sides use sizeof(s_usb_refusal_fb)) --
 * kept anyway rather than assumed, so a future signature change fails
 * loudly (skips the restore) instead of copying stale/uninitialised bytes
 * onto the panel. */
static uint8_t s_usb_refusal_fb[BYOK_DISPLAY_FB_BYTES];
static bool    s_usb_refusal_fb_valid;

void byok_power_set_display_ready(bool ready)
{
    s_display_ready = ready;
}

/* Draws "POWERING OFF" centered on the panel, if (and only if) the display
 * is known-initialised. byok_display.h's own thread-safety note (revised
 * 2026-09-03) documents that byok_display now takes an internal mutex
 * around every public draw/refresh entry point precisely so a call from
 * this task (byok_power_btn, prio 4) cannot interleave an I2C transaction
 * with a concurrent call from app_main's dispatch_task (prio 5, the
 * display's normal owner) -- see byok_display.c's s_display_mutex comment
 * for the full contract. This function itself does NOT wrap the three
 * calls below in anything of its own any more.
 *
 * REVERTED 2026-09-03 (docs/troubleshooting.md): this
 * used to wrap byok_display_clear()/_draw_text()/_full_refresh() in
 * vTaskSuspendAll()/xTaskResumeAll() to get the same "don't interleave with
 * dispatch_task" property the mutex above now gives for free. That wrapper
 * was a worse bug than the one it was trying to prevent: byok_display_full_
 * refresh() calls down to i2c_master_transmit(..., BYOK_LCD_I2C_TIMEOUT_MS)
 * (a NONZERO timeout), and FreeRTOS's xQueueSemaphoreTake() -- what
 * xSemaphoreTake() always resolves to, semphr.h -- unconditionally
 * configASSERTs when called with a nonzero timeout while the scheduler is
 * suspended (queue.c, checked at four separate call sites, not a corner
 * case). That assert fired on the very first I2C byte this handler ever
 * tried to send, EVERY WAKE-hold shutdown attempt, regardless of USB vs.
 * battery power -- turning a working power-off gesture into a guaranteed
 * abort()/panic-reboot ~3-5 s after the panic handler's own reboot delay.
 * See docs/troubleshooting.md Sec.5 for the full trace; the mutex above is
 * the real fix.
 *
 * The message text dropped its old "- RELEASE" suffix (added alongside the
 * wait-for-release loop below, back when this handler ran the wait AFTER
 * this draw and BEFORE the backlight fade -- same call order as now, the
 * suffix was purely a UI wording choice) -- plain "POWERING OFF" per the
 * 0.1.10 stock-parity pass. */
static void draw_powering_off(void)
{
    if (!s_display_ready) {
        return;
    }
    static const char msg[] = "POWERING OFF";
    const uint16_t msg_w = (uint16_t)(sizeof(msg) - 1) * 8u; /* byok_font8x8: 8px advance/glyph */
    uint16_t x = (BYOK_DISPLAY_W > msg_w) ? (uint16_t)((BYOK_DISPLAY_W - msg_w) / 2) : 0;
    uint16_t y = (BYOK_DISPLAY_H > 8u) ? (uint16_t)((BYOK_DISPLAY_H - 8u) / 2) : 0;
    byok_display_clear(false);
    byok_display_draw_text(x, y, 0, msg, sizeof(msg) - 1);
    byok_display_full_refresh(false);
}

/* Draws stock's own "On USB power, can't power off" refusal
 * (docs/troubleshooting.md §4, BYOK.bin 1.1.0
 * file 0x0B75AF/0x0B75B4/0x0B75F1) on-glass, as two centered lines (the
 * full sentence is wider than the 240 px panel at 8 px/glyph). Same
 * display-ready guard and unlocked-call shape as draw_powering_off() above
 * -- byok_display's own internal mutex (see that function's comment) is
 * what makes this safe against dispatch_task now, not anything here.
 *
 * 0.1.12: saves the framebuffer first (byok_display_save_framebuffer(),
 * into s_usb_refusal_fb) so byok_power_do_shutdown() can put whatever was
 * on the glass back afterwards -- previously this message permanently
 * replaced it (the cosmetic gap noted above). The save
 * happens before byok_display_clear() below overwrites the very buffer
 * being saved, obviously -- ordering matters here, not just presence. */
static void draw_usb_refusal(void)
{
    if (!s_display_ready) {
        return;
    }
    s_usb_refusal_fb_valid = byok_display_save_framebuffer(s_usb_refusal_fb, sizeof(s_usb_refusal_fb));
    if (!s_usb_refusal_fb_valid) {
        ESP_LOGW(TAG, "byok_display_save_framebuffer failed -- the previous screen will not be "
                      "restored after this USB-power-refusal message");
    }
    static const char line1[] = "ON USB POWER";
    static const char line2[] = "CAN'T POWER OFF";
    const uint16_t line1_w = (uint16_t)(sizeof(line1) - 1) * 8u;
    const uint16_t line2_w = (uint16_t)(sizeof(line2) - 1) * 8u;
    uint16_t x1 = (BYOK_DISPLAY_W > line1_w) ? (uint16_t)((BYOK_DISPLAY_W - line1_w) / 2) : 0;
    uint16_t x2 = (BYOK_DISPLAY_W > line2_w) ? (uint16_t)((BYOK_DISPLAY_W - line2_w) / 2) : 0;
    uint16_t y1 = (BYOK_DISPLAY_H > 16u) ? (uint16_t)((BYOK_DISPLAY_H - 16u) / 2) : 0;
    uint16_t y2 = (uint16_t)(y1 + 8u);
    byok_display_clear(false);
    byok_display_draw_text(x1, y1, 0, line1, sizeof(line1) - 1);
    byok_display_draw_text(x2, y2, 0, line2, sizeof(line2) - 1);
    byok_display_full_refresh(false);
}

/* Reproduces stock's final GPIO42-HIGH-then-spin instructions
 * (stock image file offsets 0x0B68AA..0x0B68BD; hw_config.h Sec.9): log, draw the on-glass message if we can, drive
 * GPIO42 HIGH (BYOK_POWER_OFF_LEVEL), then spin forever on a 100 ms
 * vTaskDelay -- never returns on the power-off path. The surrounding event
 * path (stock enters this via a POWER_OFF_REQUEST event) is NOT reproduced --
 * this component calls straight in on its own 3+ s hold detection.
 *
 * 0.1.10 stock parity (docs/troubleshooting.md §4 and §5): two
 * pieces of stock behaviour added this release, in this order --
 *
 *   1. USB-power refusal. Stock's WAKE-hold handler (BYOK.bin 1.1.0, file
 *      0x0B7558) calls is_usb_powered() at file 0x0B75AF and, on a non-zero
 *      result, branches at 0x0B75B4 to log "On USB power, can't power off"
 *      and post a *different* queue mode that never reaches the power cut.
 *      byok_power.c previously declined to reproduce this on the grounds
 *      that "no VBUS/TUSB320-attach source exists anywhere in this tree" --
 *      true when written, stale since byok_usb_cdc.c grew a TUSB320 attach
 *      poller and (2026-09-03) byok_usb_cdc_usb_attached(), a fresh-read
 *      wrapper around it. Checked FIRST, before anything else below,
 *      because dropping the GPIO42 latch with VBUS live does not keep the
 *      device running the way byok_power.h's old comment assumed -- it
 *      drops the rail, browns the S3 out (which releases GPIO42, since
 *      nothing holds it, same as stock), and the still-present VBUS
 *      re-establishes the rail and the board cold-boots (POWERON/EN,
 *      observed on battery-absent 0.1.6/0.1.8 testing before this fix).
 *      Fails OPEN on an unreadable USB state, deliberately: refusing to cut
 *      when we merely *cannot tell* would reintroduce 0.1.0's "device
 *      cannot be switched off on battery" defect, which is strictly worse
 *      than an unwanted restart while attached to USB. Logged either way.
 *
 *   2. Backlight fade + settle delay. Stock's own pre-cut quiesce
 *      (file 0x0B6791..0x0B679B and 0x0B6714..0x0B6772) fades the
 *      backlight to 0 over 500 ms and, right before the cut, settles 100 ms
 *      -- see SHUTDOWN_BACKLIGHT_FADE_MS/SHUTDOWN_SETTLE_MS above for the
 *      exact offsets. byok-mod has no cloud sync, no NVS writes pending, no
 *      long-lived SD mount and registers no GPIO ISR handlers, so of
 *      stock's quiesce only these two steps have an analogue here.
 *
 * Device observation, 2026-09-03: WAKE (GPIO6) is the SAME physical button
 * used to power the unit ON. Cutting power (GPIO42 HIGH) the instant the 3 s
 * threshold is reached -- as this function used to, with no wait -- does so
 * while the owner's finger is still ON the button; the external power-hold
 * circuit drops, and the still-pressed button immediately looks exactly
 * like a power-ON press again, so the device just reboots instead of
 * turning off. Stock evidently avoids this (the button-hold-to-power-off
 * gesture visibly works on the vendor firmware); the fix here is the
 * obvious one -- do not drive the latch until WAKE reads released. This
 * waits, debounced the same way byok_power_button_task()'s own outer poll
 * does (POLL_MS/DEBOUNCE_POLLS), for GPIO6 to read released before touching
 * GPIO42 at all. This is bounded by RELEASE_WAIT_TIMEOUT_MS: a stuck switch,
 * debris, or a wiring fault could otherwise leave GPIO6 reading pressed
 * forever, and this task spinning forever with GPIO42 never driven would
 * mean the WAKE button can never power the device off again short of
 * pulling the battery. 30 s is far beyond any real finger-lift, so the
 * normal path (release within ~1 s) is unaffected; only a genuine stuck-
 * button fault ever reaches the timeout, and it is logged when it does.
 * This wait-for-release is itself a DELIBERATE DIVERGENCE FROM STOCK (stock
 * posts its power-off request the instant the hold deadline expires, button
 * still down, and cuts without ever consulting release -- see
 * docs/troubleshooting.md Sec.4 for the power-off-restart symptom this
 * shutdown path was rebuilt around, and why the wait is kept anyway:
 * bounded, logged, and it cannot itself re-power the board). */
static void byok_power_do_shutdown(void)
{
    bool on_usb = false;
    esp_err_t usb_err = byok_usb_cdc_usb_attached(&on_usb);
    if (usb_err == ESP_OK && on_usb) {
        /* Stock's own log text (BYOK.bin 1.1.0 file 0x0B75F1), reproduced
         * verbatim so a serial capture reads the same as stock's. */
        ESP_LOGW(TAG, "On USB power, can't power off");
        draw_usb_refusal();
        vTaskDelay(pdMS_TO_TICKS(USB_REFUSAL_MSG_MS));
        /* 0.1.12: restore whatever was on the glass before the refusal
         * message (see draw_usb_refusal()'s own comment) -- the copy the
         * mutex-protected byok_display_restore_framebuffer() writes back
         * into the live back buffer, then one full refresh to actually push
         * it to the panel (restore alone only touches the back buffer, per
         * that function's own "nothing appears until a refresh" contract).
         * s_usb_refusal_fb_valid stays false (skipping this) if the save
         * itself failed -- restoring an unsaved/stale buffer would be worse
         * than just leaving the refusal message up. */
        if (s_display_ready && s_usb_refusal_fb_valid) {
            byok_display_restore_framebuffer(s_usb_refusal_fb, sizeof(s_usb_refusal_fb));
            byok_display_full_refresh(false);
        }
        return; /* refuse -- stay running, exactly like stock */
    }
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "USB attach state unreadable (%s) -- proceeding with power off anyway",
                 esp_err_to_name(usb_err));
    }

    ESP_LOGW(TAG, "Wake button held for 3+ seconds -- waiting for release before cutting power");

    draw_powering_off();

    /* Stock parity: fade the backlight to 0 over 500 ms right after the
     * draw above, same position stock's own pre-cut quiesce puts it in
     * relative to its own shutdown-screen draw (file 0x0B6791 then
     * 0x0B6796) -- see SHUTDOWN_BACKLIGHT_FADE_MS's own comment. Skipped
     * (same guard as draw_powering_off()) when the display was never
     * brought up: byok_display_set_backlight() has no internal init-state
     * guard of its own. */
    if (s_display_ready) {
        (void)byok_display_set_backlight(0, (uint16_t)SHUTDOWN_BACKLIGHT_FADE_MS);
        vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_BACKLIGHT_FADE_MS));
    }

    /* Best-effort flush before the point of no return: give the log/UART
     * (and, if it just ran, the display's I2C write) a moment to actually
     * leave the chip rather than racing the release-wait loop immediately
     * below. ESP_LOG* itself is synchronous over the UART/USJ console in
     * this firmware's configuration (no separate log-flush call exists in
     * IDF for that path), so this is a small fixed margin, not a real flush
     * primitive -- named/commented as "best-effort" rather than "flush()"
     * so nobody mistakes it for a guarantee. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Wait for the debounced release -- see this function's own header
     * comment for why. Same POLL_MS/DEBOUNCE_POLLS constants as the outer
     * poll loop, applied fresh here (not shared state with it): this loop
     * only cares about a clean run of "released" reads from this point
     * forward, not whatever run was in progress when the 3 s threshold
     * fired. Bounded by RELEASE_WAIT_TIMEOUT_MS (see header comment above)
     * so a stuck button cannot spin this task forever. */
    unsigned released_run = 0;
    uint32_t waited_ms = 0;
    bool timed_out = false;
    while (released_run < DEBOUNCE_POLLS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        waited_ms += POLL_MS;
        if (gpio_get_level(BYOK_BTN_WAKE_GPIO) != 0) { /* active-low: nonzero = released */
            released_run++;
        } else {
            released_run = 0; /* still pressed (or bounced back to pressed) -- reset the run */
        }
        if (waited_ms >= RELEASE_WAIT_TIMEOUT_MS) {
            timed_out = true;
            break;
        }
    }
    if (timed_out) {
        ESP_LOGW(TAG, "WAKE release-wait timed out (%" PRIu32 " ms) -- cutting power anyway",
                 waited_ms);
    } else {
        ESP_LOGW(TAG, "WAKE released -- cutting power now");
    }

    /* Stock's settle delay, immediately before the cut -- see
     * SHUTDOWN_SETTLE_MS's own comment. */
    vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_SETTLE_MS));

    gpio_set_level(BYOK_POWER_HOLD_GPIO, BYOK_POWER_OFF_LEVEL);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100)); /* spin forever -- matches stock's own shutdown loop exactly */
    }
}

static void byok_power_button_task(void *arg)
{
    (void)arg;

    /* Debounce state: a run of `raw_run_count` consecutive polls that all
     * read `raw_run_level`; once that run reaches DEBOUNCE_POLLS, `raw_run_
     * level` is trusted and (if it differs from the last trusted level)
     * treated as a real edge. `held_ms` only accumulates while the TRUSTED
     * level reads pressed, so a not-yet-debounced glitch can't inflate the
     * hold timer.
     *
     * `armed` guards against the task starting mid-press: WAKE is the same
     * front button used to power the unit ON, so this task can start with
     * WAKE already held (the user is still holding it from powering on).
     * Without `armed`, seeding trusted_level=1 (released) would read that
     * as a fresh press edge and start counting immediately, powering the
     * device back off ~3 s after an intentional power-on. `armed` starts
     * false and is set true only the first time a debounced RELEASE is
     * observed, so held_ms cannot accumulate until WAKE has been seen
     * released at least once after task start. */
    int trusted_level = -1;  /* unknown until the first debounced read */
    int raw_run_level = -1;
    unsigned raw_run_count = 0;
    uint32_t held_ms = 0;
    bool shutdown_triggered = false;
    bool armed = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int level = gpio_get_level(BYOK_BTN_WAKE_GPIO);

        if (level == raw_run_level) {
            if (raw_run_count < DEBOUNCE_POLLS) {
                raw_run_count++;
            }
        } else {
            raw_run_level = level;
            raw_run_count = 1;
        }

        if (raw_run_count >= DEBOUNCE_POLLS && trusted_level != raw_run_level) {
            trusted_level = raw_run_level;
            if (trusted_level == 0) {
                /* debounced press edge (real only if `armed`; see above) */
                held_ms = 0;
                shutdown_triggered = false;
            } else {
                /* debounced release edge -- including the very first
                 * debounced read of an already-released button, which is
                 * what arms the task in the common (not-mid-press) case */
                armed = true;
                if (held_ms >= SHORT_PRESS_MIN_MS && held_ms < HOLD_OFF_MS) {
                    ESP_LOGI(TAG, "WAKE short press (%" PRIu32 " ms) -- no-op", held_ms);
                }
                held_ms = 0;
            }
        }

        if (armed && trusted_level == 0 && !shutdown_triggered) {
            held_ms += POLL_MS;
            if (held_ms >= HOLD_OFF_MS) {
                shutdown_triggered = true;
                /* 0.1.10: no longer strictly "never returns" -- the new
                 * USB-power refusal path inside byok_power_do_shutdown()
                 * returns instead of cutting power (see that function's own
                 * header comment). shutdown_triggered stays true either way
                 * until the next debounced PRESS edge above resets it, so a
                 * refusal cannot re-trigger every poll -- release and
                 * press WAKE again to retry, same as stock requires. Every
                 * other path through byok_power_do_shutdown() still never
                 * returns (it ends by driving GPIO42 and spinning). */
                byok_power_do_shutdown();
            }
        }
    }
}

esp_err_t byok_power_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BYOK_BTN_WAKE_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* GPIO_FLOATING per hw_config.h Sec.8 (BYOK_BTN_INTERNAL_PULLUP == 0,
         * BYOK_BTN_INTERNAL_PULLDOWN == 0: the board supplies its own
         * external pull-up on this net -- matching stock's own buttons_init
         * call exactly, not a guess). */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE, /* polled, matching stock's own button_task */
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(WAKE/GPIO%d) failed: %s (power-off handling disabled)",
                 (int)BYOK_BTN_WAKE_GPIO, esp_err_to_name(err));
        /* Not fatal to boot -- see byok_power.h. A device that can't
         * configure the WAKE input still boots and works over USB; it just
         * can't be button-power-cycled, which is diagnosable and
         * recoverable (unplug), unlike a boot-time abort. But the poll task
         * must NOT be started in this case: gpio_get_level() on an
         * unconfigured pad is not guaranteed to read released, and a
         * phantom "held" reading would silently shut the device down ~3 s
         * after boot -- worse than just not having the feature. */
        return err;
    }

    /* Stack audit, 2026-09-03 (docs/troubleshooting.md
     * prompted a pass over every task this project creates). Largest
     * locals on byok_power_button_task()'s own frame are a handful of
     * int/uint32_t/bool debounce-state variables (~24 B total) -- trivial.
     * The call chain that matters is byok_power_button_task() ->
     * byok_power_do_shutdown() -> draw_powering_off()/draw_usb_refusal() ->
     * byok_display_clear()/_draw_text()/_full_refresh() -> flush_rect() ->
     * window_program()/lcd_send_bulk_data() -> i2c_master_transmit()
     * (byok_display.c has no large locals of its own on this path either --
     * s_fb is `static`), i.e. this task, not "byok_dispatch", is the one
     * that actually calls all the way down through the I2C master driver.
     * The original 3072 B was the bare `xTaskCreate` figure carried over
     * from before this driver's typical internal stack use (its new-style
     * i2c_master_transmit() call chain, plus this file's own ESP_LOGW/I
     * format-string frames) was ever weighed against it -- margin-by-
     * inspection only, no uxTaskGetStackHighWaterMark() data exists for
     * this task, but 3072 B left too little headroom over that chain to be
     * confident it isn't marginal, and this task IS the WAKE-hold power-off
     * escape hatch (byok_power.h) -- a silent overflow here would be worse
     * than most, so raised to 4096 B rather than leaving it as a judgment
     * call.
     *
     * 0.1.10 re-check: byok_power_do_shutdown() now also calls
     * byok_usb_cdc_usb_attached() -> tusb320_read_attach_state() ->
     * i2c_master_transmit_receive() FIRST, before any display call -- a
     * call chain of the same depth/shape as byok_display's own (a handful
     * of uint8_t/esp_err_t locals down to the same new-style I2C master
     * driver, per byok_usb_cdc.c's own stack-audit comment on that
     * function), and byok_display_set_backlight() (LEDC calls, shallow,
     * shallower than the I2C draw chain it runs alongside). Neither adds a
     * new deepest path -- the display draw+refresh chain above remains the
     * tallest frame -- so 4096 B is unchanged and still has the same
     * margin-by-inspection headroom this comment already documents. */
    BaseType_t created = xTaskCreate(byok_power_button_task, "byok_power_btn", 4096, NULL, 4, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(byok_power_btn) failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
