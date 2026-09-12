# SPDX-License-Identifier: MIT
# byok_idle -- UP/DOWN/EXECUTE/BRIGHTNESS back-button idle-submode +
# backlight cycling (byok_modes BYOK_MODE_CLOCK/STATIC_NOTE/BLANK). See
# include/byok_idle.h.
idf_component_register(
    SRCS "byok_idle.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "byok_display" "byok_hw_shim" "byok_menu" "byok_modes" "byok_note" "byok_nvs"
                  "byok_proto" "driver" "freertos" "log" "esp_timer"
)
