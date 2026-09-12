# SPDX-License-Identifier: MIT
# ==============================================================================
# BYOK Mod -- ESP32-S3 firmware
# ==============================================================================
# Build only: `idf.py build` (after `idf.py set-target esp32s3`). This project
# has never been flashed and firmware/s3/README.md explains why `idf.py
# flash` must never be used against the real device -- see that file before
# running anything against hardware.
cmake_minimum_required(VERSION 3.16)

# byok_proto (frame encoder/decoder for the BYOK Link protocol,
# docs/protocol.md) lives outside this project tree, in firmware/common/,
# shared with a future firmware/pico/ build. EXTRA_COMPONENT_DIRS makes IDF
# scan firmware/common/ for immediate subdirectories that are components --
# it finds firmware/common/byok_proto/ this way. hw_config.h, which sits
# directly in firmware/common/ (not inside its own component subdirectory),
# is NOT picked up by this mechanism -- components/byok_hw_shim/ adds it to
# the include path itself instead (see that component's CMakeLists.txt).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Deliberately NOT "BYOK" -- BOOT_ORIGINAL's stock-partition search
# (main/app_main.c, byok_find_stock_partition()) identifies the vendor's app
# by esp_app_desc_t.project_name == "BYOK" and excludes whichever partition
# is currently running; if our own project() name were also "BYOK" that
# search could match ourselves.
project(byok_mod_s3)
