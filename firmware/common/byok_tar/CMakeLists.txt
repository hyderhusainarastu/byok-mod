# SPDX-License-Identifier: MIT
# ==============================================================================
# byok_tar — minimal POSIX ustar header parser (see byok_tar.h)
# ==============================================================================
# Same dual-mode CMakeLists.txt pattern as firmware/common/byok_proto (see
# that component's own header comment for the full rationale): built as an
# ESP-IDF component when idf_component_register is available (picked up via
# EXTRA_COMPONENT_DIRS from firmware/s3/CMakeLists.txt, consumed by
# components/byok_sd_updater), and as a plain static library otherwise (used
# directly by tests/tar/Makefile on the host, no ESP-IDF required). No IDF
# APIs, no Kconfig, pure C stdlib (stdint.h/stddef.h/stdbool.h/string.h).
if(COMMAND idf_component_register)
    idf_component_register(
        SRCS "byok_tar.c"
        INCLUDE_DIRS "."
        REQUIRES ""
        PRIV_REQUIRES ""
    )
else()
    cmake_minimum_required(VERSION 3.16)
    project(byok_tar C)

    add_library(byok_tar STATIC byok_tar.c)
    target_include_directories(byok_tar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(byok_tar PRIVATE -std=c11 -Wall -Wextra)
endif()
