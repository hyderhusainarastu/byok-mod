# SPDX-License-Identifier: MIT
# byok_rtc -- PCF8563 real-time clock driver (hw_config.h Sec.2, I2C BUS 1
# via byok_sysbus). See include/byok_rtc.h for the register-map evidence
# grading.
idf_component_register(
    SRCS "byok_rtc.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "byok_hw_shim" "byok_sysbus" "driver" "freertos" "log"
)
