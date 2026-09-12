# SPDX-License-Identifier: MIT
# byok_note -- device-local STATIC NOTE storage + rendering (byok_modes
# BYOK_MODE_STATIC_NOTE, protocol SET_NOTE). See include/byok_note.h.
idf_component_register(
    SRCS "byok_note.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "byok_display" "byok_modes" "byok_nvs" "log"
)
