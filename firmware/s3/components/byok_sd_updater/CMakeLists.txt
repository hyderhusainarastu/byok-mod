# SPDX-License-Identifier: MIT
# byok_sd_updater -- boot-time /SDCARD/Updates/BYOK.tar scanner (SDMMC 1-bit +
# FATFS + the shared byok_tar ustar parser). See byok_sd_updater.h.
idf_component_register(
    SRCS "byok_sd_updater.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        "byok_tar"
        "byok_hw_shim"
        "fatfs"
        "esp_driver_sdmmc"
        "app_update"
        "esp_partition"
        "bootloader_support"
        "driver"
        "vfs"
        "freertos"
        "log"
        "esp_common"
        "esp_system"
)
