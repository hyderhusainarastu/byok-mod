# SPDX-License-Identifier: MIT
# byok_clock -- device-local CLOCK mode rendering (byok_modes BYOK_MODE_CLOCK,
# PCF8563 via byok_rtc). See include/byok_clock.h for the design.
idf_component_register(
    SRCS "byok_clock.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "byok_battery" "byok_display" "byok_hw_shim" "byok_idle" "byok_modes" "byok_rtc"
                  "esp_timer" "freertos" "log"
)
