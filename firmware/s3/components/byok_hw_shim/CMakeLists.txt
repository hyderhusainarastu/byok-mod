# SPDX-License-Identifier: MIT
# byok_hw_shim -- resolves firmware/common/hw_config.h Sec.13 build-blockers
# (see include/byok_hw_shim.h) and re-exports hw_config.h's include path.
#
# firmware/common/ is not itself under an EXTRA_COMPONENT_DIRS tree (it holds
# hw_config.h directly, plus the byok_proto component as its own
# subdirectory) -- so this is the one place that adds it to the include
# path, via a relative INCLUDE_DIRS entry. INCLUDE_DIRS in ESP-IDF is always
# public, so any component that REQUIRES byok_hw_shim also gets hw_config.h
# on its include path transitively.
idf_component_register(
    SRCS "byok_hw_shim.c"
    INCLUDE_DIRS "include" "../../../common"
    PRIV_REQUIRES "log"
)
