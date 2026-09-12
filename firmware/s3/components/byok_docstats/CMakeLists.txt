# SPDX-License-Identifier: MIT
# byok_docstats -- bounded /SDCARD/Projects/**/*.txt writing-stats scan
# (SDMMC 1-bit + FATFS, read-only), protocol GET_DOCSTATS/DOCSTATS. See
# include/byok_docstats.h.
idf_component_register(
    SRCS "byok_docstats.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        "byok_hw_shim"
        "byok_modes"
        "byok_rtc"
        "fatfs"
        "esp_driver_sdmmc"
        "driver"
        "vfs"
        "freertos"
        "esp_timer"
        "log"
)
