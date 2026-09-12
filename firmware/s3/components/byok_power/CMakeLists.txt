# SPDX-License-Identifier: MIT
# byok_power -- WAKE (GPIO6) button-hold power-off handler (see byok_power.h)
# 2026-09-03: "byok_usb_cdc" added for byok_usb_cdc_usb_attached() -- the
# stock USB-power shutdown refusal (docs/troubleshooting.md, "Device won't
# stay off -- restarts on its own after a WAKE-hold power-off (USB
# connected)").
# Checked: byok_usb_cdc's own PRIV_REQUIRES does not list byok_power, so
# this is not circular.
idf_component_register(
    SRCS "byok_power.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "byok_hw_shim" "byok_display" "byok_usb_cdc" "driver" "freertos" "log" "esp_common"
)
