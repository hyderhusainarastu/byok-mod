# SPDX-License-Identifier: MIT
# byok_menu -- preset-menu overlay (byok_modes BYOK_MODE_MENU, protocol
# SET_PRESETS / SET_MODE value 7 / EVT_PRESET_CHANGED). See
# include/byok_menu.h.
idf_component_register(
    SRCS "byok_menu.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "byok_display" "byok_modes" "byok_nvs" "freertos" "esp_timer" "log"
)
