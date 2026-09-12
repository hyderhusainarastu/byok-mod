/* SPDX-License-Identifier: MIT */
#include "byok_sysbus.h"

#include "esp_log.h"

#include "byok_hw_shim.h" /* BYOK_SYS_I2C_* -- hw_config.h Sec.2 */

static const char *TAG = "byok_sysbus";

static i2c_master_bus_handle_t s_bus;
static bool s_inited;

esp_err_t byok_sysbus_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* Mirrors byok_display.c's bus-0 setup exactly, one port over -- see
     * hw_config.h Sec.2 for the CONFIRMED evidence behind every field here. */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BYOK_SYS_I2C_PORT,
        .sda_io_num = BYOK_SYS_I2C_SDA_GPIO,
        .scl_io_num = BYOK_SYS_I2C_SCL_GPIO,
        .clk_source = BYOK_SYS_I2C_CLK_SRC_XTAL ? I2C_CLK_SRC_XTAL : I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = BYOK_SYS_I2C_GLITCH_IGNORE_CNT,
        .flags.enable_internal_pullup = BYOK_SYS_I2C_INTERNAL_PULLUP, /* 0 -- external pull-ups on this bus */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus(BUS %d) failed: %s", BYOK_SYS_I2C_PORT, esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    return ESP_OK;
}

i2c_master_bus_handle_t byok_sysbus_get_handle(void)
{
    return s_inited ? s_bus : NULL;
}
