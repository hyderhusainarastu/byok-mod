# SPDX-License-Identifier: MIT
# byok_nvs -- this firmware's own NVS namespace ("byokmod"), gated behind
# CONFIG_BYOK_NVS_PERSIST_ARMED (default n). See include/byok_nvs.h.
idf_component_register(
    SRCS "byok_nvs.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "nvs_flash" "log"
)
