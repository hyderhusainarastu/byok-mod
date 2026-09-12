# SPDX-License-Identifier: MIT
# byok_sysbus -- shared I2C BUS 1 handle (TUSB320 + PCF8563, hw_config.h Sec.2)
idf_component_register(
    SRCS "byok_sysbus.c"
    INCLUDE_DIRS "include"
    REQUIRES "driver"
    PRIV_REQUIRES "byok_hw_shim" "log"
)
