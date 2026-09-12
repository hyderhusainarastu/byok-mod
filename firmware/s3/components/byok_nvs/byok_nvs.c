/* SPDX-License-Identifier: MIT */
#include "byok_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sdkconfig.h"

static const char *TAG = "byok_nvs";

static bool          s_init_attempted;
static bool          s_armed;
static nvs_handle_t  s_handle;

esp_err_t byok_nvs_init(void)
{
    if (s_init_attempted) {
        return s_armed ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_init_attempted = true;

#if CONFIG_BYOK_NVS_PERSIST_ARMED
    /* Deliberately NOT the common ESP-IDF idiom of erase-then-retry on
     * ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND -- this
     * partition is shared with the vendor's own data (docs/protocol.md
     * Sec.10, this header's own top comment) and SAFETY.md §1 forbids
     * ever erasing NVS on our own initiative. Any non-OK result here just
     * disables persistence for the rest of this boot. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s -- persistence disabled this boot "
                      "(byok_nvs deliberately never calls nvs_flash_erase(), "
                      "see byok_nvs.h)", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = nvs_open(BYOK_NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(\"%s\"): %s -- persistence disabled this boot",
                 BYOK_NVS_NAMESPACE, esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    s_armed = true;
    ESP_LOGI(TAG, "namespace \"%s\" open -- SET_NOTE/idle-mode/backlight-level "
                  "persistence armed (CONFIG_BYOK_NVS_PERSIST_ARMED=y)",
             BYOK_NVS_NAMESPACE);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "CONFIG_BYOK_NVS_PERSIST_ARMED is OFF (default) -- "
                  "note/idle-mode/backlight-level are in-RAM only this boot, "
                  "see byok_nvs.h and this component's Kconfig");
    return ESP_ERR_INVALID_STATE;
#endif
}

bool byok_nvs_is_armed(void)
{
    return s_armed;
}

esp_err_t byok_nvs_get_note(char *out, size_t out_cap, size_t *out_len)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Stage into a local buffer sized for the full BYOK_NVS_NOTE_MAX_LEN +
     * NUL, mirroring byok_nvs_set_note()'s staging on the way in --
     * nvs_get_str() requires the destination capacity to cover the stored
     * string's length PLUS the NUL terminator it always writes, but
     * callers size `out`/`out_cap` to exactly BYOK_NVS_NOTE_MAX_LEN (an
     * explicit-byte-length buffer, not a C string buffer). Without this
     * staging, a note at the full 200-byte cap would need a 201-byte
     * caller buffer to read back and would otherwise fail with
     * ESP_ERR_NVS_INVALID_LENGTH. */
    char   staged[BYOK_NVS_NOTE_MAX_LEN + 1];
    size_t required = sizeof(staged);
    esp_err_t err = nvs_get_str(s_handle, BYOK_NVS_KEY_NOTE, staged, &required);
    if (err != ESP_OK) {
        *out_len = 0;
        return err;
    }
    /* nvs_get_str()'s `required` includes the NUL terminator it always
     * writes; byok_note.c's own convention is an explicit byte length, not
     * a C string, so report the length excluding that trailing NUL (it
     * cannot be 0 here -- nvs_set_str() below always writes at least the
     * terminator). */
    size_t len = (required > 0) ? (required - 1) : 0;
    if (len > out_cap) {
        /* Should not happen -- staged is capped at BYOK_NVS_NOTE_MAX_LEN
         * bytes of payload and byok_nvs_set_note() enforces the same cap
         * on the way in -- but guard against a caller-supplied out_cap
         * smaller than BYOK_NVS_NOTE_MAX_LEN rather than overflow `out`. */
        *out_len = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, staged, len);
    *out_len = len;
    return ESP_OK;
}

esp_err_t byok_nvs_set_note(const char *note, size_t len)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (note == NULL || len > BYOK_NVS_NOTE_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    /* nvs_set_str() wants a NUL-terminated C string; stage into a fixed
     * local (not the caller's buffer, which is not guaranteed to have room
     * for the terminator) since BYOK_NVS_NOTE_MAX_LEN is small and this is
     * called only from the dispatch task, never from an ISR/ interrupt. */
    char staged[BYOK_NVS_NOTE_MAX_LEN + 1];
    memcpy(staged, note, len);
    staged[len] = '\0';

    esp_err_t err = nvs_set_str(s_handle, BYOK_NVS_KEY_NOTE, staged);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_str(note): %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit(note): %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t byok_nvs_get_idle_mode(uint8_t *out)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_u8(s_handle, BYOK_NVS_KEY_IDLE_MODE, out);
}

esp_err_t byok_nvs_set_idle_mode(uint8_t mode)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_u8(s_handle, BYOK_NVS_KEY_IDLE_MODE, mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_u8(idle_mode): %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit(idle_mode): %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t byok_nvs_get_presets(uint8_t *out, size_t out_cap)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL || out_cap < BYOK_NVS_PRESETS_BLOB_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t required = BYOK_NVS_PRESETS_BLOB_LEN;
    return nvs_get_blob(s_handle, BYOK_NVS_KEY_PRESETS, out, &required);
}

esp_err_t byok_nvs_set_presets(const uint8_t *blob)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_set_blob(s_handle, BYOK_NVS_KEY_PRESETS, blob, BYOK_NVS_PRESETS_BLOB_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_blob(presets): %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit(presets): %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t byok_nvs_get_preset_idx(uint8_t *out)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_u8(s_handle, BYOK_NVS_KEY_PRESET, out);
}

esp_err_t byok_nvs_set_preset_idx(uint8_t idx)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_u8(s_handle, BYOK_NVS_KEY_PRESET, idx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_u8(preset): %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit(preset): %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t byok_nvs_get_backlight_idx(uint8_t *out)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_u8(s_handle, BYOK_NVS_KEY_BACKLIGHT, out);
}

esp_err_t byok_nvs_set_backlight_idx(uint8_t idx)
{
    if (!s_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_u8(s_handle, BYOK_NVS_KEY_BACKLIGHT, idx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_u8(backlight_lvl): %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit(backlight_lvl): %s", esp_err_to_name(err));
    }
    return err;
}
