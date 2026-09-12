/* SPDX-License-Identifier: MIT */
#include "byok_modes.h"

#include <stddef.h>

typedef struct {
    byok_modes_change_cb_t cb;
    void *ctx;
} cb_slot_t;

static byok_app_mode_t s_mode = BYOK_MODE_DASHBOARD_USB;
static cb_slot_t s_cbs[BYOK_MODES_MAX_CALLBACKS];

esp_err_t byok_modes_init(byok_app_mode_t initial)
{
    if (initial >= BYOK_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mode = initial;
    for (size_t i = 0; i < BYOK_MODES_MAX_CALLBACKS; i++) {
        s_cbs[i].cb = NULL;
        s_cbs[i].ctx = NULL;
    }
    return ESP_OK;
}

byok_app_mode_t byok_modes_get(void)
{
    return s_mode;
}

esp_err_t byok_modes_set(byok_app_mode_t mode)
{
    if (mode >= BYOK_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    byok_app_mode_t old = s_mode;
    s_mode = mode;
    if (old != mode) {
        /* Snapshot nothing extra here: a callback that itself calls
         * byok_modes_set() re-enters this function (seen today only in the
         * accepted-not-solved race byok_clock.c's clock_mode_changed()
         * already documents) -- s_cbs is only ever appended/cleared from
         * init-time registration, never mutated by a callback in this
         * tree, so a plain forward scan is safe against that re-entry. */
        for (size_t i = 0; i < BYOK_MODES_MAX_CALLBACKS; i++) {
            if (s_cbs[i].cb != NULL) {
                s_cbs[i].cb(mode, old, s_cbs[i].ctx);
            }
        }
    }
    return ESP_OK;
}

esp_err_t byok_modes_register_change_cb(byok_modes_change_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < BYOK_MODES_MAX_CALLBACKS; i++) {
        if (s_cbs[i].cb == NULL) {
            s_cbs[i].cb = cb;
            s_cbs[i].ctx = ctx;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

void byok_modes_unregister_change_cb(byok_modes_change_cb_t cb)
{
    if (cb == NULL) {
        return;
    }
    for (size_t i = 0; i < BYOK_MODES_MAX_CALLBACKS; i++) {
        if (s_cbs[i].cb == cb) {
            s_cbs[i].cb = NULL;
            s_cbs[i].ctx = NULL;
            return;
        }
    }
}

const char *byok_modes_name(byok_app_mode_t mode)
{
    switch (mode) {
    case BYOK_MODE_DASHBOARD_USB: return "DASHBOARD_USB";
    case BYOK_MODE_CLOCK:         return "CLOCK";
    case BYOK_MODE_STATIC_NOTE:   return "STATIC_NOTE";
    case BYOK_MODE_ORIGINAL:      return "ORIGINAL";
    case BYOK_MODE_BLANK:         return "BLANK";
    case BYOK_MODE_MENU:          return "MENU";
    default:                      return "?";
    }
}
