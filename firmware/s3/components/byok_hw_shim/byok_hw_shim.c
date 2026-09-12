/* SPDX-License-Identifier: MIT */
#include "byok_hw_shim.h"

#include "esp_log.h"

static const char *TAG = "byok_hw_shim";

void byok_hw_shim_log_warnings(void)
{
    ESP_LOGW(TAG, "hw_config.h Sec.13 unresolved items are running on PLACEHOLDER values:");
    ESP_LOGW(TAG, "  LCD_FB_BIT_ORDER=%d (%s)     -- unverified on real pixels, may need flipping",
             (int)BYOK_LCD_FB_BIT_ORDER, BYOK_LCD_FB_BIT_ORDER ? "LSB=top row" : "MSB=top row");
    ESP_LOGW(TAG, "  GPIO39/GPIO48 function, LCD cmd 0xC9/0x95/0xE1 meaning -- UNKNOWN, unused/replayed verbatim");
    ESP_LOGW(TAG, "See firmware/s3/README.md \"Guesses\" and firmware/common/hw_config.h Sec.13.");
}
