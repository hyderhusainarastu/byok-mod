# SPDX-License-Identifier: MIT
# ==============================================================================
# byok_proto — BYOK Link protocol v1 frame encoder/decoder
# ==============================================================================
# This component is built two ways:
#
#   1. As an ESP-IDF component, when this CMakeLists.txt is processed inside
#      an IDF build (idf.py build with this directory added to
#      EXTRA_COMPONENT_DIRS, or placed under a components/ tree). REQUIRES
#      is empty — byok_proto.c/.h and byok_crc32.c/.h use only the C
#      standard library (stdint.h, stddef.h, stdbool.h, string.h), no IDF
#      APIs, so it has no component dependencies. Kconfig-free by design:
#      no Kconfig file, so it takes no idf.py menuconfig options and no
#      CONFIG_ symbol needs to exist for it to build.
#
#   2. As a plain static library on the host, via plain clang/cmake with no
#      ESP-IDF present at all — see tests/proto/Makefile, which compiles
#      byok_proto.c and byok_crc32.c directly rather than going through this
#      file. This CMakeLists.txt's else() branch below exists so `cmake` on
#      this directory alone (outside the Makefile) also works, e.g. for an
#      IDE that indexes it as its own project.
#
# idf_component_register is only defined while an IDF build is including
# component CMakeLists.txt files; probing for the command is the standard
# way a component detects "am I inside an IDF build right now".
if(COMMAND idf_component_register)
    idf_component_register(
        SRCS "byok_proto.c" "byok_crc32.c"
        INCLUDE_DIRS "."
        REQUIRES ""
        PRIV_REQUIRES ""
    )
else()
    cmake_minimum_required(VERSION 3.16)
    project(byok_proto C)

    add_library(byok_proto STATIC byok_proto.c byok_crc32.c)
    target_include_directories(byok_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(byok_proto PRIVATE -std=c11 -Wall -Wextra)
endif()
