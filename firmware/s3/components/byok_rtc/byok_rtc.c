/* SPDX-License-Identifier: MIT */
#include "byok_rtc.h"

#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h" /* pdMS_TO_TICKS */

#include "byok_hw_shim.h" /* BYOK_PCF8563_I2C_ADDR/_SPEED_HZ -- hw_config.h Sec.2 */
#include "byok_sysbus.h"

static const char *TAG = "byok_rtc";

#define PCF8563_REG_SECONDS 0x02u /* first register of the 7-byte time burst */
#define PCF8563_BURST_LEN   7u    /* 0x02..0x08 inclusive */
#define PCF8563_VL_BIT      0x80u /* register 0x02 bit7 */
#define PCF8563_CENTURY_BIT 0x80u /* register 0x07 bit7 */
#define PCF8563_I2C_TIMEOUT_MS 1000

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static inline uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0Fu) * 10u + (bcd & 0x0Fu));
}

static inline uint8_t bin_to_bcd(uint8_t bin)
{
    return (uint8_t)((((bin / 10u) & 0x0Fu) << 4) | (bin % 10u));
}

esp_err_t byok_rtc_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = byok_sysbus_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "byok_sysbus_init failed: %s -- CLOCK mode's RTC read/SET_TIME "
                      "will be unavailable", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BYOK_PCF8563_I2C_ADDR,
        .scl_speed_hz = BYOK_PCF8563_I2C_SPEED_HZ,
    };
    err = i2c_master_bus_add_device(byok_sysbus_get_handle(), &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_bus_add_device(PCF8563) failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    return ESP_OK;
}

bool byok_rtc_is_ready(void)
{
    return s_ready;
}

esp_err_t byok_rtc_read(byok_rtc_time_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg = PCF8563_REG_SECONDS;
    uint8_t buf[PCF8563_BURST_LEN] = { 0 };
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf),
                                                 pdMS_TO_TICKS(PCF8563_I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));
    out->voltage_low = (buf[0] & PCF8563_VL_BIT) != 0;
    out->second  = bcd_to_bin((uint8_t)(buf[0] & 0x7Fu));
    out->minute  = bcd_to_bin((uint8_t)(buf[1] & 0x7Fu));
    out->hour    = bcd_to_bin((uint8_t)(buf[2] & 0x3Fu));
    out->day     = bcd_to_bin((uint8_t)(buf[3] & 0x3Fu));
    out->weekday = (uint8_t)(buf[4] & 0x07u);
    out->month   = bcd_to_bin((uint8_t)(buf[5] & 0x1Fu));
    bool century = (buf[5] & PCF8563_CENTURY_BIT) != 0;
    out->year    = (uint16_t)((century ? 2100 : 2000) + bcd_to_bin(buf[6]));
    return ESP_OK;
}

esp_err_t byok_rtc_write(const byok_rtc_time_t *t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->weekday > 6 || t->hour > 23 || t->minute > 59 || t->second > 59 ||
        t->year < 2000 || t->year > 2199) {
        return ESP_ERR_INVALID_ARG;
    }

    bool century = t->year >= 2100;
    uint16_t yy = (uint16_t)(t->year - (century ? 2100 : 2000));

    uint8_t payload[1 + PCF8563_BURST_LEN];
    payload[0] = PCF8563_REG_SECONDS;
    payload[1] = bin_to_bcd(t->second);              /* VL bit cleared -- bit7 left 0 */
    payload[2] = bin_to_bcd(t->minute);
    payload[3] = bin_to_bcd(t->hour);
    payload[4] = bin_to_bcd(t->day);
    payload[5] = (uint8_t)(t->weekday & 0x07u);
    payload[6] = (uint8_t)(bin_to_bcd(t->month) | (century ? PCF8563_CENTURY_BIT : 0));
    payload[7] = bin_to_bcd((uint8_t)yy);

    return i2c_master_transmit(s_dev, payload, sizeof(payload),
                                pdMS_TO_TICKS(PCF8563_I2C_TIMEOUT_MS));
}
