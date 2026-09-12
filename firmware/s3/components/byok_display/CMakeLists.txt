# SPDX-License-Identifier: MIT
# byok_display -- UC1611-class panel driver + LEDC backlight (hw_config.h Sec.3/4)
# "byok_battery" (0.1.11): byok_display_selftest.c's resting-screen phase
# reads the live battery reading to draw it on the glass -- see that file's
# own comment at the include. Not circular: byok_battery does not depend on
# byok_display.
idf_component_register(
    SRCS "byok_display.c" "byok_display_selftest.c"
    INCLUDE_DIRS "include"
    REQUIRES "driver"
    PRIV_REQUIRES "byok_hw_shim" "byok_battery" "log" "freertos" "esp_common" "esp_timer"
)
